Vagrant.configure("2") do |config|
  config.vm.provider "virtualbox" do |v|
    v.customize [ "modifyvm", :id, "--uartmode1", "disconnected" ]
    v.memory = 8096
    v.cpus = 2
  end
  config.vm.define "vagrant"
  config.vm.box = "bento/ubuntu-24.04"
  config.vm.provision:shell, privileged: false, reboot: true, inline: <<-SETUP
    export DEBIAN_FRONTEND=noninteractive
    sudo apt-get update
    echo 'grub-pc hold' | sudo dpkg --set-selections
    sudo apt-get install -yq --no-install-recommends python3 python3-pip python-is-python3 python3-pyelftools python3-pefile pipx \
      qemu-user-static binfmt-support dosfstools mtools uidmap libfdisk-dev libtss2-dev libssl-dev debian-archive-keyring \
      binutils-aarch64-linux-gnu jq
    sudo apt-get upgrade -yq

    mkdir -p ~/build ~/.local/bin
    echo "rsync --links -r --exclude=.git --exclude=.vagrant --exclude=mkosi.cache --exclude=mkosi.output --exclude=mkosi.builddir --exclude=mkosi.workspace --exclude='*.raw*' --exclude=cache --delete /vagrant/ ~/build" > ~/.local/bin/vagrant-rsync
    chmod u+x ~/.local/bin/vagrant-rsync
    echo "~/.local/bin/vagrant-rsync" >> ~/.bashrc
    echo "cd ~/build" >> ~/.bashrc

    pipx ensurepath
    pipx install git+https://github.com/systemd/mkosi.git@v25.3
    SETUP
  config.vm.provision:shell, privileged: true, run: 'always', inline: <<-SETUP
    sysctl --ignore --write kernel.apparmor_restrict_unprivileged_unconfined=0
    sysctl --ignore --write kernel.apparmor_restrict_unprivileged_userns=0
    SETUP
end
