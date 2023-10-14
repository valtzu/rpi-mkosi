Vagrant.configure("2") do |config|
  config.vm.provider "virtualbox" do |v|
    v.customize [ "modifyvm", :id, "--uartmode1", "disconnected" ]
    v.memory = 8096
    v.cpus = 2
  end
  config.vm.define "vagrant"
  config.vm.box = "ubuntu/jammy64"
  config.vm.provision:shell, privileged: false, reboot: true, inline: <<-SETUP
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends linux-image-6.2.0-26-generic python3 python3-pip python-is-python3 python3-pyelftools python3-pefile pipx \
      qemu-user-static binfmt-support bubblewrap dosfstools mtools uidmap libfdisk-dev libtss2-dev libssl-dev debian-archive-keyring
    sudo apt-get upgrade -y

    mkdir -p ~/build ~/.local/bin
    echo "rsync --links -r --exclude=.git --exclude=.vagrant --exclude=mkosi.cache --exclude=mkosi.output --exclude=mkosi.builddir --exclude=mkosi.workspace /vagrant/ ~/build" > ~/.local/bin/vagrant-rsync
    chmod u+x ~/.local/bin/vagrant-rsync
    echo "~/.local/bin/vagrant-rsync" >> ~/.bashrc
    echo "cd ~/build" >> ~/.bashrc

    pipx ensurepath
    pipx install git+https://github.com/systemd/mkosi.git@v18
    SETUP
end
