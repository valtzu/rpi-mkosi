/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Long-running initrd daemon that derives the root disk's LUKS passphrase
 * once and serves it (raw bytes) over an AF_UNIX connect-socket to both
 * in-initrd consumers, for the life of the initrd:
 *   - systemd-repart.service         via repart.d KeyFile=<sock>
 *   - systemd-cryptsetup@root.service via /run/cryptsetup-keys.d/root.key key
 *     auto-discovery (systemd-cryptsetup connects to a socket found there)
 *
 * The socket therefore lives at /run/cryptsetup-keys.d/root.key: that is the
 * one path systemd-cryptsetup probes with READ_FULL_FILE_CONNECT_SOCKET for a
 * volume named "root" without any unit configuration.
 *
 * Why a daemon and not a one-shot:
 *  - The firmware computes the HMAC only once per boot (it locks the OTP key
 *    as a side effect - see rpi-crypto-passphrase.c), yet the passphrase is
 *    needed more than once: systemd-repart deactivates the dm-crypt device
 *    after formatting, so systemd-cryptsetup has to reopen it; and on every
 *    later boot repart is a no-op and only systemd-cryptsetup needs it.
 *  - The key must never touch a filesystem, so it can't be cached to a file.
 *    Instead it lives only in this process's mlockall(2)'d pages, is wiped on
 *    SIGTERM, and root-passphrase-cleanup.service stops us before switch-root.
 *  - Both consumers connect more than once (repart reopens after formatting,
 *    cryptsetup retries), so a persistent accept() loop serving the cached
 *    derivation is what makes it work.
 *
 * The passphrase is HMAC-SHA256(static context + this board's rpi-machine-id),
 * computed by the firmware mailbox via /dev/rpi-crypto-passphrase. The
 * rpi-machine-id is a 32-hex-char per-device identifier the Raspberry Pi
 * bootloader derives from the OTP serial (+ MAC on Pi 4/5) and publishes in
 * the device tree at /chosen/rpi-machine-id - stable, unique, and readable
 * before anything is unlocked. Under QEMU rpi-fw-mock synthesises the
 * same node. If neither is present (non-Pi, or no mock) we fall back to the
 * static "cryptsetup.passphrase" dev credential from mkosi.conf's [Runtime]
 * Credentials=.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "rpi-crypto-passphrase.h"

#define RPI_CRYPTO_DEV "/dev/rpi-crypto-passphrase"
#define STATIC_CONTEXT "rpi-mkosi/root-luks-passphrase:"
#define RPI_MACHINE_ID_DT "/sys/firmware/devicetree/base/chosen/rpi-machine-id"

static unsigned char g_key[64];
static size_t g_keylen;

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

/*
 * The board's rpi-machine-id: a device-tree string property, so the sysfs file
 * is the 32 hex chars plus a trailing NUL. strlen() therefore stops at 32.
 */
static int read_rpi_machine_id(char *out, size_t outsize)
{
	char buf[64];
	ssize_t n;
	size_t i;
	int fd;

	if (outsize < 33)
		return -1;

	fd = open(RPI_MACHINE_ID_DT, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 32)
		return -1;
	buf[n] = '\0';
	buf[strcspn(buf, "\r\n")] = '\0';

	if (strlen(buf) != 32)
		return -1;
	for (i = 0; i < 32; i++)
		if (!isxdigit((unsigned char)buf[i]))
			return -1;

	memcpy(out, buf, 33);
	return 0;
}

static ssize_t derive_via_firmware(unsigned char *out, size_t outsize)
{
	struct rpi_crypto_passphrase_req req = { 0 };
	char machine_id[33];
	int fd, msglen;

	if (outsize < sizeof(req.hmac))
		return -1;

	if (read_rpi_machine_id(machine_id, sizeof(machine_id)) != 0) {
		fprintf(stderr, "rpi-root-passphrase: no valid %s\n", RPI_MACHINE_ID_DT);
		return -1;
	}

	fd = open(RPI_CRYPTO_DEV, O_RDWR);
	if (fd < 0)
		return -1;

	msglen = snprintf((char *)req.message, sizeof(req.message), "%s%s",
			   STATIC_CONTEXT, machine_id);
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

/* Minimal sd_notify(3): a datagram to $NOTIFY_SOCKET, no libsystemd. */
static void notify_ready(void)
{
	const char *ns = getenv("NOTIFY_SOCKET");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd;

	if (!ns || (ns[0] != '/' && ns[0] != '@') || strlen(ns) >= sizeof(sa.sun_path))
		return;

	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	memcpy(sa.sun_path, ns, strlen(ns));
	if (sa.sun_path[0] == '@')
		sa.sun_path[0] = '\0';   /* abstract namespace */
	sendto(fd, "READY=1", 7, MSG_NOSIGNAL, (struct sockaddr *)&sa,
	       offsetof(struct sockaddr_un, sun_path) + strlen(ns));
	close(fd);
}

/* Wipe the derived key from our (locked) memory before exiting so it can't be
 * scraped from freed pages after switch-root. */
static void on_term(int sig)
{
	volatile unsigned char *p = g_key;
	size_t i;

	(void)sig;
	for (i = 0; i < sizeof(g_key); i++)
		p[i] = 0;
	_exit(0);
}

int main(void)
{
	struct sigaction sa = { 0 };
	ssize_t keylen;
	int listen_fd;

	/* A broken connect-socket write must surface as a clean write() error,
	 * not silently kill us. */
	signal(SIGPIPE, SIG_IGN);

	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	listen_fd = activation_socket_fd();
	if (listen_fd < 0) {
		fprintf(stderr, "rpi-root-passphrase: not socket-activated\n");
		return 1;
	}

	/* Keep the derived key out of swap and off any core dump. */
	mlockall(MCL_CURRENT | MCL_FUTURE);

	if (access(RPI_CRYPTO_DEV, F_OK) == 0)
		keylen = derive_via_firmware(g_key, sizeof(g_key));
	else
		keylen = derive_via_dev_credential(g_key, sizeof(g_key));

	if (keylen <= 0) {
		fprintf(stderr, "rpi-root-passphrase: derivation failed\n");
		return 1;
	}
	g_keylen = keylen;
	fprintf(stderr, "rpi-root-passphrase: derived %zd-byte passphrase, serving\n",
		keylen);

	/* Type=notify: only now is the key ready to be served, so units ordered
	 * After=root-passphrase.service can safely connect. */
	notify_ready();

	for (;;) {
		int conn = accept(listen_fd, NULL, NULL);

		if (conn < 0) {
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			perror("rpi-root-passphrase: accept");
			return 1;
		}
		if (write_all(conn, g_key, g_keylen) == 0)
			fprintf(stderr, "rpi-root-passphrase: served passphrase\n");
		close(conn);
	}
}
