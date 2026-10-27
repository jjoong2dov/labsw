#!/bin/bash
# lab_up.sh : the lab "switch": br0 (VLAN filtering) with ports p1..p4. The other
# end of port pN is eN in network namespace hN, a host for traffic tests.
# vlan_default_pvid 0: a new port has no VLAN until the agent gives it one.
# SPDX-License-Identifier: MIT
set -euo pipefail
N=${LAB_PORTS:-4}
# br0 carries the alias "labsw-lab" so that lab_down.sh removes only what this made
if ip link show br0 >/dev/null 2>&1; then
  if [ "$(cat /sys/class/net/br0/ifalias 2>/dev/null)" = labsw-lab ]; then
    for i in $(seq 1 "$N"); do   # all there, or say so (lab_down.sh, then again)
      [ "$(basename "$(readlink /sys/class/net/p$i/master 2>/dev/null)")" = br0 ] ||
        { echo "lab_up.sh: br0 is the lab's but p$i is missing; run lab_down.sh first" >&2; exit 1; }
    done
    exit 0
  fi
  # made by an older lab_up.sh (no alias yet): p1 is its port
  if [ "$(basename "$(readlink /sys/class/net/p1/master 2>/dev/null)")" = br0 ]; then
    ip link set br0 alias labsw-lab; exit 0
  fi
  echo "lab_up.sh: br0 exists and is not the lab's; not touching it" >&2; exit 1
fi
for i in $(seq 1 "$N"); do
  if ip netns list | awk '{print $1}' | grep -qx "h$i" || ip link show "p$i" >/dev/null 2>&1; then
    echo "lab_up.sh: netns h$i or link p$i exists already; not touching it" >&2; exit 1
  fi
done
ip link add br0 type bridge vlan_filtering 1 vlan_default_pvid 0
ip link set br0 alias labsw-lab
ip link set br0 up
for i in $(seq 1 "$N"); do
  ip netns add h$i
  ip link add p$i type veth peer name e$i
  ip link set e$i netns h$i
  ip link set p$i master br0 up
  ip -n h$i link set e$i up
  ip -n h$i link set lo up
done
