# Agent notes

## Commits

- Do not add a `Co-Authored-By` trailer.

## Code style

- Don't write comments unless the WHY is non-obvious (a hidden constraint, a
  workaround for a specific bug, something that would surprise a reader).
  Never explain WHAT the code does - well-named identifiers already do that.

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
