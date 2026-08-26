/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Derives the root disk's LUKS passphrase and writes it (raw, 32 bytes) to
 * stdout, for use as a systemd repart.d KeyFile= connect-socket provider
 * (see root-passphrase.socket/.service).
 *
 * On Raspberry Pi hardware: HMAC-SHA256(static context + root disk's own
 * hardware id) computed by the firmware mailbox via /dev/rpi-crypto-passphrase
 * (see mkosi.profiles/disk/rpi-crypto-passphrase.c), which also locks the
 * OTP key for the rest of the boot as a side effect.
 *
 * Under QEMU (mkosi vm), there is no RPi firmware mailbox, so
 * /dev/rpi-crypto-passphrase never appears (the kernel module's init fails
 * with -ENODEV there); fall back to the static "cryptsetup.passphrase" dev
 * credential set in mkosi.conf's [Runtime] Credentials=.
 */

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rpi-crypto-passphrase.h"

#define RPI_CRYPTO_DEV "/dev/rpi-crypto-passphrase"
#define STATIC_CONTEXT "rpi-mkosi/root-luks-passphrase:"
#define LOADER_PARTUUID_EFIVAR \
	"/sys/firmware/efi/efivars/LoaderDevicePartUUID-4a67b082-0a4c-41cf-b6c7-440b29bb8c4f"

static int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n <= 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

/* efivarfs entries are 4 bytes of attributes followed by UTF-16LE data. */
static int read_loader_partuuid(char *out, size_t outsize)
{
	unsigned char buf[256];
	ssize_t n;
	size_t j = 0;

	int fd = open(LOADER_PARTUUID_EFIVAR, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n <= 4)
		return -1;

	for (ssize_t i = 4; i + 1 < n && j + 1 < outsize; i += 2) {
		if (buf[i] == 0)
			break;
		out[j++] = (char)tolower(buf[i]);
	}
	out[j] = '\0';
	return j ? 0 : -1;
}

/* nvme0n1p1 -> nvme0n1, mmcblk0p1 -> mmcblk0, sda1 -> sda */
static void disk_name_from_partition(char *name)
{
	size_t len = strlen(name);

	while (len > 0 && isdigit((unsigned char)name[len - 1]))
		len--;
	if (len > 1 && name[len - 1] == 'p' && isdigit((unsigned char)name[len - 2]))
		len--;
	name[len] = '\0';
}

static int resolve_root_disk(char *diskname, size_t diskname_size)
{
	char partuuid[64];
	char path[128];
	char link[256];
	ssize_t n;
	char *base;

	if (read_loader_partuuid(partuuid, sizeof(partuuid)) != 0)
		return -1;

	snprintf(path, sizeof(path), "/dev/disk/by-partuuid/%s", partuuid);
	n = readlink(path, link, sizeof(link) - 1);
	if (n < 0)
		return -1;
	link[n] = '\0';

	base = strrchr(link, '/');
	base = base ? base + 1 : link;
	snprintf(diskname, diskname_size, "%s", base);
	disk_name_from_partition(diskname);
	return 0;
}

static int read_trimmed(const char *path, char *buf, size_t bufsize)
{
	ssize_t n;
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, bufsize - 1);
	close(fd);
	if (n <= 0)
		return -1;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
		n--;
	buf[n] = '\0';
	return n ? 0 : -1;
}

static int read_disk_hardware_id(const char *diskname, char *idbuf, size_t idbuf_size)
{
	char path[128];

	snprintf(path, sizeof(path), "/sys/block/%s/device/cid", diskname);
	if (read_trimmed(path, idbuf, idbuf_size) == 0)
		return 0;

	snprintf(path, sizeof(path), "/sys/block/%s/device/serial", diskname);
	return read_trimmed(path, idbuf, idbuf_size);
}

static int derive_via_firmware(void)
{
	struct rpi_crypto_passphrase_req req = { 0 };
	char diskname[64];
	char hwid[256];
	int fd, msglen;

	fd = open(RPI_CRYPTO_DEV, O_RDWR);
	if (fd < 0)
		return -1;

	if (resolve_root_disk(diskname, sizeof(diskname)) != 0) {
		fprintf(stderr, "rpi-root-passphrase: failed to resolve root disk\n");
		close(fd);
		return -1;
	}

	if (read_disk_hardware_id(diskname, hwid, sizeof(hwid)) != 0) {
		fprintf(stderr, "rpi-root-passphrase: no hardware id for %s\n", diskname);
		close(fd);
		return -1;
	}

	msglen = snprintf((char *)req.message, sizeof(req.message), "%s%s",
			   STATIC_CONTEXT, hwid);
	if (msglen < 0 || (size_t)msglen >= sizeof(req.message)) {
		close(fd);
		return -1;
	}
	req.message_len = (u32)msglen;

	if (ioctl(fd, RPI_CRYPTO_PASSPHRASE_IOC_HMAC, &req) != 0) {
		perror("rpi-root-passphrase: ioctl");
		close(fd);
		return -1;
	}
	close(fd);

	return write_all(STDOUT_FILENO, req.hmac, sizeof(req.hmac));
}

/* mkosi vm / mkosi qemu: no firmware mailbox, use the dev-only static
 * credential set via [Runtime] Credentials=cryptsetup.passphrase in
 * mkosi.conf, imported into $CREDENTIALS_DIRECTORY by the service unit. */
static int derive_via_dev_credential(void)
{
	const char *cred_dir = getenv("CREDENTIALS_DIRECTORY");
	char path[PATH_MAX];
	char buf[4096];
	ssize_t n;
	int fd, ret;

	if (!cred_dir) {
		fprintf(stderr, "rpi-root-passphrase: no crypto device and no CREDENTIALS_DIRECTORY\n");
		return -1;
	}

	snprintf(path, sizeof(path), "%s/cryptsetup.passphrase", cred_dir);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("rpi-root-passphrase: open credential");
		return -1;
	}

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (write_all(STDOUT_FILENO, buf, n) != 0) {
			close(fd);
			return -1;
		}
	}
	ret = n < 0 ? -1 : 0;
	close(fd);
	return ret;
}

int main(void)
{
	if (access(RPI_CRYPTO_DEV, F_OK) == 0)
		return derive_via_firmware() == 0 ? 0 : 1;

	return derive_via_dev_credential() == 0 ? 0 : 1;
}
