// SPDX-License-Identifier: GPL-2.0
/*
 * rpi-crypto-passphrase: minimal bridge to the Raspberry Pi firmware's
 * mailbox crypto service (HMAC-SHA256 + key-status lock), used to derive
 * the root disk's LUKS passphrase without ever exposing the OTP private
 * key itself to Linux, and without keeping the ability to derive it a
 * second time within the same boot.
 *
 * Mailbox tag numbers and key-status lock bits are taken from
 * raspberrypi/utils rpifwcrypto.h (BSD-3-Clause), which documents the
 * firmware's crypto property-channel protocol; we talk to rpi_firmware
 * directly instead of going through /dev/vcio (downstream-only, not
 * built here) since rpi_firmware_property() is exported by the upstream
 * in-tree raspberrypi-firmware driver already used by nvmem-raspberrypi-otp.
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#include "rpi-crypto-passphrase.h"

/* Pi4 has a single OTP private-key slot (offset 0); see rpi-otp-private-key. */
#define RPI_FW_CRYPTO_KEY_ID 0

#define TAG_GET_CRYPTO_HMAC_SHA256 0x00030092
#define TAG_SET_CRYPTO_KEY_STATUS  0x00038090

#define VC_MAILBOX_ERROR 0x80000000

#define ARM_CRYPTO_KEY_STATUS_READ_LOCKED  (1 << 8)
#define ARM_CRYPTO_KEY_STATUS_GEN_LOCKED   (1 << 9)
#define ARM_CRYPTO_KEY_STATUS_SIGN_LOCKED  (1 << 10)
#define ARM_CRYPTO_KEY_STATUS_HMAC_LOCKED  (1 << 11)
#define ARM_CRYPTO_KEY_STATUS_USAGE_LOCKED (1 << 12)
#define RPI_CRYPTO_LOCK_ALL ( \
	ARM_CRYPTO_KEY_STATUS_READ_LOCKED | \
	ARM_CRYPTO_KEY_STATUS_GEN_LOCKED | \
	ARM_CRYPTO_KEY_STATUS_SIGN_LOCKED | \
	ARM_CRYPTO_KEY_STATUS_HMAC_LOCKED | \
	ARM_CRYPTO_KEY_STATUS_USAGE_LOCKED)

struct rpi_fw_hmac_payload {
	union {
		struct {
			u32 flags;
			u32 key_id;
			u32 length;
			u8 message[RPI_CRYPTO_PASSPHRASE_HMAC_MSG_MAX_SIZE];
		} req;
		struct {
			u32 status;
			u32 length;
			u8 hmac[32];
		} resp;
	};
};

struct rpi_fw_key_status_payload {
	u32 key_id;
	u32 status;
};

static struct rpi_firmware *rpi_fw;

/*
 * Set as soon as the first ioctl is attempted, before touching the
 * firmware, so a second concurrent/later caller is rejected immediately
 * rather than merely failing at the (also locked) firmware level.
 */
static atomic_t rpi_crypto_used = ATOMIC_INIT(0);

static void rpi_crypto_lock_key(void)
{
	struct rpi_fw_key_status_payload lock_req = {
		.key_id = RPI_FW_CRYPTO_KEY_ID,
		.status = RPI_CRYPTO_LOCK_ALL,
	};

	rpi_firmware_property(rpi_fw, TAG_SET_CRYPTO_KEY_STATUS,
			       &lock_req, sizeof(lock_req));
}

static long rpi_crypto_passphrase_ioctl(struct file *file, unsigned int cmd,
					 unsigned long arg)
{
	struct rpi_crypto_passphrase_req __user *uarg = (void __user *)arg;
	struct rpi_crypto_passphrase_req *kreq;
	struct rpi_fw_hmac_payload *payload;
	int ret;

	if (cmd != RPI_CRYPTO_PASSPHRASE_IOC_HMAC)
		return -ENOTTY;

	/* One HMAC derivation per boot, no exceptions. */
	if (atomic_cmpxchg(&rpi_crypto_used, 0, 1) != 0)
		return -EPERM;

	kreq = kzalloc(sizeof(*kreq), GFP_KERNEL);
	payload = kzalloc(sizeof(*payload), GFP_KERNEL);
	if (!kreq || !payload) {
		ret = -ENOMEM;
		goto out_lock;
	}

	if (copy_from_user(kreq, uarg, sizeof(*kreq))) {
		ret = -EFAULT;
		goto out_lock;
	}

	if (kreq->message_len > sizeof(kreq->message)) {
		ret = -EINVAL;
		goto out_lock;
	}

	payload->req.flags = 0;
	payload->req.key_id = RPI_FW_CRYPTO_KEY_ID;
	payload->req.length = kreq->message_len;
	memcpy(payload->req.message, kreq->message, kreq->message_len);

	ret = rpi_firmware_property(rpi_fw, TAG_GET_CRYPTO_HMAC_SHA256,
				     payload, sizeof(*payload));
	if (ret)
		goto out_lock;

	if (payload->resp.status & VC_MAILBOX_ERROR) {
		ret = -EIO;
		goto out_lock;
	}

	memcpy(kreq->hmac, payload->resp.hmac, sizeof(kreq->hmac));

	if (copy_to_user(uarg, kreq, sizeof(*kreq)))
		ret = -EFAULT;

out_lock:
	/*
	 * Lock regardless of the outcome above: a failed attempt must not
	 * be retriable either, since retrying would defeat the point of
	 * the one-shot guard.
	 */
	rpi_crypto_lock_key();
	kfree(payload);
	kfree(kreq);
	return ret;
}

static const struct file_operations rpi_crypto_passphrase_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = rpi_crypto_passphrase_ioctl,
};

static struct miscdevice rpi_crypto_passphrase_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "rpi-crypto-passphrase",
	.fops = &rpi_crypto_passphrase_fops,
	.mode = 0600,
};

static int __init rpi_crypto_passphrase_init(void)
{
	struct device_node *fw_node;

	fw_node = of_find_compatible_node(NULL, NULL, "raspberrypi,bcm2835-firmware");
	if (!fw_node)
		return -ENODEV;

	rpi_fw = rpi_firmware_get(fw_node);
	of_node_put(fw_node);
	if (!rpi_fw)
		return -ENODEV;

	return misc_register(&rpi_crypto_passphrase_miscdev);
}

static void __exit rpi_crypto_passphrase_exit(void)
{
	misc_deregister(&rpi_crypto_passphrase_miscdev);
}

module_init(rpi_crypto_passphrase_init);
module_exit(rpi_crypto_passphrase_exit);

MODULE_DESCRIPTION("Raspberry Pi firmware mailbox crypto bridge for root disk passphrase derivation");
MODULE_LICENSE("GPL");
