# rpi-fw-mock

A kprobe module that fakes the Raspberry Pi firmware **crypto** mailbox, so
`rpi-crypto-passphrase` (and, through it, the root-disk passphrase flow) can be
exercised under plain `mkosi vm` — the QEMU `virt` machine, which has no BCM2711
mailbox at all.

## Why this exists

- **Every feature should be testable in a fast edit-build-run loop.** Booting to
  a real Pi rarely is that — especially with no serial console and no netboot —
  and the firmware-crypto path is otherwise only reachable on hardware.
- **The pipeline should be able to test firmware-dependent things too**, not
  just skip them; that needs the firmware side available in a VM.
- **QEMU's built-in `raspi4b` machine isn't enough**: its mailbox implementation
  predates the 2025 firmware crypto API, and the machine hard-caps guest RAM at
  2 GB.

So the mock stands in for the firmware's crypto mailbox in a `virt` VM: it
services the property tags, and publishes the per-device `rpi-machine-id` the Pi
bootloader would put in the device tree.

## How it works

It shims `of_find_compatible_node("raspberrypi,bcm2835-firmware")`,
`rpi_firmware_get()`, `rpi_firmware_put()`, `of_node_put()` and
`rpi_firmware_property()` via kprobes, returning sentinels and servicing the
crypto property tags in-module. See the header comment in `rpi-fw-mock.c`.

Serviced tags: `GET_CRYPTO_KEY_STATUS` (0x30090), `GET_CRYPTO_HMAC_SHA256`
(0x30092), `GET_CRYPTO_GEN_ECDSA_KEY` (0x30095), `SET_CRYPTO_KEY_STATUS`
(0x38090).

## Loading

Built and installed to `extra/drivers/misc/` in **every** image
(`mkosi.build.chroot`), but `mock_guard()` refuses to load unless all of:

- running in an initrd (`/etc/initrd-release` present) — a booted system can't
  `modprobe` it in;
- this module named in `rd.modules_load=` / `modules_load=` on the kernel
  command line — nothing loads it implicitly;
- no real `raspberrypi,bcm2835-firmware` device-tree node — never on hardware.

`mkosi.conf`'s `[Runtime]` carries `rd.modules_load=rpi-fw-mock` +
`QemuArgs=-machine acpi=off` (so the guest boots device-tree-only, like a Pi),
so every `mkosi vm` (any profile) loads it.
`modprobe@rpi-crypto-passphrase.service` is ordered
`After=systemd-modules-load.service` so the kprobes are live before that init.

## Parameters

Passed as `rpi_fw_mock.<param>=` on the kernel command line — QEMU exposes none
of the firmware state, so the mock takes it here. All in `mkosi.conf`
`[Runtime]`.

| param | effect |
|---|---|
| `machine_id=<32 lc hex>` | value published at DT `/chosen/rpi-machine-id` (default `0123…cdef`); what `rpi-root-passphrase` keys the LUKS HMAC off. Needs `CONFIG_OF_DYNAMIC` (Debian arm64 has it via `OF_OVERLAY`). |
| `private_key=<hex>` | provisions the OTP crypto slot. Empty/absent → slot blank, HMAC refused (status `0x80000001`) until `GET_CRYPTO_GEN_ECDSA_KEY` fabricates a key. |
| `lock_device_private_key=1` | slot `READ_LOCKED` — HMAC and usage still allowed |
| `lock_device_key_write=1` | slot `GEN_LOCKED` — `GET_CRYPTO_GEN_ECDSA_KEY` refused |
| `mock_key_id=<n>` | firmware key id the mock answers for (default 1) |
| `selftest=1` | at load, probe the mock through the shims and log the key-status word + HMAC result (or its refusal, for a blank slot) against an in-module reference |

`rpi-crypto-passphrase` locks the whole key (HMAC bit included) right after its
one derivation, so a second HMAC in the same boot is refused.

## End-to-end test (`mkosi vm`)

```bash
mkosi build && mkosi vm
```
```bash
journalctl -b | grep -E 'rpi-fw-mock|rpi-crypto|rpi-root-passphrase'
```

Provisioned (default cmdline): `rpi-root-passphrase: derived 32-byte
passphrase, serving`.

Blank slot (drop `rpi_fw_mock.private_key` from the cmdline): expect
`rpi-crypto-passphrase: generated device key in slot 1` +
`rpi-fw-mock: generated device key in slot 1: <hex>` before the derivation.

### Verifying the root LUKS key is the mock-derived HMAC

The mock logs the message it HMACs:

```
rpi-fw-mock: /chosen/rpi-machine-id = 0123456789abcdef0123456789abcdef
rpi-fw-mock: HMAC over "rpi-mkosi/root-luks-passphrase:0123456789abcdef0123456789abcdef"
```

Recompute it and check it opens the real container (`systemd-cryptsetup` and
`openssl` are always in the image):

```bash
kh=$(sed -n 's/.*rpi_fw_mock\.private_key=\([0-9A-Fa-f]\+\).*/\1/p' /proc/cmdline)
mid=$(tr -d '\0' < /sys/firmware/devicetree/base/chosen/rpi-machine-id)
printf '%s' "rpi-mkosi/root-luks-passphrase:$mid" \
  | openssl mac -digest SHA256 -macopt hexkey:"$kh" -binary HMAC > /run/k
back=/dev/$(ls "/sys/class/block/$(basename "$(readlink -f "$(findmnt -no SOURCE /)")")/slaves")
/usr/lib/systemd/systemd-cryptsetup attach vrfy "$back" /run/k headless=yes \
  && echo MATCH && /usr/lib/systemd/systemd-cryptsetup detach vrfy
```
