# -*- mode: ruby -*-
# vi: set ft=ruby :

$VAGRANT_EXTRA_STEPS = <<~SCRIPT
  git config --global --add safe.directory '*'
  echo "cd /vagrant" >> /home/vagrant/.bashrc
SCRIPT

$SET_NETWORK = <<~'SCRIPT'
  IFNAME=$(ifconfig | grep -B1 10.0.1. | grep -o "^\w*")
  echo "export IFNAME=$IFNAME" >> /home/vagrant/.bashrc
  sudo echo "export IFNAME=$IFNAME" >> /root/.bashrc

  sudo tcset $IFNAME --rate 100Mbps --delay 20ms
  sudo sed -i 's/PasswordAuthentication no/PasswordAuthentication yes/g' \
      /etc/ssh/sshd_config
  sudo service sshd restart
SCRIPT

Vagrant.configure(2) do |config|
  config.ssh.forward_agent = true
  config.vm.synced_folder "./foggytcp", "/vagrant/foggytcp/"
  config.vm.provision "shell",
        inline: "sudo sh /vagrant/foggytcp/setup/docker-setup.sh"
  config.vm.provision "shell", inline: $VAGRANT_EXTRA_STEPS

  config.ssh.insert_key = false

  config.vm.provider "vmware_desktop" do |v, override|
    override.vm.box = "bento/debian-11"
  end

  config.vm.define :client, primary: true do |host|
    host.vm.hostname = "client"
    host.vm.provision "shell", inline: $SET_NETWORK
  end

  config.vm.define :server do |host|
    host.vm.hostname = "server"
    host.vm.provision "shell", inline: $SET_NETWORK
  end
end

