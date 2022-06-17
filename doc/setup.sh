
# install required packages
apt install mosquitto jq

# install portal software
mkdir /opt/portal300 # base folder for everything

git clone https://github.com/shackspace/portal300

# install mosquitto
echo '# Configuration for shackspace Portal 300

# Listen on the MQTTS port
listener 8883

# TLS certificate
cafile   /etc/mosquitto/ca_certificates/shack-portal.crt
certfile /etc/mosquitto/certs/server.crt
keyfile  /etc/mosquitto/certs/server.key

# Client requirements
require_certificate      true  # we require the use of client certificates
use_subject_as_username  true  # we use the CN of the client certificate as the user name
use_identity_as_username false # we dont use the full certificate identity as user name
' > /etc/mosquitto/conf.d/portal.conf

systemctl enable mosquitto

# setup portal users:
for user in open-front open-back close; do
  useradd -m "${user}"
  mkdir "/home/${user}/.ssh"
  chown "${user}:${user}" "/home/${user}/.ssh"
  touch "/home/${user}/.ssh/authorized_keys"
  chown "${user}:${user}" "/home/${user}/.ssh/authorized_keys"
done

# disable access for any "rogue" users:
echo 'AllowUsers open-front open-back close root' >> /etc/ssh/sshd_config



# command="",no-port-forwarding,no-X11-forwarding,no-agent-forwarding ${key}
#
# environment for this command looks roughly like this:
# [felix@denkplatte-v2 ~]$ ssh open-back@10.42.20.137 foo bar bam
# USER=open-back
# LANGUAGE=en_US:en
# SSH_CLIENT=192.168.66.13 48394 22
# XDG_SESSION_TYPE=tty
# HOME=/home/open-back
# MOTD_SHOWN=pam
# LOGNAME=open-back
# XDG_SESSION_CLASS=user
# SSH_ORIGINAL_COMMAND=foo bar bam
# XDG_SESSION_ID=26
# PATH=/usr/local/bin:/usr/bin:/bin:/usr/games
# XDG_RUNTIME_DIR=/run/user/1002
# LANG=en_US.UTF-8
# SHELL=/bin/sh
# PWD=/home/open-back
# SSH_CONNECTION=192.168.66.13 48394 10.42.20.137 22