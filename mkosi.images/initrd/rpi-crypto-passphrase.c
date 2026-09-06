// SPDX-License-Identifier: GPL-2.0
/*
 * rpi-crypto-passphrase: minimal bridge to the Raspberry Pi firmware's
 * mailbox crypto service (key generation + HMAC-SHA256 + key-status lock),
 * used to derive the root disk's LUKS passphrase without ever exposing the
 * OTP private key itself to Linux, and without keeping the ability to derive
 * it a second time within the same boot.
 *
 * On the first release boot the device key slot is still blank; the module
 * generates the key (one-time OTP write) and locks it before the first HMAC.
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

/*
 * Firmware crypto key id. rpi-otp-private-key's first keystore slot - the one
 * its examples provision with "--key-id 1" - is id 1, not 0; on a board whose
 * key sits there, id 0 makes the firmware reject the HMAC request outright
 * (mailbox status 0x80000001). Override with rpi-crypto-passphrase.key_id= if
 * a board is provisioned elsewhere.
 */
static u32 key_id = 1;
module_param(key_id, uint, 0444);
MODULE_PARM_DESC(key_id, "firmware OTP private-key id to HMAC with (default 1)");

/*
 * Generate the device key when the slot is still blank. This is a one-time OTP
 * write, so it only happens where the firmware still permits it: release images
 * ship config.txt lock_device_key_write=0, dev images ship =1 (GEN_LOCKED) and
 * never provision. Set provision=0 to opt out even on release.
 */
static bool provision = true;
module_param(provision, bool, 0444);
MODULE_PARM_DESC(provision, "generate the device key if the slot is blank and unlocked (default Y)");

#define TAG_GET_CRYPTO_KEY_STATUS    0x00030090
#define TAG_GET_CRYPTO_HMAC_SHA256   0x00030092
#define TAG_GET_CRYPTO_GEN_ECDSA_KEY 0x00030095
#define TAG_SET_CRYPTO_KEY_STATUS    0x00038090

#define VC_MAILBOX_ERROR 0x80000000

#define ARM_CRYPTO_KEY_STATUS_TYPE_DEVICE_PRIVATE_KEY (1 << 0)
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

struct rpi_fw_gen_key_payload {
	u32 flags;
	u32 key_id;
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
		.key_id = key_id,
		.status = RPI_CRYPTO_LOCK_ALL,
	};
	int ret;

	ret = rpi_firmware_property(rpi_fw, TAG_SET_CRYPTO_KEY_STATUS,
				     &lock_req, sizeof(lock_req));
	if (ret || (lock_req.status & VC_MAILBOX_ERROR))
		pr_err("rpi-crypto-passphrase: failed to lock OTP key (ret=%d status=0x%08x)\n",
		       ret, lock_req.status);
}

/*
 * Converge the key slot before deriving:
 *  - blank slot, generation still permitted -> generate the ECDSA P-256 device
 *    key (one-time OTP write; only reachable on release images, see the
 *    provision param);
 *  - populated slot whose generation/usage aren't locked yet -> lock them so the
 *    key can never be regenerated or repurposed.
 * Anything unexpected is logged and left alone - the HMAC step below surfaces
 * the real error if the slot turns out to be unusable.
 */
static void rpi_crypto_provision_key(void)
{
	u32 status = key_id;
	int ret;

	ret = rpi_firmware_property(rpi_fw, TAG_GET_CRYPTO_KEY_STATUS,
				   &status, sizeof(status));
	if (ret || (status & VC_MAILBOX_ERROR)) {
		pr_warn("rpi-crypto-passphrase: key status unavailable (ret=%d status=0x%08x), skipping provisioning\n",
			ret, status);
		return;
	}

	if (!(status & ARM_CRYPTO_KEY_STATUS_TYPE_DEVICE_PRIVATE_KEY)) {
		struct rpi_fw_gen_key_payload gen = { .key_id = key_id };

		if (!provision)
			return;
		if (status & ARM_CRYPTO_KEY_STATUS_GEN_LOCKED) {
			pr_info("rpi-crypto-passphrase: slot %u blank but key generation is locked\n",
				key_id);
			return;
		}

		ret = rpi_firmware_property(rpi_fw, TAG_GET_CRYPTO_GEN_ECDSA_KEY,
					   &gen, sizeof(gen));
		if (ret)
			pr_err("rpi-crypto-passphrase: key generation failed (ret=%d)\n", ret);
		else
			pr_info("rpi-crypto-passphrase: generated device key in slot %u\n", key_id);
		return;
	}

	if ((status & (ARM_CRYPTO_KEY_STATUS_GEN_LOCKED | ARM_CRYPTO_KEY_STATUS_USAGE_LOCKED)) !=
	    (ARM_CRYPTO_KEY_STATUS_GEN_LOCKED | ARM_CRYPTO_KEY_STATUS_USAGE_LOCKED)) {
		struct rpi_fw_key_status_payload lock_req = {
			.key_id = key_id,
			.status = ARM_CRYPTO_KEY_STATUS_GEN_LOCKED |
				  ARM_CRYPTO_KEY_STATUS_USAGE_LOCKED,
		};

		ret = rpi_firmware_property(rpi_fw, TAG_SET_CRYPTO_KEY_STATUS,
					   &lock_req, sizeof(lock_req));
		if (ret || (lock_req.status & VC_MAILBOX_ERROR))
			pr_err("rpi-crypto-passphrase: failed to write-lock key (ret=%d status=0x%08x)\n",
			       ret, lock_req.status);
	}
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

	rpi_crypto_provision_key();

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
	payload->req.key_id = key_id;
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
	int ret;

	fw_node = of_find_compatible_node(NULL, NULL, "raspberrypi,bcm2835-firmware");
	if (!fw_node)
		return -ENODEV;

	rpi_fw = rpi_firmware_get(fw_node);
	of_node_put(fw_node);
	if (!rpi_fw)
		return -ENODEV;

	ret = misc_register(&rpi_crypto_passphrase_miscdev);
	if (ret) {
		rpi_firmware_put(rpi_fw);
		rpi_fw = NULL;
	}
	return ret;
}

static void __exit rpi_crypto_passphrase_exit(void)
{
	misc_deregister(&rpi_crypto_passphrase_miscdev);
	rpi_firmware_put(rpi_fw);
}

module_init(rpi_crypto_passphrase_init);
module_exit(rpi_crypto_passphrase_exit);

MODULE_DESCRIPTION("Raspberry Pi firmware mailbox crypto bridge for root disk passphrase derivation");
MODULE_LICENSE("GPL");
