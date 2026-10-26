#!/bin/bash
# mkrepo.sh DIR : make a fresh sysrepo repository in DIR with the netopeer2
# modules and labsw-switch installed, for one user, without root.
# Needs: sysrepoctl/sysrepocfg/netopeer2 scripts of the stack on PATH or in $LABSW_PREFIX.
# SPDX-License-Identifier: MIT
set -euo pipefail
dir=$(realpath -m "$1")
prefix=${LABSW_PREFIX:-$HOME/opt/labsw-host}
here=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$dir"
export SYSREPO_REPOSITORY_PATH=$dir
export SYSREPO_SHM_PREFIX=${SYSREPO_SHM_PREFIX:-lswt$$}
export SYSREPOCTL_EXECUTABLE=$prefix/bin/sysrepoctl SYSREPOCFG_EXECUTABLE=$prefix/bin/sysrepocfg
export OPENSSL_EXECUTABLE=$(command -v openssl)
export NP2_MODULE_DIR=$prefix/share/yang/modules/netopeer2 LN2_MODULE_DIR=$prefix/share/yang/modules/libnetconf2
export NP2_MODULE_PERMS=600 SR_NOTIFD_MODULE_DIR=$prefix/share/yang/modules/sysrepo SR_NOTIFD_MODULE_PERMS=600
bash "$prefix/share/sysrepo/scripts/setup_notifd.sh" >/dev/null
bash "$prefix/share/netopeer2/scripts/setup.sh" >/dev/null
bash "$prefix/share/netopeer2/scripts/merge_hostkey.sh" >/dev/null
"$SYSREPOCTL_EXECUTABLE" -i "$here"/yang/labsw-switch@*.yang -p 600
