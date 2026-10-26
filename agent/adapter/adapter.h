/* adapter.h - how the agent reads and changes the device
 * SPDX-License-Identifier: MIT */
#ifndef LSW_ADAPTER_H
#define LSW_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "../core/state.h"

struct lsw_adapter {
    const char *name;
    /* device preconditions (for the bridge: vlan_filtering is on) */
    int (*check)(struct lsw_adapter *a, char *err, size_t len);
    /* read every port of the bridge and its VLANs */
    int (*read)(struct lsw_adapter *a, struct lsw_state *out, char *err, size_t len);
    /* add a membership or replace its flags (LSW_F_PVID, LSW_F_UNTAGGED) */
    int (*set)(struct lsw_adapter *a, const char *port, int vid, uint8_t flags, char *err, size_t len);
    int (*del)(struct lsw_adapter *a, const char *port, int vid, char *err, size_t len);
    void (*destroy)(struct lsw_adapter *a);
    void *priv;
};

/* Linux bridge through `bridge` and `ip` (iproute2) */
struct lsw_adapter *lsw_bridge_adapter(const char *bridge);

#ifdef LSW_TESTING
/* in-memory device kept in a JSON file, every command appended to trace (may be NULL) */
struct lsw_adapter *lsw_mock_adapter(const char *state_file, const char *trace_file);
#endif

#endif
