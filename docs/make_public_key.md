sudo mkdir -p /etc/phonehome
sudo ssh-keygen -t ed25519 -f /etc/phonehome/id_ed25519 -N ""
sudo chmod 600 /etc/phonehome/id_ed25519