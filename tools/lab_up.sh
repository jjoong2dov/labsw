#!/bin/bash
# lab_up.sh : the lab "switch": br0 (VLAN filtering) with ports p1..p4. The other
# end of port pN is eN in network namespace hN, a host for traffic tests.
# vlan_default_pvid 0: a new port has no VLAN until the agent gives it one.
# SPDX-License-Identifier: MIT
set -euo pipefail
N=${LAB_PORTS:-4}
ip link show br0 >/dev/null 2>&1 && exit 0
ip link add br0 type bridge vlan_filtering 1 vlan_default_pvid 0
ip link set br0 up
for i in $(seq 1 "$N"); do
  ip netns add h$i
  ip link add p$i type veth peer name e$i
  ip link set e$i netns h$i
  ip link set p$i master br0 up
  ip -n h$i link set e$i up
  ip -n h$i link set lo up
done
