TODO
---

## Configuration

### Provision the root partition encryption key

* The root partition passphrase is now derived via the firmware mailbox's HMAC-SHA256
  crypto service (OTP key id 0), not read raw from OTP - see
  `mkosi.profiles/disk/rpi-crypto-passphrase.c`. This requires OTP key id 0 to already
  hold a valid ECDSA P-256 private key (the `d` component), not an arbitrary random
  value. Provisioning is still a manual, one-time, out-of-band step, e.g.:
  ```
  openssl ecparam -name prime256v1 -genkey -noout -out private_key.pem
  openssl ec -in private_key.pem -text -noout | awk '/priv:/{flag=1; next} /pub:/{flag=0} flag' | tr -d ' \n:' | head -n1 > d.hex
  rpi-otp-private-key -w $(cat d.hex)
  ```
  It is worth noting that OTP is one-time-programmable, so once the bits go nonzero,
  there's no going back. There should be some config switch to drive this.

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
