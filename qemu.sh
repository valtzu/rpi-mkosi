#!/bin/sh -ex

qemu-system-aarch64 \
  -machine virt \
  -cpu cortex-a72 \
  -smp 1 \
  -m 4G \
  -object rng-random,filename=/dev/urandom,id=rng0 \
  -device virtio-rng-pci,rng=rng0,id=rng-device0 \
  -nic user,model=virtio-net-pci \
  -nographic -nodefaults \
  -chardev stdio,mux=on,id=console,signal=off \
  -serial chardev:console -mon console \
  -drive if=pflash,format=raw,readonly=on,file=AAVMF_CODE.fd \
  -drive file=AAVMF_VARS.fd,if=pflash,format=raw \
  -smbios 'type=11,value=io.systemd.stub.kernel-cmdline-extra=systemd.wants=network.target module_blacklist=vmw_vmci systemd.tty.term.ttyAMA0=xterm-256color systemd.tty.columns.ttyAMA0=308 systemd.tty.rows.ttyAMA0=18 ip=enc0:any ip=enp0s1:any ip=enp0s2:any ip=host0:any ip=none loglevel=4 SYSTEMD_SULOGIN_FORCE=1 systemd.tty.term.console=xterm-256color systemd.tty.columns.console=308 systemd.tty.rows.console=18 console=ttyAMA0 TERM=xterm-256color systemd.mount-extra=LABEL=scratch:/var/tmp:ext4' -smbios 'type=11,value=io.systemd.boot.kernel-cmdline-extra=systemd.wants=network.target module_blacklist=vmw_vmci systemd.tty.term.ttyAMA0=xterm-256color systemd.tty.columns.ttyAMA0=308 systemd.tty.rows.ttyAMA0=18 ip=enc0:any ip=enp0s1:any ip=enp0s2:any ip=host0:any ip=none loglevel=4 SYSTEMD_SULOGIN_FORCE=1 systemd.tty.term.console=xterm-256color systemd.tty.columns.console=308 systemd.tty.rows.console=18 console=ttyAMA0 TERM=xterm-256color systemd.mount-extra=LABEL=scratch:/var/tmp:ext4' \
  -drive if=none,id=mkosi,file=/home/vagrant/build/mkosi.output/system_dev.raw,format=raw

# qemu-system-aarch64 -nographic -M virt -cpu cortex-a72 -m 4G -pflash AAVMF_CODE.fd -pflash AAVMF_VARS.fd disk.img
