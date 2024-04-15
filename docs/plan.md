- `systemd-tmpfiles-setup.service` in initramfs should populate `/etc` on `/sysroot`, and thus,
  must be run with `--root=/sysroot` and `After=systemd-repart`. To do this, extra initramfs is
  needed.
- Add empty B partition + verity on first boot
- Include pieeprom.upd to add public key to eeprom?
