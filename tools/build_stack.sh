#!/bin/bash
# build_stack.sh host [PREFIX]   : build the stack of deps.lock for this PC into PREFIX
#                                  (default ~/opt/labsw-host), no root needed; for development
#                                  and the tests. sysrepo keeps its repository in PREFIX/var/sysrepo.
# build_stack.sh aarch64 STAGE   : cross-build for the device (Pi OS / Debian 13 arm64) with
#                                  prefix /opt/labsw, installed into STAGE/opt/labsw (DESTDIR).
#                                  Needs XB_SYSROOT (a copy of the device's /usr), the
#                                  aarch64-linux-gnu-gcc toolchain and qemu-aarch64 (sysrepo's
#                                  try_run); see cmake/aarch64.cmake.
# Sources are cloned into ${LABSW_SRC:-.deps/src} at the tag of deps.lock and checked against
# its commit. Build packages (Debian/Ubuntu): build-essential cmake pkg-config git
#   libpcre2-dev libssh-dev libssl-dev libcurl4-openssl-dev libjson-c-dev libcmocka-dev
# SPDX-License-Identifier: MIT
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
mode=${1:?usage: build_stack.sh host [PREFIX] | aarch64 STAGE}
src=$(realpath -m "${LABSW_SRC:-$here/.deps/src}")
obj=$(realpath -m "${LABSW_OBJ:-$here/.deps/obj-$mode}")
jobs=${JOBS:-$(nproc)}

case $mode in
  host)
    prefix=$(realpath -m "${2:-$HOME/opt/labsw-host}")
    # /etc/sysrepo (the default) needs root. The shared memory prefix cannot be set here
    # (sysrepo has it fixed to "sr"; cmake warns that SHM_PREFIX is not used): set
    # SYSREPO_SHM_PREFIX when running, as tools/devlab.py and the tests do, or this repository
    # and a system sysrepo would meet in /dev/shm ("Shared memory repository path mismatch").
    # REPO_SHM_SETUP=OFF for the same reason: it runs sysrepoctl at install, without the prefix
    opts=(-DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" -DREPO_PATH="$prefix/var/sysrepo"
          -DNOTIFD_SETUP=OFF -DSYSREPO_SETUP=OFF -DREPO_SHM_SETUP=OFF)
    export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    destdir=;;
  aarch64)
    stage=$(realpath -m "${2:?usage: build_stack.sh aarch64 STAGE}"); prefix=/opt/labsw
    : "${XB_SYSROOT:?set XB_SYSROOT to a copy of the device root}"
    export XB_SYSROOT=$(realpath "$XB_SYSROOT") XB_STAGE=$stage
    # the sysroot's opt/labsw must be THIS stage: a link left to an older stage (xbuild.sh
    # makes one) lets cmake find the old libraries first
    mkdir -p "$XB_SYSROOT/opt" "$stage$prefix"
    if [ "$(readlink "$XB_SYSROOT/opt/labsw" 2>/dev/null)" != "$stage$prefix" ]; then
      [ -L "$XB_SYSROOT/opt/labsw" ] && rm "$XB_SYSROOT/opt/labsw"
      [ -e "$XB_SYSROOT/opt/labsw" ] && { echo "build_stack.sh: $XB_SYSROOT/opt/labsw is not a link, move it away" >&2; exit 1; }
      ln -s "$stage$prefix" "$XB_SYSROOT/opt/labsw"
    fi
    export PKG_CONFIG=pkgconf PKG_CONFIG_SYSROOT_DIR=$XB_SYSROOT
    export PKG_CONFIG_LIBDIR=$XB_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$XB_SYSROOT/usr/share/pkgconfig
    export PKG_CONFIG_PATH=$stage$prefix/lib/pkgconfig
    # the module and repository setup runs sysrepoctl, an aarch64 program: do it on the device
    # (deploy/install.sh). REPO_SHM_SETUP is new in sysrepo 5.2 and on by default
    opts=(-DCMAKE_TOOLCHAIN_FILE="$here/cmake/aarch64.cmake" -DCMAKE_INSTALL_PREFIX=$prefix -DCMAKE_PREFIX_PATH=$prefix
          -DNOTIFD_SETUP=OFF -DSYSREPO_SETUP=OFF -DREPO_SHM_SETUP=OFF)
    destdir=$stage;;
  *) echo "build_stack.sh: mode is host or aarch64" >&2; exit 2;;
esac

mkdir -p "$src" "$obj"
grep -v '^#' "$here/deps.lock" | while read -r name url tag commit _; do
  [ -n "$name" ] || continue
  echo "=== $name $tag $(date +%T)"
  [ -d "$src/$name/.git" ] || git clone -q "$url" "$src/$name"
  git -C "$src/$name" fetch -q --tags origin
  git -C "$src/$name" checkout -q "$tag"
  have=$(git -C "$src/$name" rev-parse HEAD)
  [ "$have" = "$commit" ] || { echo "$name: $tag is $have, deps.lock says $commit" >&2; exit 1; }
  rm -rf "$obj/$name"; mkdir -p "$obj/$name"
  ( cd "$obj/$name"
    cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF "${opts[@]}" "$src/$name" \
      > cmake.log 2>&1 || { tail -25 cmake.log; exit 1; }
    make -j"$jobs" > make.log 2>&1 || { grep -m5 -B2 -A5 error make.log; exit 1; }
    make install ${destdir:+DESTDIR=$destdir} > install.log 2>&1 || { tail -15 install.log; exit 1; } )
done
echo "=== done $(date +%T) -> ${destdir:+$destdir}$prefix"
