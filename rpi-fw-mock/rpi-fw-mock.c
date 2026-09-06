// SPDX-License-Identifier: GPL-2.0
/*
 * rpi-fw-mock - fake the Raspberry Pi firmware mailbox crypto service so
 * rpi-crypto-passphrase can be exercised under plain QEMU (`mkosi vm`, the
 * `virt` machine), which has no BCM2711 mailbox at all - not just missing
 * property tags, the whole "raspberrypi,bcm2835-firmware" node is absent, so
 * rpi_firmware_crypto_passphrase_init() bails at -ENODEV before any property
 * call is ever made.
 *
 * It ships in every image but load()s only when ALL of these hold (see
 * mock_guard()): running in an initrd, this module's name in an
 * rd.modules_load= / modules_load= list on the kernel command line, and no real
 * raspberrypi,bcm2835-firmware device-tree node present. So a booted system
 * can't modprobe it in, and it never intercepts anything on real hardware.
 *
 * The mock is entirely kprobe-based; it does not register a mailbox controller
 * or a firmware platform device. The call sites of the in-tree
 * raspberrypi-firmware driver are shimmed:
 *
 *   of_find_compatible_node("raspberrypi,bcm2835-firmware")
 *                        -> returns a sentinel device_node pointer
 *   rpi_firmware_get() / devm_rpi_firmware_get()
 *                        -> return a sentinel rpi_firmware handle
 *   of_node_put() / rpi_firmware_put() on those sentinels -> no-op
 *   rpi_firmware_property(<sentinel fw>, ...) -> serviced here
 *
 * Every one of these skips the real function body by rewinding the return
 * address in the kprobe pre-handler (arm64: PC <- LR, pre-handler returns 1).
 * The pointer/string filters are exact, so unrelated callers are untouched.
 *
 * A single OTP device-key slot is modelled. QEMU never reads config.txt, so
 * the firmware state it would otherwise carry comes from module parameters,
 * passed as rpi_fw_mock.<param>= on the kernel command line:
 *
 *   private_key=<hex>            key material for the slot. Empty/absent -> slot
 *                               blank/unprovisioned, HMAC refused until the key
 *                               is generated.
 *   lock_device_private_key=1   slot READ_LOCKED (raw key read blocked; HMAC and
 *                               usage still allowed).
 *   lock_device_key_write=1     slot GEN_LOCKED (GET_CRYPTO_GEN_ECDSA_KEY and any
 *                               key-material write refused).
 *   machine_id=<32 hex>         value published at device-tree
 *                               /chosen/rpi-machine-id, the way the Pi
 *                               bootloader does on real hardware, so
 *                               rpi-root-passphrase finds it there (needs
 *                               CONFIG_OF_DYNAMIC; default is a fixed test value).
 *
 * Serviced tags (numbers + status bits from raspberrypi/utils rpifwcrypto.h,
 * mirrored in rpi-crypto-passphrase.c):
 *
 *   TAG_GET_CRYPTO_KEY_STATUS   (0x00030090)
 *   TAG_GET_CRYPTO_HMAC_SHA256  (0x00030092)
 *   TAG_GET_CRYPTO_GEN_ECDSA_KEY(0x00030095) - fabricates a deterministic key
 *   TAG_SET_CRYPTO_KEY_STATUS   (0x00038090) - OR-accumulates lock bits
 *
 * rpi-crypto-passphrase locks the key (all bits, HMAC included) right after the
 * one derivation, so a second HMAC in the same boot is refused - reload this
 * module to reset the slot.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <crypto/sha2.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define FW_COMPATIBLE "raspberrypi,bcm2835-firmware"

#define TAG_GET_CRYPTO_KEY_STATUS    0x00030090
#define TAG_GET_CRYPTO_HMAC_SHA256   0x00030092
#define TAG_GET_CRYPTO_GEN_ECDSA_KEY 0x00030095
#define TAG_SET_CRYPTO_KEY_STATUS    0x00038090

#define VC_MAILBOX_STATUS_KEY_REJECTED 0x80000001
#define VC_MAILBOX_ERROR              0x80000000

#define KEY_STATUS_TYPE_DEVICE_PRIVATE_KEY (1 << 0)
#define KEY_STATUS_READ_LOCKED            (1 << 8)
#define KEY_STATUS_GEN_LOCKED             (1 << 9)
#define KEY_STATUS_SIGN_LOCKED            (1 << 10)
#define KEY_STATUS_HMAC_LOCKED            (1 << 11)
#define KEY_STATUS_USAGE_LOCKED           (1 << 12)
#define KEY_STATUS_LOCK_MASK              0x1f00

#define HMAC_MSG_MAX 2048
#define KEY_MAX      64

/* Request/response layout of the GET_CRYPTO_HMAC_SHA256 property buffer. */
struct hmac_payload {
	__le32 flags;
	__le32 key_id;
	__le32 length;
	u8 message[HMAC_MSG_MAX];
} __packed;

struct hmac_response {
	__le32 status;
	__le32 length;
	u8 hmac[SHA256_DIGEST_SIZE];
} __packed;

struct key_status_payload {
	__le32 key_id;
	__le32 status;
} __packed;

struct gen_key_payload {
	__le32 flags;
	__le32 key_id;
} __packed;

static u32 mock_key_id = 1;
module_param(mock_key_id, uint, 0444);
MODULE_PARM_DESC(mock_key_id,
		 "firmware key id this mock answers for; other ids are rejected (default 1)");

static bool selftest;
module_param(selftest, bool, 0444);
MODULE_PARM_DESC(selftest, "at load, probe the mock through the shimmed calls and log the result");

static char *machine_id;
module_param(machine_id, charp, 0444);
MODULE_PARM_DESC(machine_id,
		 "32 lowercase hex chars published at device-tree /chosen/rpi-machine-id");

static char *private_key;
module_param(private_key, charp, 0444);
MODULE_PARM_DESC(private_key,
		 "hex OTP crypto key material; empty/absent leaves the slot blank");

static bool lock_device_private_key;
module_param(lock_device_private_key, bool, 0444);
MODULE_PARM_DESC(lock_device_private_key, "start with the slot READ_LOCKED");

static bool lock_device_key_write;
module_param(lock_device_key_write, bool, 0444);
MODULE_PARM_DESC(lock_device_key_write, "start with the slot GEN_LOCKED");

/* Sentinels handed back in place of the real firmware node / handle. */
static u8 mock_fw_node;
static u8 mock_fw_handle;

/* The modelled OTP device-key slot. */
static DEFINE_SPINLOCK(slot_lock);
static bool slot_populated;
static u8   slot_key[KEY_MAX];
static u32  slot_key_len;
static u32  slot_lock_bits;

/* Published at device-tree /chosen/rpi-machine-id (see mock_dt_*); the
 * machine_id= param overwrites this default. */
static char mock_machine_id[33] = "0123456789abcdef0123456789abcdef";

static u32 slot_status_locked(void)
{
	return slot_lock_bits |
	       (slot_populated ? KEY_STATUS_TYPE_DEVICE_PRIVATE_KEY : 0);
}

static void slot_generate_locked(void)
{
	static const char seed_prefix[] = "rpi-fw-mock:genkey:";
	u8 seed[sizeof(seed_prefix) + 4];
	u32 id = cpu_to_le32(mock_key_id);

	memcpy(seed, seed_prefix, sizeof(seed_prefix));
	memcpy(seed + sizeof(seed_prefix), &id, sizeof(id));
	sha256(seed, sizeof(seed), slot_key);
	slot_key_len = SHA256_DIGEST_SIZE;
	slot_populated = true;
}

static void skip_to_caller(struct pt_regs *regs)
{
	instruction_pointer_set(regs, procedure_link_pointer(regs));
}

static int key_status_get(void *buf, size_t size)
{
	__le32 *w = buf;
	unsigned long flags;

	if (size < sizeof(*w))
		return -EINVAL;

	if (le32_to_cpu(*w) != mock_key_id) {
		*w = cpu_to_le32(VC_MAILBOX_ERROR);
		return 0;
	}

	spin_lock_irqsave(&slot_lock, flags);
	*w = cpu_to_le32(slot_status_locked());
	spin_unlock_irqrestore(&slot_lock, flags);
	return 0;
}

static int key_status_set(void *buf, size_t size)
{
	struct key_status_payload *p = buf;
	unsigned long flags;

	if (size < sizeof(*p))
		return -EINVAL;

	if (le32_to_cpu(p->key_id) != mock_key_id) {
		p->status = cpu_to_le32(VC_MAILBOX_ERROR);
		return 0;
	}

	spin_lock_irqsave(&slot_lock, flags);
	slot_lock_bits |= le32_to_cpu(p->status) & KEY_STATUS_LOCK_MASK;
	spin_unlock_irqrestore(&slot_lock, flags);

	p->status = 0;
	return 0;
}

static int gen_key(void *buf, size_t size)
{
	struct gen_key_payload *p = buf;
	unsigned long flags;
	int ret = 0;

	if (size < sizeof(*p))
		return -EINVAL;
	if (le32_to_cpu(p->key_id) != mock_key_id)
		return -EIO;

	spin_lock_irqsave(&slot_lock, flags);
	if (slot_lock_bits & KEY_STATUS_GEN_LOCKED)
		ret = -EIO;
	else if (slot_populated)
		ret = -EIO;			/* one-time OTP write */
	else
		slot_generate_locked();
	spin_unlock_irqrestore(&slot_lock, flags);

	if (!ret)
		pr_info("rpi-fw-mock: generated device key in slot %u: %*phN\n",
			mock_key_id, SHA256_DIGEST_SIZE, slot_key);
	return ret;
}

static int hmac_request(void *buf, size_t size)
{
	struct hmac_payload *req = buf;
	struct hmac_response *resp = buf;
	u32 key_id, length;
	u8 key[KEY_MAX], out[SHA256_DIGEST_SIZE];
	unsigned long flags;
	u32 keylen;
	bool ok;

	if (size < sizeof(*resp))
		return -EINVAL;

	key_id = le32_to_cpu(req->key_id);
	length = le32_to_cpu(req->length);
	if (length > HMAC_MSG_MAX ||
	    size < offsetof(struct hmac_payload, message) + length)
		return -EINVAL;

	spin_lock_irqsave(&slot_lock, flags);
	ok = key_id == mock_key_id && slot_populated &&
	     !(slot_lock_bits & KEY_STATUS_HMAC_LOCKED);
	keylen = slot_key_len;
	if (ok)
		memcpy(key, slot_key, keylen);
	spin_unlock_irqrestore(&slot_lock, flags);

	if (!ok) {
		resp->status = cpu_to_le32(VC_MAILBOX_STATUS_KEY_REJECTED);
		resp->length = 0;
		return 0;
	}

	hmac_sha256_usingrawkey(key, keylen, req->message, length, out);
	memzero_explicit(key, sizeof(key));

	/* the message (context string + caller's disk id) is not secret and is
	 * what you need to reproduce the LUKS passphrase by hand; the HMAC
	 * itself is the passphrase, so it is not logged */
	pr_info("rpi-fw-mock: HMAC over \"%.*s\"\n", length, req->message);

	resp->status = 0;
	resp->length = cpu_to_le32(sizeof(out));
	memcpy(resp->hmac, out, sizeof(out));
	return 0;
}

static int pre_fw_property(struct kprobe *p, struct pt_regs *regs)
{
	u32 tag;
	void *data;
	size_t size;
	int ret;

	if ((void *)regs->regs[0] != (void *)&mock_fw_handle)
		return 0;

	tag = (u32)regs->regs[1];
	data = (void *)regs->regs[2];
	size = (size_t)regs->regs[3];

	if (!data) {
		ret = -EINVAL;
		goto done;
	}

	switch (tag) {
	case TAG_GET_CRYPTO_KEY_STATUS:
		ret = key_status_get(data, size);
		break;
	case TAG_SET_CRYPTO_KEY_STATUS:
		ret = key_status_set(data, size);
		break;
	case TAG_GET_CRYPTO_GEN_ECDSA_KEY:
		ret = gen_key(data, size);
		break;
	case TAG_GET_CRYPTO_HMAC_SHA256:
		ret = hmac_request(data, size);
		break;
	default:
		pr_warn_ratelimited("rpi-fw-mock: unhandled tag 0x%08x\n", tag);
		ret = -EOPNOTSUPP;
		break;
	}

done:
	regs->regs[0] = (unsigned long)(long)ret;
	skip_to_caller(regs);
	return 1;
}

static int pre_fw_get(struct kprobe *p, struct pt_regs *regs)
{
	regs->regs[0] = (unsigned long)&mock_fw_handle;
	skip_to_caller(regs);
	return 1;
}

static int pre_fw_put(struct kprobe *p, struct pt_regs *regs)
{
	if ((void *)regs->regs[0] != (void *)&mock_fw_handle)
		return 0;
	skip_to_caller(regs);
	return 1;
}

static int pre_of_find_compatible(struct kprobe *p, struct pt_regs *regs)
{
	const char *compat = (const char *)regs->regs[2];

	if (!compat || strcmp(compat, FW_COMPATIBLE))
		return 0;

	regs->regs[0] = (unsigned long)&mock_fw_node;
	skip_to_caller(regs);
	return 1;
}

static int pre_of_node_put(struct kprobe *p, struct pt_regs *regs)
{
	if ((void *)regs->regs[0] != (void *)&mock_fw_node)
		return 0;
	skip_to_caller(regs);
	return 1;
}

/*
 * Never called (pre_handlers return 1 on the intercepted paths), but its mere
 * presence keeps the kprobe from being jump-optimized - the optimized trampoline
 * does not honour a pre_handler that rewrites the return address.
 */
static void post_noop(struct kprobe *p, struct pt_regs *regs, unsigned long flags)
{
}

static struct kprobe probes[] = {
	{ .symbol_name = "of_find_compatible_node", .pre_handler = pre_of_find_compatible, .post_handler = post_noop },
	{ .symbol_name = "of_node_put",             .pre_handler = pre_of_node_put,        .post_handler = post_noop },
	{ .symbol_name = "rpi_firmware_get",        .pre_handler = pre_fw_get,             .post_handler = post_noop },
	{ .symbol_name = "devm_rpi_firmware_get",   .pre_handler = pre_fw_get,             .post_handler = post_noop },
	{ .symbol_name = "rpi_firmware_put",        .pre_handler = pre_fw_put,             .post_handler = post_noop },
	{ .symbol_name = "rpi_firmware_property",   .pre_handler = pre_fw_property,        .post_handler = post_noop },
};

/* ---- module parameters + load guard ---------------------------------- */

static int __init parse_hex(const char *s, u8 *out, u32 outmax, u32 *outlen)
{
	u32 n = 0;

	*outlen = 0;
	if (!s)
		return 0;
	for (; *s; s += 2, n++) {
		if (!isxdigit(s[0]) || !isxdigit(s[1]) || n >= outmax)
			return -EINVAL;
		if (sscanf(s, "%2hhx", &out[n]) != 1)
			return -EINVAL;
	}
	*outlen = n;
	return 0;
}

static void __init apply_params(void)
{
	size_t i;

	if (parse_hex(private_key, slot_key, KEY_MAX, &slot_key_len) == 0 && slot_key_len) {
		slot_populated = true;
		pr_info("rpi-fw-mock: slot %u provisioned from private_key= (%u bytes)\n",
			mock_key_id, slot_key_len);
	} else {
		if (private_key && *private_key)
			pr_warn("rpi-fw-mock: ignoring invalid private_key=\n");
		pr_info("rpi-fw-mock: slot %u unprovisioned\n", mock_key_id);
	}

	if (lock_device_private_key) {
		slot_lock_bits |= KEY_STATUS_READ_LOCKED;
		pr_info("rpi-fw-mock: lock_device_private_key -> READ_LOCKED\n");
	}
	if (lock_device_key_write) {
		slot_lock_bits |= KEY_STATUS_GEN_LOCKED;
		pr_info("rpi-fw-mock: lock_device_key_write -> GEN_LOCKED\n");
	}

	if (machine_id) {
		bool ok = strlen(machine_id) == 32;

		for (i = 0; ok && i < 32; i++)
			if (!isxdigit(machine_id[i]) || isupper(machine_id[i]))
				ok = false;
		if (ok)
			strscpy(mock_machine_id, machine_id, sizeof(mock_machine_id));
		else
			pr_warn("rpi-fw-mock: ignoring invalid machine_id= (want 32 lowercase hex)\n");
	}
}

/* /proc/cmdline into a kzalloc'd PAGE_SIZE buffer (caller frees), or NULL. */
static char *read_proc_cmdline(void)
{
	struct file *f;
	loff_t pos = 0;
	ssize_t n;
	char *cl;

	cl = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!cl)
		return NULL;

	f = filp_open("/proc/cmdline", O_RDONLY, 0);
	if (IS_ERR(f)) {
		kfree(cl);
		return NULL;
	}
	n = kernel_read(f, cl, PAGE_SIZE - 1, &pos);
	filp_close(f, NULL);
	if (n <= 0) {
		kfree(cl);
		return NULL;
	}
	cl[n] = '\0';
	return cl;
}

static bool token_is_this_module(const char *s, size_t len)
{
	static const char name[] = "rpi-fw-mock";
	size_t i;

	if (len != sizeof(name) - 1)
		return false;
	for (i = 0; i < len; i++) {
		char c = s[i] == '_' ? '-' : s[i];

		if (c != name[i])
			return false;
	}
	return true;
}

/* This module's name appears in an rd.modules_load= / modules_load= list. */
static bool cmdline_module_enabled(const char *cl)
{
	static const char * const keys[] = { "rd.modules_load=", "modules_load=" };
	int k;

	for (k = 0; k < ARRAY_SIZE(keys); k++) {
		const char *p = cl;

		while ((p = strstr(p, keys[k]))) {
			const char *v = p + strlen(keys[k]);

			p = v;
			while (*v && *v != ' ' && *v != '\t' && *v != '\n') {
				const char *e = v;

				while (*e && *e != ',' && *e != ' ' &&
				       *e != '\t' && *e != '\n')
					e++;
				if (token_is_this_module(v, e - v))
					return true;
				v = (*e == ',') ? e + 1 : e;
			}
		}
	}
	return false;
}

/* ---- device-tree /chosen/rpi-machine-id ------------------------------- */

#ifdef CONFIG_OF_DYNAMIC
static struct of_changeset mock_dt_cs;
static bool mock_dt_applied;

static void mock_dt_add_machine_id(void)
{
	int ret;

	if (!of_chosen) {
		pr_warn("rpi-fw-mock: no /chosen node; not adding rpi-machine-id\n");
		return;
	}
	if (of_property_present(of_chosen, "rpi-machine-id")) {
		pr_info("rpi-fw-mock: /chosen/rpi-machine-id already present, leaving it\n");
		return;
	}

	of_changeset_init(&mock_dt_cs);
	ret = of_changeset_add_prop_string(&mock_dt_cs, of_chosen,
					  "rpi-machine-id", mock_machine_id);
	if (!ret)
		ret = of_changeset_apply(&mock_dt_cs);
	if (ret) {
		of_changeset_destroy(&mock_dt_cs);
		pr_warn("rpi-fw-mock: could not add /chosen/rpi-machine-id: %d\n", ret);
		return;
	}
	mock_dt_applied = true;
	pr_info("rpi-fw-mock: /chosen/rpi-machine-id = %s\n", mock_machine_id);
}

static void mock_dt_del_machine_id(void)
{
	if (!mock_dt_applied)
		return;
	of_changeset_revert(&mock_dt_cs);
	of_changeset_destroy(&mock_dt_cs);
	mock_dt_applied = false;
}
#else
static void mock_dt_add_machine_id(void)
{
	pr_warn("rpi-fw-mock: kernel has no CONFIG_OF_DYNAMIC; cannot publish rpi-machine-id\n");
}
static void mock_dt_del_machine_id(void) { }
#endif

/* ---- self-test -------------------------------------------------------- */

static void __init run_selftest(void)
{
	static const u8 msg[] = "rpi-fw-mock selftest";
	const size_t msglen = sizeof(msg) - 1;
	struct hmac_response *resp;
	struct hmac_payload *pl;
	struct device_node *np;
	struct rpi_firmware *fw;
	u32 status = mock_key_id;
	int ret;

	np = of_find_compatible_node(NULL, NULL, FW_COMPATIBLE);
	fw = rpi_firmware_get(np);
	of_node_put(np);
	if (np != (struct device_node *)&mock_fw_node ||
	    fw != (struct rpi_firmware *)&mock_fw_handle) {
		pr_err("rpi-fw-mock: selftest: shim not active (np=%p fw=%p)\n", np, fw);
		return;
	}

	ret = rpi_firmware_property(fw, TAG_GET_CRYPTO_KEY_STATUS, &status, sizeof(status));
	pr_info("rpi-fw-mock: selftest: key status = 0x%08x (ret=%d)\n", status, ret);

	pl = kzalloc(sizeof(*pl), GFP_KERNEL);
	if (!pl) {
		rpi_firmware_put(fw);
		return;
	}
	resp = (struct hmac_response *)pl;

	pl->key_id = cpu_to_le32(mock_key_id);
	pl->length = cpu_to_le32(msglen);
	memcpy(pl->message, msg, msglen);
	ret = rpi_firmware_property(fw, TAG_GET_CRYPTO_HMAC_SHA256, pl, sizeof(*pl));

	if (slot_populated) {
		u8 want[SHA256_DIGEST_SIZE];

		hmac_sha256_usingrawkey(slot_key, slot_key_len, msg, msglen, want);
		if (ret || le32_to_cpu(resp->status) ||
		    memcmp(resp->hmac, want, sizeof(want)))
			pr_err("rpi-fw-mock: selftest: FAILED (ret=%d status=0x%08x)\n",
			       ret, le32_to_cpu(resp->status));
		else
			pr_info("rpi-fw-mock: selftest: OK, provisioned, HMAC=%*phN\n",
				SHA256_DIGEST_SIZE, resp->hmac);
	} else {
		if (!ret && le32_to_cpu(resp->status) == VC_MAILBOX_STATUS_KEY_REJECTED)
			pr_info("rpi-fw-mock: selftest: OK, unprovisioned, HMAC refused\n");
		else
			pr_err("rpi-fw-mock: selftest: FAILED unprovisioned case (ret=%d status=0x%08x)\n",
			       ret, le32_to_cpu(resp->status));
	}

	rpi_firmware_put(fw);
	kfree(pl);
}

/* Refuse to run anywhere it could do harm: only in the initrd, only when
 * explicitly requested on the kernel command line, and never where a real
 * Raspberry Pi firmware node is present. */
static int __init mock_guard(const char *cl)
{
	struct file *f;
	struct device_node *np;

	f = filp_open("/etc/initrd-release", O_RDONLY, 0);
	if (IS_ERR(f)) {
		pr_err("rpi-fw-mock: not in an initrd - refusing to load\n");
		return -EPERM;
	}
	filp_close(f, NULL);

	if (!cmdline_module_enabled(cl)) {
		pr_err("rpi-fw-mock: not in rd.modules_load= - refusing to load\n");
		return -EPERM;
	}

	np = of_find_compatible_node(NULL, NULL, FW_COMPATIBLE);
	if (np) {
		of_node_put(np);
		pr_err("rpi-fw-mock: real %s node present - refusing to load\n",
		       FW_COMPATIBLE);
		return -ENODEV;
	}

	return 0;
}

static int __init mock_init(void)
{
	int ret, i, registered = 0;
	char *cl;

	cl = read_proc_cmdline();
	if (!cl)
		return -ENOMEM;

	ret = mock_guard(cl);
	kfree(cl);
	if (ret)
		return ret;

	apply_params();
	mock_dt_add_machine_id();

	/* symbol_name lookup needs the firmware driver's kallsyms present */
	request_module("raspberrypi-firmware");

	for (i = 0; i < ARRAY_SIZE(probes); i++) {
		ret = register_kprobe(&probes[i]);
		if (ret) {
			pr_err("rpi-fw-mock: register_kprobe(%s) failed: %d\n",
			       probes[i].symbol_name, ret);
			goto err;
		}
		registered++;
	}

	pr_info("rpi-fw-mock: faking %s crypto mailbox for key id %u\n",
		FW_COMPATIBLE, mock_key_id);

	if (selftest)
		run_selftest();
	return 0;

err:
	while (registered--)
		unregister_kprobe(&probes[registered]);
	mock_dt_del_machine_id();
	return ret;
}

static void __exit mock_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(probes); i++)
		unregister_kprobe(&probes[i]);
	mock_dt_del_machine_id();
	memzero_explicit(slot_key, sizeof(slot_key));
	pr_info("rpi-fw-mock: unloaded\n");
}

module_init(mock_init);
module_exit(mock_exit);

MODULE_SOFTDEP("pre: raspberrypi-firmware");
MODULE_DESCRIPTION("Mock Raspberry Pi firmware crypto mailbox for QEMU testing");
MODULE_AUTHOR("rpi-mkosi");
MODULE_LICENSE("GPL");
