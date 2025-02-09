TODO
---

## Configuration

### Where to store root disk encryption key
* As plaintext in eeprom config?
  * still better than as plaintext on ESP partition
  * could be populated on second boot (add pieeprom.upd on first boot and remove it on second boot)
  * does not work with secure boot as the machine does not _(and should not)_ have the signing key ☹
    * Two-step flashing process could fix this: first, flash identity/eeprom, then OS. But then we can generate the secret on the device.
* In eeprom OTP registry? – Debian contains no `vcmailbox` tools and [no nvmem-raspberrypi-otp kernel module](https://forums.raspberrypi.com/viewtopic.php?t=380883) (so `/dev/vcio` missing)
* Just buy more of [those TPM](https://raspberrypi.dk/en/product/letstrust-tpm-for-raspberry-pi/)s?

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
