/* routes.h - the labsw-route part of labswd (static routes in FRR)
 * SPDX-License-Identifier: MIT */
#ifndef LSW_ROUTES_H
#define LSW_ROUTES_H

#include <sysrepo.h>

#include "adapter/route_adapter.h"

#define ROUTE_MODULE "labsw-route"

/* takes ownership of a; log prints one line */
int routes_init(struct lsw_route_adapter *a, int fail_at, int deadline_ms, void (*log)(const char *msg));
void routes_cleanup(void);

/* module change callback body; the caller holds the agent's transaction lock */
int routes_on_change(sr_session_ctx_t *sess, sr_event_t event, uint32_t op_id);
/* routes-state */
int routes_on_oper(sr_session_ctx_t *sess, struct lyd_node **parent);
/* reconcile in two steps: read running WITHOUT the agent's lock (sysrepo call),
 * then, with the lock held, compare with FRR and put running back */
int routes_read_running(sr_session_ctx_t *sess, struct lsw_routes *want, char *err, size_t len);
void routes_reconcile_locked(const struct lsw_routes *want);
void routes_reconcile_failed(const char *err);
/* 1 if a route change is between CHANGE and DONE/ABORT (expired ones are dropped); lock held */
int routes_pending(void);

#endif
