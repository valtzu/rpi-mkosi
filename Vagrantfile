Vagrant.configure("2") do |config|
  config.vm.provider "virtualbox" do |v|
    v.memory = 8096
    v.cpus = 2
  end
  config.vm.define "vagrant"
  config.vm.box = "ubuntu/lunar64"
  config.vm.provision:shell, inline: <<-SETUP
    echo "deb-src http://archive.ubuntu.com/ubuntu/ $(lsb_release -cs) main restricted universe multiverse" | sudo tee -a /etc/apt/sources.list
    apt-get update
    apt-get install -y --no-install-recommends python3 python3-pip python-is-python3 python3-pyelftools python3-pefile pipx \
      qemu-user-static bubblewrap dosfstools mtools uidmap libfdisk-dev libtss2-dev
    apt-get build-dep -y systemd
    sudo -u vagrant pipx ensurepath
    sudo -u vagrant pipx install git+https://github.com/systemd/mkosi.git@v17.1
    sudo -u vagrant git clone --branch=v254 https://github.com/systemd/systemd --depth=1 /home/vagrant/systemd
    sudo -u vagrant meson setup /home/vagrant/systemd/build /home/vagrant/systemd \
        -D repart=true \
        -D efi=true \
        -D bootloader=true \
        -D ukify=true \
        -D firstboot=true \
        -D blkid=true \
        -D openssl=true \
        -D tpm2=true

    BINARIES=(
      bootctl
      systemctl
      systemd-dissect
      systemd-firstboot
      systemd-measure
      systemd-nspawn
      systemd-repart
      ukify
    )

    sudo -u vagrant ninja -C /home/vagrant/systemd/build ${BINARIES[@]}

    for BINARY in "${BINARIES[@]}"; do
      sudo -u vagrant ln -svf /home/vagrant/systemd/build/$BINARY /home/vagrant/.local/bin/$BINARY
    done
    sudo -u vagrant mkdir -p /home/vagrant/build
    echo "rsync -r --exclude=.git --exclude=.vagrant /vagrant/ /home/vagrant/build" >> /home/vagrant/.bashrc
    echo "cd /home/vagrant/build" >> /home/vagrant/.bashrc
    SETUP
end
