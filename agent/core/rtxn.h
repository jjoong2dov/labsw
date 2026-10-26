/* rtxn.h - apply wanted static routes, put the old ones back if anything fails
 * (same contract as txn.h, for routes)
 * SPDX-License-Identifier: MIT */
#ifndef LSW_RTXN_H
#define LSW_RTXN_H

#include "../adapter/route_adapter.h"
#include "route.h"
#include "txn.h"

struct lsw_rtxn {
    struct lsw_route_adapter *a;
    int fail_at;   /* tests: make the n-th route command of an apply fail (0 = off) */
    int cmd_count;
    int restoring;
    int deadline_ms; /* an apply that runs longer stops and is undone (0 = no limit) */
    long long started_ms;
    void (*log)(const char *msg);
};

enum lsw_txn_result lsw_rtxn_apply(struct lsw_rtxn *t, const struct lsw_routes *want, struct lsw_routes *snapshot,
                                   char *err, size_t errlen);
int lsw_rtxn_restore(struct lsw_rtxn *t, const struct lsw_routes *snap, char *err, size_t errlen);

#endif
