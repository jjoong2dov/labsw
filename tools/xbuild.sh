#!/bin/bash
# xbuild.sh [build-dir] : cross-build labsw for aarch64
#   LABSW_TESTING=ON (default) test build: mock adapters, -F/-G, reject_sub
#   LABSW_TESTING=OFF          what goes to the device (tools/pkg.sh)
# Needs XB_SYSROOT (target root, from the device) and XB_STAGE (the stack installed with
# DESTDIR, prefix /opt/labsw). The stack is linked into the sysroot so that pkg-config
# paths (sysroot + /opt/labsw/...) point at it.
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$here/build-aarch64}
: "${XB_SYSROOT:?set XB_SYSROOT}" "${XB_STAGE:?set XB_STAGE}"
mkdir -p "$XB_SYSROOT/opt"
[ -e "$XB_SYSROOT/opt/labsw" ] || ln -s "$XB_STAGE/opt/labsw" "$XB_SYSROOT/opt/labsw"
export PKG_CONFIG=pkgconf PKG_CONFIG_SYSROOT_DIR=$XB_SYSROOT
export PKG_CONFIG_LIBDIR=$XB_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$XB_SYSROOT/usr/share/pkgconfig:$XB_SYSROOT/opt/labsw/lib/pkgconfig
cmake -S "$here" -B "$out" -DCMAKE_TOOLCHAIN_FILE="$here/cmake/aarch64.cmake" -DLABSW_TESTING="${LABSW_TESTING:-ON}" \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/opt/labsw >/dev/null
cmake --build "$out" -j"$(nproc)"
