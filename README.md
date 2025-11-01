## Raspberry Pi 4B + `mkosi` + `systemd`

Inspired by https://0pointer.net/blog/fitting-everything-together.html 

### Included in the image

1. ESP (`/efi`) partition
   * `boot.img` that contains RPi firmware & config + EDK2 firmware with Secure Boot using our custom cert (`mkosi.crt`)
   * `boot.sig` signed with `mkosi.key`. `mkosi.crt` should be included in EEPROM (using `rpi-eeprom-config`) to make the boot chain secure
   * Unified Kernel Image (UKI), signed with `mkosi.key`
     * `linux-image-generic` from the distribution 
     * `nvmem-raspberrypi-otp` kernel module from [raspberrypi/linux](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/drivers/nvmem/raspberrypi-otp.c)
2. Readonly `/usr` partition
   * Debian Trixie distribution, other systemd>=256 distros should work too
   * "Golden" `/etc` stored into `/usr/share/factory/etc`
   * verity & verity-sig partitions make sure the contents are not tampered with

### On first boot

1. Create encrypted root partition
   * passphrase from [RPi eeprom OTP registry](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#otp-register-and-bit-definitions)
   * `/etc` populated from `/usr/share/factory/etc` using `systemd-repart`'s `CopyFiles=`
   * other root directories & files populated with `systemd-tmpfiles` (no custom configuration)
2. Create 3 empty matching-size partitions (labeled `_empty`) for `/usr` updates

### On every boot

**NOTE:** This is default behavior of `systemd-sysupdate`

1. After 15 minutes of uptime, query updates from GitHub releases using `systemd-sysupdate`
   * Download the new `usr` + `verity` + `verity-sig` partitions directly into
     the `_empty` partitions
   * Download the new UKI to `/efi/EFI/Linux/system_x.x.x.efi`
2. Periodically check if a new version is installed
   * if found, reboot
     * if reboot fails, auto-rollback to previous version (untested!)

## Setup dev env

Everything is done inside virtual machine since we need quite recent systemd + previously `mkosi` required `root` access.

```bash
vagrant up
vagrant ssh
```

### Generate Secure Boot keys
``` 
mkosi --directory="" genkey

# If using Vagrant with rsync, copy keys back to host so you don't lose them
cp mkosi.key mkosi.crt /vagrant/ 
```

### Build image for Raspberry Pi
```
mkosi
```

### Run on host
```
mkosi vm
```
