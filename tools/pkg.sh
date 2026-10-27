#!/bin/bash
# pkg.sh STAGE AGENT_BUILD_DIR [OUT] : make the bundle that deploy/install.sh installs
#   STAGE            DESTDIR of the cross-built stack (libyang, libnetconf2, sysrepo, netopeer2): STAGE/opt/labsw
#   AGENT_BUILD_DIR  a build of this repository for the device (LABSW_TESTING=OFF)
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
stage=$(realpath "$1"); build=$(realpath "$2"); out=$(realpath -m "${3:-$here/dist/labsw-bundle.tgz}")
# a test build has the mock adapter and fault injection: never ship one. Asked from the
# build's cache, not by running labswd (an aarch64 program does not run on the PC)
if ! grep -qx "LABSW_TESTING:BOOL=OFF" "$build/CMakeCache.txt" 2>/dev/null; then
  echo "pkg.sh: $build is not a build with LABSW_TESTING=OFF (see its CMakeCache.txt)" >&2; exit 1
fi
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
d=$t/labsw; mkdir -p "$d/yang" "$d/systemd"
[ -x "$stage/opt/labsw/sbin/netopeer2-server" ] || { echo "pkg.sh: no $stage/opt/labsw/sbin/netopeer2-server" >&2; exit 1; }
tar -C "$stage" --owner=0 --group=0 -czf "$d/labsw-stack.tgz" opt/labsw
cp "$build/labswd" "$here/tools/lab_up.sh" "$here/tools/lab_down.sh" "$here/deploy/install.sh" "$here/deploy/nacm_admin.xml" \
   "$here/tools/labsw-upgrade" "$here/tools/labsw-upgrade-check" "$d/"
cp "$here"/yang/*.yang "$d/yang/"
cp "$here"/deploy/systemd/*.service "$d/systemd/"
# every file of the bundle (install.sh and the units run as root too); this catches a
# file broken on the way, not a forged bundle: compare it with a hash you got elsewhere
( cd "$d" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS )
git -C "$here" describe --always --dirty > "$d/VERSION"
mkdir -p "$(dirname "$out")"
tar -C "$t" -czf "$out" labsw
echo "$out ($(du -h "$out" | cut -f1)), $(cat "$d/VERSION")"
