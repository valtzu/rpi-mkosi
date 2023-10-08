### Build image for Raspberry Pi
```
vagrant up
vagrant ssh
mkosi --preset=tools
mkosi
```

### Run on host
```
mkosi --architecture=x86-64 --compress-output=no --preset=system --package=linux-image-generic --force qemu
```
