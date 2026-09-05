/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Serves the root disk's LUKS passphrase (raw bytes) over an AF_UNIX
 * connect-socket, for both consumers in the initrd:
 *   - systemd-repart.service        via repart.d KeyFile=/run/root-passphrase.sock
 *   - systemd-cryptsetup@root.service via LoadCredential=cryptsetup.passphrase:<sock>
 *
 * The firmware can derive the HMAC only once per boot (it locks the OTP key
 * as a side effect - see rpi-crypto-passphrase.c), but the passphrase is
 * needed more than once per boot: systemd-repart deactivates the dm-crypt
 * device after formatting it, so systemd-cryptsetup has to reopen it. So the
 * first derivation is cached in a tmpfs file (/run/root-passphrase.key,
 * root-only) and every later connection is answered from that cache.
 * root-passphrase-cleanup.service removes the cache before initrd-switch-root
 * so it never reaches the booted system (initrd /run is carried over).
 *
 * On Raspberry Pi hardware the passphrase is HMAC-SHA256(static context +
 * root disk's own hardware id) computed by the firmware mailbox via
 * /dev/rpi-crypto-passphrase. Under QEMU (mkosi vm) there is no firmware
 * mailbox and the device never appears; fall back to the static
 * "cryptsetup.passphrase" dev credential set in mkosi.conf's [Runtime]
 * Credentials= and imported by the service unit.
 */

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rpi-crypto-passphrase.h"

#define RPI_CRYPTO_DEV "/dev/rpi-crypto-passphrase"
#define KEY_CACHE "/run/root-passphrase.key"
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

/*
 * udev's ID_SERIAL_SHORT is the one hardware id that resolves across every
 * transport we boot from: the raw MMC serial from the CID for an SD card, the
 * ATA/NVMe serial for a disk, and the USB descriptor serial for a stick - the
 * bare /sys/block/<disk>/device/serial only exists for some of those. Prefer
 * ID_SERIAL_SHORT (raw serial, stable across udev versions); fall back to the
 * composite ID_SERIAL only if the short form is absent.
 */
static int read_disk_hardware_id(const char *diskname, char *idbuf, size_t idbuf_size)
{
	char cmd[256];
	char line[512];
	char id_serial[256] = "";
	char id_serial_short[256] = "";
	FILE *p;

	snprintf(cmd, sizeof(cmd),
		 "udevadm info --query=property --name=/dev/%s 2>/dev/null", diskname);
	p = popen(cmd, "r");
	if (!p)
		return -1;

	while (fgets(line, sizeof(line), p)) {
		line[strcspn(line, "\n")] = '\0';
		if (!strncmp(line, "ID_SERIAL_SHORT=", 16))
			snprintf(id_serial_short, sizeof(id_serial_short), "%.*s",
				 (int)sizeof(id_serial_short) - 1, line + 16);
		else if (!strncmp(line, "ID_SERIAL=", 10))
			snprintf(id_serial, sizeof(id_serial), "%.*s",
				 (int)sizeof(id_serial) - 1, line + 10);
	}
	pclose(p);

	if (id_serial_short[0])
		snprintf(idbuf, idbuf_size, "%s", id_serial_short);
	else if (id_serial[0])
		snprintf(idbuf, idbuf_size, "%s", id_serial);
	else
		return -1;

	return 0;
}

static ssize_t derive_via_firmware(unsigned char *out, size_t outsize)
{
	struct rpi_crypto_passphrase_req req = { 0 };
	char diskname[64];
	char hwid[256];
	int fd, msglen;

	if (outsize < sizeof(req.hmac))
		return -1;

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

	memcpy(out, req.hmac, sizeof(req.hmac));
	return sizeof(req.hmac);
}

/* mkosi vm / mkosi qemu: no firmware mailbox, use the dev-only static
 * credential set via [Runtime] Credentials=cryptsetup.passphrase in
 * mkosi.conf, imported into $CREDENTIALS_DIRECTORY by the service unit. */
static ssize_t derive_via_dev_credential(unsigned char *out, size_t outsize)
{
	const char *cred_dir = getenv("CREDENTIALS_DIRECTORY");
	char path[PATH_MAX];
	ssize_t total = 0, n;
	int fd;

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

	while (total < (ssize_t)outsize &&
	       (n = read(fd, out + total, outsize - total)) > 0)
		total += n;
	close(fd);

	return total > 0 ? total : -1;
}

static ssize_t read_key_cache(unsigned char *out, size_t outsize)
{
	ssize_t total = 0, n;
	int fd = open(KEY_CACHE, O_RDONLY);

	if (fd < 0)
		return -1;
	while (total < (ssize_t)outsize &&
	       (n = read(fd, out + total, outsize - total)) > 0)
		total += n;
	close(fd);
	return total > 0 ? total : -1;
}

/* Best effort: a concurrent instance winning the O_EXCL race is fine, we
 * still hold the freshly derived key in our own buffer. */
static void write_key_cache(const unsigned char *key, size_t keylen)
{
	int fd = open(KEY_CACHE, O_WRONLY | O_CREAT | O_EXCL, 0600);

	if (fd < 0)
		return;
	write_all(fd, key, keylen);
	close(fd);
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
	unsigned char key[4096];
	ssize_t keylen;
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

	keylen = read_key_cache(key, sizeof(key));
	if (keylen < 0) {
		if (access(RPI_CRYPTO_DEV, F_OK) == 0)
			keylen = derive_via_firmware(key, sizeof(key));
		else
			keylen = derive_via_dev_credential(key, sizeof(key));
		if (keylen > 0)
			write_key_cache(key, keylen);
	}

	if (keylen <= 0) {
		close(conn);
		return 1;
	}

	ret = write_all(conn, key, keylen);
	close(conn);
	return ret == 0 ? 0 : 1;
}
