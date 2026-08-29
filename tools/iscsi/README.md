# iSCSI dev target

Serves the latest built image (`mkosi.output/system*.raw`) over iSCSI so the Pi
can netboot it instead of a physical USB disk.

`tgt` is installed into the default tools tree (`ToolsTreePackages=` in the
top-level `mkosi.conf`), and `mkosi box` keeps host networking - so this
needs no root and no container runtime.

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
