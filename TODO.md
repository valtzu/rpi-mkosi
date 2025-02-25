TODO
---

## Configuration

### Generate root partition encryption key

* Root partition encryption key is read from `/sys/bus/nvmem/devices/nvmem_priv0/nvmem`, but
  it's all zeros by default. There should be some config switch to write a random value into 
  it. It is worth noting that it is one-time-programmable, so once the bits go nonzero, there's
  no going back. 

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
