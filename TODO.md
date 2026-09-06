TODO
---

## Configuration

### Provision the root partition encryption key

* The root partition passphrase is derived via the firmware mailbox's HMAC-SHA256
  crypto service (OTP key id 1 - `rpi-otp-private-key`'s default keystore slot; the
  module's `key_id=` param overrides it), not read raw from OTP - see
  `rpi-crypto-passphrase.c`. This requires that key id to hold a valid ECDSA P-256
  private key.
* Release images provision it automatically: `config.txt` ships
  `lock_device_key_write=0`, and on the first boot `rpi-crypto-passphrase` sees the
  blank slot and issues the firmware `genkey` (a one-time OTP write), then locks
  generation + usage. Dev images ship `lock_device_key_write=1` and never touch OTP.
  Pass `rpi-crypto-passphrase.provision=0` to opt a release image out.
* OTP is one-time-programmable - once the bits go nonzero there's no going back, so
  the first release boot on a given board is the point of no return.

### How to populate device-specific configuration 

* Populate confext via `import.pull` systemd credential?
  * How to make it device-specific though?
* Just use ansible or some other traditional configuration management tool? ☹
* Use `ConditionHost=` etc and include configuration for all devices in the golden image
  * Only works as long as there's no secrets (or the config is not in public repo)
* Patch golden image with device-specific configuration before flashing
  * This only works for initial flashing – how to update configuration?

### Things to configure per device

* IP & hostname
  * I don't want to leave identity assignment for DHCP server
* Services
  * Which services to run
  * Configuration for a single service may vary between nodes
  * Preferably via some kind of group/role
