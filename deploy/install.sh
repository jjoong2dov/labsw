#!/bin/bash
# install.sh <NETCONF user> : on the device (Pi OS / Debian 13 arm64), as root,
# from an unpacked labsw bundle (tools/pkg.sh). Safe to run again: what is already
# configured (NACM groups, server users, data) is merged into, not replaced.
# <NETCONF user> is an account of the device; it may log in with the keys in its
# ~/.ssh/authorized_keys and is put in the NACM admin group.
# SPDX-License-Identifier: MIT
set -euo pipefail
NC_USER=${1:?usage: install.sh <NETCONF user>}
id "$NC_USER" >/dev/null 2>&1 || { echo "install.sh: no account $NC_USER on this device" >&2; exit 1; }
P=/opt/labsw
cd "$(dirname "$0")"
apt-get update -qq
apt-get install -y -qq libssh-4 libcurl4t64 libjson-c5 libpcre2-8-0 openssl iproute2 frr python3 >/dev/null
# no NETCONF edits while the stack and the modules change (the upgrade rehearsal is not a
# snapshot), and no running program on libraries being replaced; started again at the end
systemctl stop netopeer2.service labswd.service 2>/dev/null || true
tar -C / --no-same-owner -xzf labsw-stack.tgz
install -m 755 lab_up.sh lab_down.sh labsw-upgrade labsw-upgrade-check $P/sbin/
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
# merge_config.sh lets in only the user who ran sudo (and only on a first install):
# add <NETCONF user> to every SSH endpoint, with the keys of that account
users=$(mktemp)
{ printf '<netconf-server xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-server"><listen><endpoints>'
  sysrepocfg -X -x "/ietf-netconf-server:netconf-server/listen/endpoints/endpoint[ssh]/name" -d running -f xml |
    sed -n 's|.*<name>\(.*\)</name>.*|\1|p' | while read -r ep; do
      printf '<endpoint><name>%s</name><ssh><ssh-server-parameters><client-authentication><users><user><name>%s</name>' "$ep" "$NC_USER"
      printf '<public-keys><use-system-keys xmlns="urn:cesnet:libnetconf2-netconf-server"/></public-keys>'
      printf '</user></users></client-authentication></ssh-server-parameters></ssh></endpoint>'
    done
  printf '</endpoints></listen></netconf-server>\n'; } > "$users"
sysrepocfg --edit="$users" -d startup -m ietf-netconf-server -f xml
sysrepocfg --edit="$users" -d running -m ietf-netconf-server -f xml
rm -f "$users"
# the modules of this bundle only (the directory may hold files of an older install);
# a module already there with the same revision is left as it is, a new revision goes
# through labsw-upgrade (rehearsal on a copy, saved old state; stops on data loss
# unless LABSW_ALLOW_LOSS=1, exactly). The new agent is installed only after this.
for m in yang/*.yang; do
  name=$(basename "$m" .yang); rev=${name#*@}; name=${name%@*}
  have=$(sysrepoctl -l | awk -F'|' -v n="$name" '{gsub(/ /,"",$1); gsub(/ /,"",$2)} $1==n {print $2}')
  if [ -z "$have" ]; then sysrepoctl -i "$P/share/yang/modules/labsw/$(basename "$m")"
  elif [ "$have" != "$rev" ]; then
    allow=; [ "${LABSW_ALLOW_LOSS:-0}" = 1 ] && allow=--allow-loss   # exactly 1, not any value
    $P/sbin/labsw-upgrade "$P/share/yang/modules/labsw/$(basename "$m")" $allow
  fi
done
install -m 755 labswd $P/sbin/
# merge the admin group and rule (--edit), do not replace the module (--import): groups
# and rules added on the device must survive running this script again
nacm=$(mktemp); sed "s/NC_USER/$NC_USER/" nacm_admin.xml > "$nacm"
sysrepocfg --edit="$nacm" -d startup -m ietf-netconf-acm -f xml
sysrepocfg --edit="$nacm" -d running -m ietf-netconf-acm -f xml
rm -f "$nacm"
systemctl daemon-reload
systemctl enable labsw-lab.service netopeer2.service labswd.service
systemctl restart labsw-lab.service netopeer2.service labswd.service
systemctl --no-pager --lines=0 status labsw-lab netopeer2 labswd | grep -E "●|Active:"
