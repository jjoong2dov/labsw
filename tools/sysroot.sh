#!/bin/bash
# sysroot.sh SSH-DEST DIR : copy what the cross build needs from the device (Debian 13 / Pi OS
# arm64) into DIR, for tools/build_stack.sh aarch64 and tools/xbuild.sh (XB_SYSROOT=DIR).
# On the device first: sudo apt-get install rsync libpcre2-dev libssh-dev libssl-dev
#   libcurl4-openssl-dev libjson-c-dev libsystemd-dev
# Then removes what Debian's own libyang/libnetconf2/sysrepo packages left there: the cross
# build must find the versions of deps.lock (in the stage), never those.
# SSH options go in RSYNC_RSH, e.g. RSYNC_RSH="ssh -p 2223 -i key".
# SPDX-License-Identifier: MIT
set -euo pipefail
dest=${1:?usage: sysroot.sh user@device DIR}; dir=$(realpath -m "${2:?usage: sysroot.sh user@device DIR}")
mkdir -p "$dir/usr/lib" "$dir/usr/share"
rsync -a --delete "$dest:/usr/include/" "$dir/usr/include/"
rsync -a --delete "$dest:/usr/lib/aarch64-linux-gnu/" "$dir/usr/lib/aarch64-linux-gnu/"
rsync -a "$dest:/usr/lib/ld-linux-aarch64.so.1" "$dir/usr/lib/"
rsync -a "$dest:/usr/lib/linux" "$dir/usr/lib/" 2>/dev/null || true   # asm headers link here on Debian
rsync -a --delete "$dest:/usr/share/pkgconfig/" "$dir/usr/share/pkgconfig/"
# /lib -> usr/lib on merged-/usr systems
[ -e "$dir/lib" ] || ln -s usr/lib "$dir/lib"
removed=0
for p in "$dir"/usr/include/{libyang,libnetconf2,sysrepo} "$dir"/usr/include/{nc_client,nc_server,nc_version,sysrepo,sysrepo_types}.h \
         "$dir"/usr/lib/aarch64-linux-gnu/{libyang,libnetconf2,libsysrepo}.so* \
         "$dir"/usr/lib/aarch64-linux-gnu/pkgconfig/{libyang,libnetconf2,sysrepo}.pc; do
  [ -e "$p" ] || [ -L "$p" ] || continue
  rm -rf "$p"; echo "removed ${p#$dir/}"; removed=$((removed + 1))
done
for pc in json-c libpcre2-8 libssh libcurl libssl; do
  ls "$dir"/usr/lib/aarch64-linux-gnu/pkgconfig/$pc.pc >/dev/null 2>&1 || echo "warning: no $pc.pc (install its -dev package on the device)"
done
echo "sysroot $dir: $(du -sh "$dir" | cut -f1), $removed stale stack files removed"
