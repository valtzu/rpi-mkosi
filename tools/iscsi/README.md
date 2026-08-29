# iSCSI dev target

Boot the Pi from a locally served image instead of reflashing a USB disk.
`mkosi --profile dev` builds an image whose EDK2 attaches root over iSCSI;
`serve-iscsi` on this machine serves it.

`tgt` is installed into the default tools tree (`ToolsTreePackages=` in the
top-level `mkosi.conf`), and `mkosi box` keeps host networking - so this
needs no root and no container runtime.

## Per-machine config: `mkosi.local.conf`

`mkosi --profile dev` needs an untracked `mkosi.local.conf` at the repo root:

```
[Build]
Environment=
    PI_MAC=dc:a6:32:00:00:00
```

- `PI_MAC` - the Pi's Ethernet MAC. `mkosi.finalize` runs
  `tools/iscsi/edk2-iscsi-efivars` to bake a static iSCSI "Attempt 1" into the
  UEFI varstore in `boot.img`; EDK2 only loads it if this MAC matches the NIC.
- `DEV_HOST` - optional; the iSCSI portal, defaults to this machine's primary
  IP (`hostname -I`). Set it only if that picks the wrong interface.

EDK2 takes iSCSI config only from NVRAM or DHCP, and the RPi UEFI runs from a
signed ramdisk so menu edits never persist - hence baking the variable.

The dev profile also sets `BOOT_ORDER=0xf147` (HTTP, then USB, then SD) and
`HTTP_HOST`/`HTTP_PORT=8081` in the EEPROM config, so the Pi HTTP-boots
`boot.img` straight from `mkosi serve`.

## First-time setup

1. create `mkosi.local.conf` with `PI_MAC` (see above).
2. `mkosi --profile dev`
3. Write the fallback boot medium (also self-updates the EEPROM on first boot):

   ```bash
   tools/iscsi/burn-boot /dev/disk/by-id/usb-<...>
   ```

   This is a bare FAT partition with `boot.img` + `boot.sig` + `config.txt` -
   no OS ESP, so EDK2 has no local boot target and uses the iSCSI attempt.

## The loop

Two terminals from the repo root, both left running:

```bash
mkosi box -- tools/iscsi/serve-iscsi
```

```bash
mkosi --profile dev serve
```

Then `mkosi --profile dev` to rebuild, and reboot the Pi. `serve` re-serves
`mkosi.output` over HTTP:8081; `serve-iscsi` (target `iqn.2026-08.lan.local:rpi-dev`,
LUN 1, TCP 3260) re-points the LUN once the Pi disconnects - so a rebuild
mid-session can't corrupt it. Open 3260 to the Pi's subnet if the host
firewalls it.

## serve-iscsi config (env)

- `IQN` - target name (default `iqn.2026-08.lan.local:rpi-dev`).
- `MKOSI_OUTPUT` - output directory (default `mkosi.output`).
- `POLL` - output-dir poll interval in seconds (default `5`).
- `TGT_IPC_SOCKET` - tgt control socket path (default `mkosi.output/tgt.ipc`;
  `mkosi box` mounts `/run` read-only so tgt's default can't be used).

## Notes

- A private copy of the newest `system*.raw` is served as `iscsi-lun.img`, so a
  running Pi (which writes to its disk) and a fresh build never collide.
- `tgtd` logs a few harmless lines on start under `mkosi box`: `Failed to
  initialize RDMA`, `can't adjust oom-killer`, `can't adjust nr_open`.
- If a `tgtd` is ever left behind: `pkill -9 tgtd`.
