/* bridge_parse.h - turn `ip -j link show master BR` and `bridge -j vlan show` into a state
 * SPDX-License-Identifier: MIT */
#ifndef LSW_BRIDGE_PARSE_H
#define LSW_BRIDGE_PARSE_H

#include <stddef.h>

#include "../core/state.h"

/* ports come from link_json (every port of the bridge, even one without VLANs),
 * VLANs from vlan_json (entries of other devices, and of the bridge itself, are ignored) */
int lsw_bridge_parse(const char *link_json, const char *vlan_json, const char *bridge, struct lsw_state *out,
                     char *err, size_t len);

#endif
