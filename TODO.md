TODO
---

## Configuration

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
