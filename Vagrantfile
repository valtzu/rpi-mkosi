Vagrant.configure("2") do |config|
  config.vm.provider "virtualbox" do |v|
    v.memory = 8096
    v.cpus = 2
  end
  config.vm.define "vagrant"
  config.vm.box = "debian/testing64"
  config.vm.provision:shell, inline: <<-SETUP
    apt-get update
    apt-get install -y --no-install-recommends rsync qemu-user-static mkosi uidmap zstd systemd-boot dosfstools mtools e2fsprogs python3-pefile curl binutils
    echo "rsync --links -r --exclude=.git --exclude=.vagrant /vagrant/ /home/vagrant/build" >> /home/vagrant/.bashrc
    echo "cd /home/vagrant/build" >> /home/vagrant/.bashrc
    SETUP
end
