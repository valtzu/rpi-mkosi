### Setup dev env
```bash
vagrant up
vagrant ssh
```

### Generate Secure Boot keys
``` 
mkosi --directory="" genkey

# If using Vagrant with rsync, copy keys back to host so you don't lose them
cp mkosi.key mkosi.crt /vagrant/ 
```

### Build image for Raspberry Pi
```
mkosi
```

### Run on host
```
mkosi --architecture=x86-64 --compress-output=no --force qemu
```
