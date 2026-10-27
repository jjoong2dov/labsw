#!/bin/bash
# lab_down.sh : remove what lab_up.sh made, and only that: br0 with the alias
# "labsw-lab" (or with p1 as a port: an older lab_up.sh) and the namespaces hN holding
# eN; a namespace hN with nothing but lo (its eN went with a deleted pN) holds nothing,
# so it goes either way
# SPDX-License-Identifier: MIT
N=${LAB_PORTS:-4}
ours=0   # asked before the namespaces go: deleting hN deletes pN too
if [ "$(cat /sys/class/net/br0/ifalias 2>/dev/null)" = labsw-lab ] ||
   [ "$(basename "$(readlink /sys/class/net/p1/master 2>/dev/null)")" = br0 ]; then ours=1; fi
for i in $(seq 1 "$N"); do
  ip netns list | awk '{print $1}' | grep -qx "h$i" || continue
  if { [ $ours = 1 ] && ip -n "h$i" link show "e$i" >/dev/null 2>&1; } ||
     [ -z "$(ip -n "h$i" -o link | grep -v ': lo:')" ]; then
    ip netns del "h$i"
  fi
done
if [ $ours = 1 ]; then
  ip link del br0 2>/dev/null
elif ip link show br0 >/dev/null 2>&1; then
  echo "lab_down.sh: br0 is not the lab's; left alone" >&2
fi
exit 0
