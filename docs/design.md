# Image layout & signing chain

How the build artifacts fit together and which key signs what. See the
[README](../README.md) for the boot / update flow; this doc is just the static
picture.

## Signing keys

One key pair (`mkosi genkey`) is the root of trust for everything:

| File | What it is | Where it comes from |
| --- | --- | --- |
| `mkosi.key` | RSA private key | `mkosi genkey` (kept off-device) |
| `mkosi.crt` | X.509 cert wrapping the public key | `mkosi genkey` |
| `mkosi.pub` | bare RSA public key | `openssl rsa -in mkosi.key -pubout`, in `mkosi.images/rootfs/mkosi.finalize` |

`mkosi.crt` is what verifies UEFI Secure Boot and the dm-verity signature;
`mkosi.pub` is the form `rpi-eeprom-config` embeds so the bootloader can verify
its own updates and `boot.img`.

## `boot.img` and the EEPROM update

`boot.img` is an 8 MiB FAT16 image built by
[`mkosi.images/rootfs/mkosi.finalize`](../mkosi.images/rootfs/mkosi.finalize).
It is copied into the ESP **and** emitted standalone into `mkosi.output/`
(with `boot.sig`) for TFTP/HTTP netboot.

```mermaid
flowchart TD
    key["mkosi.key<br/>(RSA private)"]
    crt["mkosi.crt<br/>(X.509 cert)"]
    pub["mkosi.pub<br/>(RSA public)"]
    key -. "mkosi genkey" .-> crt
    key -. "openssl rsa -pubout<br/>(mkosi.finalize)" .-> pub

    subgraph bootimg["boot.img — 8 MiB FAT16 (mkosi.finalize)"]
        fw["start4.elf<br/>fixup4.dat<br/>bcm2711-rpi-4-b.dtb"]
        ov["overlays/<br/>upstream-pi4, disable-wifi,<br/>disable-bt, overlay_map"]
        cfg["config.txt<br/>armstub=RPI_EFI.fd<br/>lock_device_key_write=0 (release) / 1 (dev)"]
        edk["RPI_EFI.fd<br/>EDK2 UEFI + Secure Boot vars<br/>(pftf/RPi4 release)"]

        subgraph upd["pieeprom.upd"]
            eeprom["pieeprom-&lt;EEPROM_VERSION&gt;.bin<br/>(raspberrypi/rpi-eeprom, pinned)"]
            embpub["mkosi.pub"]
            bconf["bootconf.txt<br/>SIGNED_BOOT=1, BOOT_ORDER, ..."]
            bsig["bootconf.sig"]
        end
        psig["pieeprom.sig"]
    end

    fwsrc[("raspberrypi/firmware<br/>FIRMWARE_REF (curl.txt)")] -. download .-> fw
    fwsrc -. download .-> ov
    uefisrc[("pftf/RPi4<br/>UEFI_VERSION")] -. download .-> edk

    crt -- "virt-fw-vars --enroll-cert / --add-db<br/>(+ efivars.json)" --> edk
    bconf -- "rpi-eeprom-digest -k mkosi.key" --> bsig
    key --> bsig
    pub -- "rpi-eeprom-config --pubkey" --> embpub
    bconf --> upd
    bsig --> upd
    eeprom --> upd
    upd -- "rpi-eeprom-digest (no key,<br/>corruption guard only)" --> psig

    bootimg -- "rpi-eeprom-digest -k mkosi.key" --> bootsig["boot.sig"]
    key --> bootsig
```

The EEPROM verifies `pieeprom.upd` / `boot.img` against the key hash of the cert
it already trusts. `SOURCE_DATE_EPOCH` pins timestamps in the `.sig` files so an
unchanged tree does not trigger an EEPROM reflash on every boot.

## ESP contents and the UKI

The ESP partition ([`mkosi.repart/00-esp.conf`](../mkosi.repart/00-esp.conf))
takes `/efi` + `/boot` from the rootfs image.

```mermaid
flowchart TD
    key["mkosi.key"]
    crt["mkosi.crt"]

    subgraph esp["ESP — /efi (vfat)"]
        bimg["boot.img"]
        bsig["boot.sig"]
        loader["loader/loader.conf<br/>default latest (dev) / menu"]
        sdboot["EFI/BOOT/BOOTAA64.efi<br/>(systemd-boot)"]

        subgraph uki["EFI/Linux/system_&lt;version&gt;.efi — UKI"]
            kern["linux kernel (arm64)<br/>linux-image-generic"]
            initrd["initrd<br/>base initrd image + raspberrypi modules<br/>+ rpi-crypto-passphrase.ko"]
            cmdline["cmdline<br/>console=ttyAMA0, mount.usrflags=ro, ...<br/>+ usrhash=&lt;verity root hash&gt;"]
            prof["UKI profiles<br/>@0 main (timers inert)<br/>latest → +sysupdate.update=latest"]
        end
    end

    crt -. "enrolled in RPI_EFI.fd" .-> bimg
    key -- "mkosi sign (SecureBoot=yes,<br/>SecureBootKey/Certificate)" --> uki
    verity[("usr-verity")] -- "root hash" --> cmdline
```

`UnifiedKernelImages=unsigned` in `mkosi.conf` only means mkosi does not sign
via `sbsign` in the tool step; the `[Validation]` `SecureBootKey` /
`SecureBootCertificate` still sign the finished UKI. systemd-boot itself is
verified by the Secure Boot db enrolled into `RPI_EFI.fd`.

## `/usr`, dm-verity and the disk image

```mermaid
flowchart TD
    key["mkosi.key"]
    crt["mkosi.crt"]

    usr["usr partition<br/>ext4, read-only /usr<br/>Debian forky + golden /etc → /usr/share/factory"]
    verity["usr-verity<br/>dm-verity hash tree"]
    veritysig["usr-verity-sig<br/>PKCS#7 signature of the verity root hash"]

    usr -- "systemd-repart Verity=data" --> verity
    verity -- "root hash" --> veritysig
    key -- "VerityKey" --> veritysig
    crt -- "VerityCertificate (embedded for verification)" --> veritysig
```

### Partition layout

`SplitArtifacts=uki,partitions` also drops each partition out as its own
`system_<version>.usr-*.raw` file (+ `system_<version>.efi`), which is what
`systemd-sysupdate` downloads (`mkosi.images/rootfs/mkosi.extra/usr/lib/sysupdate.d/`).

```mermaid
flowchart LR
    subgraph build["build-time disk — system_&lt;version&gt;.raw (mkosi.repart/)"]
        b_esp["ESP (boot)"]
        b_usra["usr-a"]
        b_va["usr-a-verity"]
        b_sa["usr-a-verity-sig"]
    end

    subgraph firstboot["after first boot — systemd-repart in initrd (rootfs .../repart.d/)"]
        f_esp["ESP (CopyBlocks=auto)"]
        f_usra["usr-a (populated)"]
        f_usrb["usr-b (_empty spare)"]
        f_root["root<br/>ext4, encrypted (key-file)<br/>FactoryReset, /etc from factory"]
    end

    build ==> firstboot
```

The root partition's key file is the passphrase the firmware mailbox derives
(HMAC-SHA256 of `/chosen/rpi-machine-id` under the OTP device key) — see the
README "On first boot" section. The spare `_empty` `usr` / verity / verity-sig
partitions are the A/B target `systemd-sysupdate` stages new versions into.
