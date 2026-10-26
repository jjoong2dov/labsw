#!/bin/bash
# install.sh <NETCONF user> : on the device (Pi OS / Debian 13 arm64), as root,
# from an unpacked labsw bundle (tools/pkg.sh). Safe to run again.
# SPDX-License-Identifier: MIT
set -euo pipefail
NC_USER=${1:?usage: install.sh <NETCONF user>}
P=/opt/labsw
cd "$(dirname "$0")"
apt-get update -qq
apt-get install -y -qq libssh-4 libcurl4t64 libjson-c5 libpcre2-8-0 openssl iproute2 frr >/dev/null
tar -C / --no-same-owner -xzf labsw-stack.tgz
install -m 755 labswd lab_up.sh lab_down.sh $P/sbin/
install -d $P/share/yang/modules/labsw
install -m 644 yang/*.yang $P/share/yang/modules/labsw/
echo $P/lib > /etc/ld.so.conf.d/labsw.conf
ldconfig
install -m 644 systemd/*.service /etc/systemd/system/
export PATH=$P/bin:$P/sbin:$PATH
export SYSREPOCTL_EXECUTABLE=$P/bin/sysrepoctl SYSREPOCFG_EXECUTABLE=$P/bin/sysrepocfg OPENSSL_EXECUTABLE=/usr/bin/openssl
SR_NOTIFD_MODULE_DIR=$P/share/yang/modules/sysrepo SR_NOTIFD_MODULE_PERMS=600 bash $P/share/sysrepo/scripts/setup_notifd.sh >/dev/null
NP2_MODULE_DIR=$P/share/yang/modules/netopeer2 LN2_MODULE_DIR=$P/share/yang/modules/libnetconf2 NP2_MODULE_PERMS=600 \
  bash $P/share/netopeer2/scripts/setup.sh >/dev/null
bash $P/share/netopeer2/scripts/merge_hostkey.sh >/dev/null
bash $P/share/netopeer2/scripts/merge_config.sh >/dev/null
for m in $P/share/yang/modules/labsw/*.yang; do
  name=$(basename "$m" .yang); name=${name%@*}
  if sysrepoctl -l | grep -q "^$name "; then sysrepoctl -U "$m"; else sysrepoctl -i "$m"; fi
done
sed "s/NC_USER/$NC_USER/" nacm_admin.xml > /tmp/labsw-nacm.xml
sysrepocfg --import=/tmp/labsw-nacm.xml -d startup -m ietf-netconf-acm -f xml
sysrepocfg --import=/tmp/labsw-nacm.xml -d running -m ietf-netconf-acm -f xml
rm -f /tmp/labsw-nacm.xml
systemctl daemon-reload
systemctl enable labsw-lab.service netopeer2.service labswd.service
systemctl restart labsw-lab.service netopeer2.service labswd.service
systemctl --no-pager --lines=0 status labsw-lab netopeer2 labswd | grep -E "●|Active:"
