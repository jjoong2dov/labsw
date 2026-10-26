#!/bin/bash
# lab_down.sh : remove what lab_up.sh made
# SPDX-License-Identifier: MIT
N=${LAB_PORTS:-4}
for i in $(seq 1 "$N"); do ip netns del h$i 2>/dev/null; done
ip link del br0 2>/dev/null
exit 0
