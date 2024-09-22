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


### How to create credentials

See https://www.freedesktop.org/software/systemd/man/latest/systemd.system-credentials.html

**NOTE:** --with-key=null requires systemd v256

```
systemd-creds encrypt --with-key=null --name=network.hosts <(echo 127.0.0.1 localhost) /efi/loader/credentials/network.hosts.cred
```
