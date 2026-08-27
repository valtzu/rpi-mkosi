/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Derives the root disk's LUKS passphrase and writes it (raw, 32 bytes) to
 * stdout, for use as a systemd repart.d KeyFile= connect-socket provider
 * (see root-passphrase.socket/.service).
 *
 * On Raspberry Pi hardware: HMAC-SHA256(static context + root disk's own
 * hardware id) computed by the firmware mailbox via /dev/rpi-crypto-passphrase
 * (see rpi-crypto-passphrase.c), which also locks the
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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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
		if (n < 0) {
			perror("rpi-root-passphrase: write");
			return -1;
		}
		if (n == 0) {
			fprintf(stderr, "rpi-root-passphrase: write: short write, peer closed?\n");
			return -1;
		}
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

/*
 * LoaderDevicePartUUID identifies the ESP systemd-boot was loaded from, not
 * the root partition - but ESP and root are sibling partitions on the same
 * physical disk in this image (see repart.d/00-esp.conf, 30-root.conf), and
 * a disk name (not a specific partition) is all the caller needs, so this
 * still resolves to the right place.
 */
static int resolve_boot_disk(char *diskname, size_t diskname_size)
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
	snprintf(diskname, diskname_size, "%.*s", (int)diskname_size - 1, base);
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

static int derive_via_firmware(int out_fd)
{
	struct rpi_crypto_passphrase_req req = { 0 };
	char diskname[64];
	char hwid[256];
	int fd, msglen;

	fd = open(RPI_CRYPTO_DEV, O_RDWR);
	if (fd < 0)
		return -1;

	if (resolve_boot_disk(diskname, sizeof(diskname)) != 0) {
		fprintf(stderr, "rpi-root-passphrase: failed to resolve boot disk\n");
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

	return write_all(out_fd, req.hmac, sizeof(req.hmac));
}

/* mkosi vm / mkosi qemu: no firmware mailbox, use the dev-only static
 * credential set via [Runtime] Credentials=cryptsetup.passphrase in
 * mkosi.conf, imported into $CREDENTIALS_DIRECTORY by the service unit. */
static int derive_via_dev_credential(int out_fd)
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
		if (write_all(out_fd, buf, n) != 0) {
			close(fd);
			return -1;
		}
	}
	ret = n < 0 ? -1 : 0;
	close(fd);
	return ret;
}

/* Standard systemd socket-activation protocol (sd_listen_fds(3)), read
 * directly off the environment so we don't need to link libsystemd. */
#define SD_LISTEN_FDS_START 3

static int activation_socket_fd(void)
{
	const char *pid = getenv("LISTEN_PID");
	const char *fds = getenv("LISTEN_FDS");

	if (!pid || !fds || atoi(pid) != (int)getpid() || atoi(fds) != 1)
		return -1;
	return SD_LISTEN_FDS_START;
}

int main(void)
{
	int listen_fd, conn, ret;

	/* A broken connect-socket write must surface as a clean write() error,
	 * not silently kill us. */
	signal(SIGPIPE, SIG_IGN);

	listen_fd = activation_socket_fd();
	if (listen_fd < 0) {
		fprintf(stderr, "rpi-root-passphrase: not socket-activated\n");
		return 1;
	}

	/*
	 * With Accept=no, socket activation hands us the *listening* socket,
	 * not a pre-accepted connection (that only happens for per-connection
	 * Accept=yes instances) - we have to accept() it ourselves.
	 */
	conn = accept(listen_fd, NULL, NULL);
	if (conn < 0) {
		perror("rpi-root-passphrase: accept");
		return 1;
	}

	if (access(RPI_CRYPTO_DEV, F_OK) == 0)
		ret = derive_via_firmware(conn);
	else
		ret = derive_via_dev_credential(conn);

	close(conn);
	return ret == 0 ? 0 : 1;
}
