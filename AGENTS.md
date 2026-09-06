# Agent notes

## Commits

- Do not add a `Co-Authored-By` trailer.

## Code style

- Don't write comments unless the WHY is non-obvious (a hidden constraint, a
  workaround for a specific bug, something that would surprise a reader).
  Never explain WHAT the code does - well-named identifiers already do that.

## Enabling units in the initrd

mkosi runs `systemctl --root=… preset-all` at build time for the initrd image
too, so `enable <unit>` presets *do* take effect there - but only from the
right directory. Because the initrd ships `/etc/initrd-release` and an
`usr/lib/systemd/initrd-preset/` dir, systemctl reads presets exclusively from
`initrd-preset/` and ignores `system-preset/` entirely. Put initrd `enable`
lines in `mkosi.images/initrd/mkosi.extra/usr/lib/systemd/initrd-preset/`.

Second gotcha: a preset only creates the `.wants` symlink for the unit's
`[Install] WantedBy=`. Stock units often say `WantedBy=multi-user.target`,
which the initrd (booting through `initrd.target`) never reaches - add a
drop-in retargeting `[Install]` at `sysinit.target`/`sockets.target`/etc.

## Referring to disks

Always use `/dev/disk/by-id/<...>`, never a bare `/dev/sdX`. This applies
equally on the dev host and when SSHed into the Pi itself - device letters
are not stable across reboots/replugs on either side, and the same drive can
enumerate differently between the two machines.

## Flashing to physical media

Use `mkosi burn /dev/disk/by-id/<...>` instead of manually `dd`-ing
`mkosi.output/system_dev.raw`. It builds if needed and writes to the device,
and - unlike a raw `dd` - corrects the GPT to match the actual disk's sector
count and size. Manual `dd` leaves a GPT sized for the (much smaller)
build-time image; on a disk that was previously flashed and had its root
partition grown by `systemd-repart` on first boot, a partial wipe (e.g. only
the primary GPT header) can let the old partition table - and the multi-GB
root partition already on it - resurface, so the next boot silently reuses
stale state instead of provisioning fresh.
