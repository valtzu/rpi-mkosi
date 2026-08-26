/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ioctl protocol shared between rpi-crypto-passphrase.c (kernel module)
 * and rpi-root-passphrase.c (userspace helper, mkosi.images/initrd).
 * Keep both sides in sync when changing this file.
 */

#ifndef RPI_CRYPTO_PASSPHRASE_H
#define RPI_CRYPTO_PASSPHRASE_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#endif

#define RPI_CRYPTO_PASSPHRASE_HMAC_MSG_MAX_SIZE 2048

struct rpi_crypto_passphrase_req {
	u8 message[RPI_CRYPTO_PASSPHRASE_HMAC_MSG_MAX_SIZE];
	u32 message_len;
	u8 hmac[32];
};

#define RPI_CRYPTO_PASSPHRASE_IOC_MAGIC 0xC0
#define RPI_CRYPTO_PASSPHRASE_IOC_HMAC \
	_IOWR(RPI_CRYPTO_PASSPHRASE_IOC_MAGIC, 0, struct rpi_crypto_passphrase_req)

#endif /* RPI_CRYPTO_PASSPHRASE_H */
