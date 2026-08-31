# iSCSI dev target

Boot the Pi from a locally served image instead of reflashing a USB disk.
`mkosi --profile dev` builds an image; `serve-iscsi` on this machine serves its
root over iSCSI, and the Pi reaches it like this:

1. The RPi bootloader HTTP-boots `boot.img` (EDK2 + firmware) from `mkosi serve`
   (`:8081`), using the static `HTTP_HOST`/`HTTP_PORT` baked into the EEPROM
   config by `mkosi.finalize`.
2. EDK2 does `Start PXE over IPv4`. `serve-netboot` (proxy-DHCP + TFTP `dnsmasq`
   on this machine) chainloads iPXE's arm64 `snponly.efi`. On an HTTP boot EDK2
   is armstub-loaded with no filesystem, so PXE is the only way to hand it iPXE.
3. iPXE re-DHCPs (user-class `iPXE`), gets `script.ipxe` over HTTP, does plain
   DHCP and `sanboot`s the iSCSI LUN. The ESP there chainloads; then
4. Linux comes up and its initrd re-attaches the iSCSI root itself
   (`iscsi_portal=`/`iscsi_target=` on the kernel cmdline, see
   `mkosi.conf.d/dev.conf` and the initrd `initrd-iscsi-root`).

`mkosi.finalize` also bakes `snponly.efi` + `autoexec.ipxe` into `boot.img`
(`\EFI\BOOT\BOOTAA64.EFI`); EDK2 only sees that FAT when booting from a
`burn-boot` USB stick, not over HTTP. `edk2-iscsi-efivars` (static EDK2 iSCSI
attempt) is kept for reference but unused.

`tgt` is installed into the default tools tree (`ToolsTreePackages=` in the
top-level `mkosi.conf`), and `mkosi box` keeps host networking - so this
needs no root and no container runtime.

## Per-machine config: `mkosi.local.conf`

`mkosi --profile dev` reads an untracked `mkosi.local.conf` at the repo root:

```
[Build]
Environment=
    DEV_HOST=192.168.1.8
```

- `DEV_HOST` - optional; this machine's LAN IP, used for the EEPROM `HTTP_HOST`
  and as the default iSCSI portal. Defaults to `hostname -I | awk '{print $1}'`;
  set it only if that picks the wrong interface.

The Pi's MAC is no longer needed here (the old EDK2 iSCSI attempt matched on it;
DHCP-driven HTTP boot does not).

## Secure Boot

`mkosi.finalize` enrolls two certs into the EDK2 `db` for the dev profile:

- `mkosi.crt` - signs the UKI (`system_.efi`) and the systemd-boot stub.
- `cache/ipxe/testsign.crt` - iPXE's arm64 secure-boot test-signing cert, so
  EDK2 loads `snponly.efi` under Secure Boot.

`mkosi.sync` fetches `snponly.efi` + `testsign.crt` from
`boot.ipxe.org/arm64-efi-sb/` into `cache/ipxe/` when the dev profile is active
(skipped if already present). iPXE regenerates that test key roughly monthly;
if the Pi's serial shows `snponly.efi` failing image verification,
`rm cache/ipxe/testsign.crt cache/ipxe/snponly.efi` and rebuild to refetch.

The upstream PFTF firmware is used by default; drop a locally built `RPI_EFI.dev.fd`
at the repo root to substitute a local build (see `tools/iscsi/edk2/`).

## `serve-netboot` (proxy-DHCP + TFTP)

`tools/iscsi/serve-netboot` runs a proxy-DHCP + TFTP `dnsmasq` (in the tools
tree) that answers EDK2's PXE request with iPXE's `snponly.efi`, then hands
iPXE `http://$DEV_HOST:8081/script.ipxe`. Needs privileged ports:

```bash
DEV_HOST=192.168.1.8 sudo mkosi box -- tools/iscsi/serve-netboot
```

`serve-netboot` fills the `<--` placeholders in `dnsmasq.conf` from `DEV_HOST`
(+ a `/24` `SUBNET`, override if wrong). The script it serves:

```
#!ipxe
dhcp
set initiator-iqn iqn.2026-08.lan.local:rpi-dev-client
sanboot iscsi:<DEV_HOST>:::1:iqn.2026-08.lan.local:rpi-dev
```

iPXE attaches the LUN, chainloads its ESP, and Linux re-attaches the same root
itself (`iscsi_portal=` cmdline). The proxy-DHCP/PXE handshake is unreliable if
the dev host is on wifi with power saving on - turn it off (see "The loop").

## First-time setup

1. optionally create `mkosi.local.conf` with `DEV_HOST` (see above).
2. `mkosi --profile dev`
3. Write the fallback boot medium (also self-updates the EEPROM on first boot):

   ```bash
   tools/iscsi/burn-boot /dev/disk/by-id/usb-<...>
   ```

   A bare FAT with `boot.img` + `boot.sig` + `config.txt` (`boot.img` carries
   iPXE), for when HTTP boot of `boot.img` isn't available.

## The loop

Three terminals from the repo root, all left running:

```bash
mkosi box -- tools/iscsi/serve-iscsi
```

```bash
mkosi --profile dev serve
```

```bash
DEV_HOST=192.168.1.8 sudo mkosi box -- tools/iscsi/serve-netboot
```

Then `mkosi --profile dev` to rebuild, and reboot the Pi. `serve` re-serves
`mkosi.output` over HTTP:8081; `serve-iscsi` (target `iqn.2026-08.lan.local:rpi-dev`,
LUN 1, TCP 3260) re-points the LUN once the Pi disconnects - so a rebuild
mid-session can't corrupt it. Open 3260 to the Pi's subnet if the host
firewalls it.

If the host serves iSCSI over **wifi**, turn its power saving off or the
session stalls drop the connection under read load (`conn error (1020)`,
`I/O error ... dev sda`), which breaks userspace even though the boot itself
succeeded:

```bash
sudo iw dev <wlan-iface> set power_save off
```

(The initrd already sets a long `replacement_timeout` and disarms NOP-out so a
brief stall blocks I/O instead of failing it - but wired is much better.)

## serve-iscsi config (env)

- `IQN` - target name (default `iqn.2026-08.lan.local:rpi-dev`).
- `MKOSI_OUTPUT` - output directory (default `mkosi.output`).
- `POLL` - output-dir poll interval in seconds (default `5`).
- `LUN_SIZE` - the served LUN is padded (sparse) to this so first-boot
  `systemd-repart` can add the root partition (default `8G`).
- `TGT_IPC_SOCKET` - tgt control socket path (default `mkosi.output/tgt.ipc`;
  `mkosi box` mounts `/run` read-only so tgt's default can't be used).

## Notes

- A private copy of the newest `system*.raw` is served as `iscsi-lun.img`
  (sparse, padded to `LUN_SIZE`), so a running Pi (which writes to its disk,
  incl. first-boot repart) and a fresh build never collide.
- `tgtd` logs a few harmless lines on start under `mkosi box`: `Failed to
  initialize RDMA`, `can't adjust oom-killer`, `can't adjust nr_open`.
- If a `tgtd` is ever left behind: `pkill -9 tgtd`.
