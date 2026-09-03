## Raspberry Pi 4B + `mkosi` + `systemd`

Inspired by https://0pointer.net/blog/fitting-everything-together.html 

### Included in the image

1. ESP (`/efi`) partition
   * `boot.img` that contains RPi firmware & config + EDK2 firmware with Secure Boot using our custom cert (`mkosi.crt`)
   * `boot.sig` signed with `mkosi.key`. `mkosi.crt` should be included in EEPROM (using `rpi-eeprom-config`) to make the boot chain secure
   * `boot.img` + `boot.sig` are also emitted standalone into `mkosi.output/` (and attached to releases) for TFTP/HTTP netboot
   * Unified Kernel Image (UKI), signed with `mkosi.key`
     * `linux-image-generic` from the distribution 
     * `nvmem-raspberrypi-otp` kernel module from [raspberrypi/linux](https://github.com/raspberrypi/linux/blob/rpi-6.12.y/drivers/nvmem/raspberrypi-otp.c)
2. Readonly `/usr` partition
   * Debian Forky distribution, other systemd>=256 distros should work too
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

### Reproducible builds

Package versions are pinned to [snapshot.debian.org](https://snapshot.debian.org)
timestamps in [`mkosi.conf`](mkosi.conf) (`Snapshot=` for the image,
`ToolsTreeSnapshot=` for the build tools tree), and the build is
byte-reproducible: two builds of the same commit produce an identical disk image
(`system_<ver>.raw`), UKI (`system_<ver>.efi`), rootfs tarball, initrd,
`boot.img` and `usr` / verity / verity-sig partitions. CI pins
`SOURCE_DATE_EPOCH` to the commit timestamp and writes a static `mkosi.seed`
(the repart seed); to reproduce locally:

```bash
echo -n 8c58b3b9-7383-50e6-aed7-8d8341fdaf5f > mkosi.seed
SOURCE_DATE_EPOCH=$(git log -1 --format=%ct) mkosi -f build
```

### Versioning

[`mkosi.version`](mkosi.version) is a script: `git describe` on a release build
(so exactly the tag), `<tag>-<n>-g<sha>` in between, plus `-<unix-time>` when the
tree is dirty. `systemd-sysupdate`'s version compare ranks every one of those
above the last release, so the local loop just works:

```bash
git switch -c my-change
# edit mkosi.conf / add a package / ...
mkosi build && mkosi serve   # a device pointed at this will sysupdate to it
```

(Commit the change for a stable version; the `-<unix-time>` suffix on a dirty
tree still advances on every build.)

### Package updates

Both driven by [`valtzu/gh-action-mkosi-bump`](https://github.com/valtzu/gh-action-mkosi-bump),
no PAT:

* [`Weekly package release`](.github/workflows/mkosi-bump.yml) — **Friday 19:00
  UTC**: moves `Snapshot=` to the newest `mkosi latest-snapshot` timestamp,
  commits it straight to `main`, tags the next patch version, and
  `workflow_dispatch`es [`mkosi.yml`](.github/workflows/mkosi.yml) on that tag.
  The release job builds, uploads the assets to a draft, then flips it to
  published + latest. Devices pick it up on their next `systemd-sysupdate` poll
  and reboot into it at `systemd-sysupdate-reboot.timer` (04:10 local), so the
  rollout lands over the weekend nights. No review gate - auto-rollback covers a
  bad boot, but a broken package set still reaches every device until the next
  release.
* [`Bump mkosi tools tree`](.github/workflows/mkosi-bump-tools-tree.yml) — moves
  `ToolsTreeSnapshot=` in a rolling `mkosi-bump-tools-tree` PR. A tools-tree bump
  can break the build, so it goes through review; the PR is built by the
  `pull_request` trigger in `mkosi.yml` (click *Approve workflows to run* on it,
  since a `GITHUB_TOKEN`-opened PR's checks start pending).

## Setup dev env

`mkosi` runs unprivileged now, so the build happens directly on the host - no
VM. You need a recent systemd (>=256) and `mkosi` v27:

```bash
pipx install git+https://github.com/systemd/mkosi.git@v27
```

Cross-building for aarch64 also needs `qemu-user-static` + `binfmt-support` (or
your distro's equivalent) registered.

### Generate Secure Boot keys
```
mkosi --directory="" genkey
```

### Build image for Raspberry Pi
```
mkosi
```

### Run on host
```
mkosi vm
```
