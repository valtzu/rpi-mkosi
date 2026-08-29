# iSCSI dev target

Boot the Pi from a locally served image instead of reflashing a USB disk.
`mkosi --profile dev` builds an image whose EDK2 attaches root over iSCSI;
`serve-iscsi` on this machine serves it.

`tgt` is installed into the default tools tree (`ToolsTreePackages=` in the
top-level `mkosi.conf`), and `mkosi box` keeps host networking - so this
needs no root and no container runtime.

## Per-machine config: `dev.local.conf`

`mkosi --profile dev` needs an untracked `dev.local.conf` at the repo root
(copy `dev.local.conf.example`):

- `DEV_HOST` - this machine's LAN IP (the iSCSI portal).
- `PI_MAC` - the Pi's Ethernet MAC. `mkosi.finalize` runs
  `tools/iscsi/edk2-iscsi-efivars` to bake a static iSCSI "Attempt 1" into the
  UEFI varstore in `boot.img`; EDK2 only loads it if this MAC matches the NIC.

EDK2 takes iSCSI config only from NVRAM or DHCP, and the RPi UEFI runs from a
signed ramdisk so menu edits never persist - hence baking the variable.

## Usage

From the repo root, leave this running:

```bash
mkosi box -- tools/iscsi/serve-iscsi
```

Target: `iqn.2026-08.lan.local:rpi-dev`, LUN 1, TCP `3260` on the host. Open
3260 to the Pi's subnet if the host firewalls it. Stop with Ctrl-C.

It watches `mkosi.output` and re-points the LUN at each new build automatically -
but only while no initiator is connected, since a live swap would corrupt the
Pi. So the loop is just: `mkosi` in one terminal, reboot the Pi, done.

## Config (env)

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
