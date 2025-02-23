## Raspberry Pi 4B + `mkosi` + `systemd`

What we are doing here using `mkosi` & other `systemd` tooling:

1. build OS image (Debian Trixie) that has everything in readonly `/usr` partition (& related verity partitions)
2. `/efi` partition contains signed Unified Kernel Image (UKI) + `boot.sig`&`boot.img` that contains RPi firmware 
3. On first boot, create root encrypted partition, using passphrase from [RPi OTP registry](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html#otp-register-and-bit-definitions)
4. Auto-update existing deployments from GitHub releases using `systemd-sysupdate` 

### Setup dev env
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
mkosi --credential=cryptsetup.passphrase=123 qemu
```
