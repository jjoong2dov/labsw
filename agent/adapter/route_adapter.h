/* route_adapter.h - how the agent reads and changes static routes
 * SPDX-License-Identifier: MIT */
#ifndef LSW_ROUTE_ADAPTER_H
#define LSW_ROUTE_ADAPTER_H

#include <stddef.h>

#include "../core/route.h"

struct lsw_route_adapter {
    const char *name;
    /* every static route the agent owns; -1 if one cannot be expressed (err says which) */
    int (*read)(struct lsw_route_adapter *a, struct lsw_routes *out, char *err, size_t len);
    int (*add)(struct lsw_route_adapter *a, const struct lsw_route *r, char *err, size_t len);
    int (*del)(struct lsw_route_adapter *a, const struct lsw_route *r, char *err, size_t len);
    void (*destroy)(struct lsw_route_adapter *a);
    void *priv;
};

/* FRR through vtysh; path is the vtysh binary, ns the FRR instance (-N) or NULL */
struct lsw_route_adapter *lsw_frr_adapter(const char *vtysh, const char *ns);

/* parse `show mgmt datastore-contents running json` (exported for tests) */
int lsw_frr_parse(const char *json, struct lsw_routes *out, char *err, size_t len);

#ifdef LSW_TESTING
/* routes kept in a JSON file {"routes": {"192.0.2.0/24": "198.51.100.1" | "blackhole"}} */
struct lsw_route_adapter *lsw_mock_route_adapter(const char *state_file, const char *trace_file);
#endif

#endif
