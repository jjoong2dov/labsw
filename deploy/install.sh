#!/bin/bash
# install.sh <NETCONF user> : on the device (Pi OS / Debian 13 arm64), as root,
# from an unpacked labsw bundle (tools/pkg.sh). Safe to run again.
# SPDX-License-Identifier: MIT
set -euo pipefail
# --allow-loss: upgrade a module even if its new revision loses configured values.
NC_USER=${1:?usage: install.sh <NETCONF user> [--allow-loss]}
ALLOW_LOSS=${2:-}
[ -z "$ALLOW_LOSS" ] || [ "$ALLOW_LOSS" = --allow-loss ] || { echo "usage: install.sh <NETCONF user> [--allow-loss]" >&2; exit 1; }
P=/opt/labsw
cd "$(dirname "$0")"
apt-get update -qq
apt-get install -y -qq libssh-4 libcurl4t64 libjson-c5 libpcre2-8-0 openssl iproute2 frr >/dev/null
tar -C / --no-same-owner -xzf labsw-stack.tgz
echo $P/lib > /etc/ld.so.conf.d/labsw.conf
ldconfig
install -m 755 labsw-upgrade labsw-upgrade-check labsw-dump $P/sbin/
export PATH=$P/bin:$P/sbin:$PATH
rev_of() { sysrepoctl -l | awk -F'|' -v n="$1" '{gsub(/ /,"",$1); gsub(/ /,"",$2)} $1==n {print $2}'; }
# every module whose revision changes is checked before labswd and the modules change:
# a refused upgrade stops the install with the old ones in place
for m in yang/*.yang; do
  name=$(basename "$m" .yang); rev=${name#*@}; name=${name%@*}
  have=$(rev_of "$name")
  [ -n "$have" ] && [ "$have" != "$rev" ] || continue
  rc=0; labsw-upgrade-check "$m" || rc=$?
  [ $rc -eq 0 ] || { [ $rc -eq 3 ] && [ -n "$ALLOW_LOSS" ]; } && continue
  echo "install.sh: cannot upgrade $name $have -> $rev safely (see above); labswd and the modules are unchanged" >&2
  [ $rc -ne 3 ] || echo "install.sh: run again with --allow-loss to accept the loss" >&2
  exit $rc
done
install -m 755 labswd lab_up.sh lab_down.sh $P/sbin/
install -d $P/share/yang/modules/labsw
install -m 644 yang/*.yang $P/share/yang/modules/labsw/
install -m 644 systemd/*.service /etc/systemd/system/
export SYSREPOCTL_EXECUTABLE=$P/bin/sysrepoctl SYSREPOCFG_EXECUTABLE=$P/bin/sysrepocfg OPENSSL_EXECUTABLE=/usr/bin/openssl
SR_NOTIFD_MODULE_DIR=$P/share/yang/modules/sysrepo SR_NOTIFD_MODULE_PERMS=600 bash $P/share/sysrepo/scripts/setup_notifd.sh >/dev/null
NP2_MODULE_DIR=$P/share/yang/modules/netopeer2 LN2_MODULE_DIR=$P/share/yang/modules/libnetconf2 NP2_MODULE_PERMS=600 \
  bash $P/share/netopeer2/scripts/setup.sh >/dev/null
bash $P/share/netopeer2/scripts/merge_hostkey.sh >/dev/null
bash $P/share/netopeer2/scripts/merge_config.sh >/dev/null
# the modules of this bundle only (the directory may hold files of an older install);
# a module already there with the same revision is left as it is; a new revision goes
# through labsw-upgrade (saves what is needed, its last line is the rollback command)
for m in yang/*.yang; do
  name=$(basename "$m" .yang); rev=${name#*@}; name=${name%@*}
  have=$(rev_of "$name")
  if [ -z "$have" ]; then sysrepoctl -i "$P/share/yang/modules/labsw/$(basename "$m")"
  elif [ "$have" != "$rev" ]; then labsw-upgrade "$P/share/yang/modules/labsw/$(basename "$m")" $ALLOW_LOSS
  fi
done
sed "s/NC_USER/$NC_USER/" nacm_admin.xml > /tmp/labsw-nacm.xml
sysrepocfg --import=/tmp/labsw-nacm.xml -d startup -m ietf-netconf-acm -f xml
sysrepocfg --import=/tmp/labsw-nacm.xml -d running -m ietf-netconf-acm -f xml
rm -f /tmp/labsw-nacm.xml
systemctl daemon-reload
systemctl enable labsw-lab.service netopeer2.service labswd.service
systemctl restart labsw-lab.service netopeer2.service labswd.service
systemctl --no-pager --lines=0 status labsw-lab netopeer2 labswd | grep -E "●|Active:"
