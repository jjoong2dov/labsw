#!/bin/bash
# lab_down.sh : remove what lab_up.sh made, and only that: br0 with the alias
# "labsw-lab" (or with p1 as a port: an older lab_up.sh), namespaces hN holding eN
# SPDX-License-Identifier: MIT
N=${LAB_PORTS:-4}
ours=0   # asked before the namespaces go: deleting hN deletes pN too
if [ "$(cat /sys/class/net/br0/ifalias 2>/dev/null)" = labsw-lab ] ||
   [ "$(basename "$(readlink /sys/class/net/p1/master 2>/dev/null)")" = br0 ]; then ours=1; fi
if [ $ours = 0 ]; then
  ip link show br0 >/dev/null 2>&1 && echo "lab_down.sh: br0 is not the lab's; left alone" >&2
  exit 0
fi
for i in $(seq 1 "$N"); do
  ip -n "h$i" link show "e$i" >/dev/null 2>&1 && ip netns del "h$i"
done
ip link del br0 2>/dev/null
exit 0
