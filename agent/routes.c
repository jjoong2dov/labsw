/* routes.c - /labsw-route:routes <-> FRR static routes
 * Same events and rules as the switch part in main.c.
 * SPDX-License-Identifier: MIT */
#include "routes.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libyang/libyang.h>
#include <sysrepo/error_format.h>

#include "core/rtxn.h"

#define MAX_PENDING 8

static struct {
    struct lsw_rtxn txn;
    void (*log)(const char *msg);
    pthread_mutex_t lock; /* health, last_error, repairs */
    int degraded;
    char last_error[1024];
    uint64_t repairs;
    struct {
        int used;
        uint32_t op_id;
        struct lsw_routes *snap;
    } pending[MAX_PENDING];
} r = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    r.log(buf);
}

static void set_health(int degraded, const char *err)
{
    pthread_mutex_lock(&r.lock);
    r.degraded = degraded;
    snprintf(r.last_error, sizeof(r.last_error), "%s", err ? err : "");
    pthread_mutex_unlock(&r.lock);
}

int routes_init(struct lsw_route_adapter *a, int fail_at, void (*log)(const char *msg))
{
    r.txn.a = a;
    r.txn.fail_at = fail_at;
    r.txn.log = log;
    r.log = log;
    for (int i = 0; i < MAX_PENDING; i++)
        if (!(r.pending[i].snap = malloc(sizeof(struct lsw_routes))))
            return -1;
    return 0;
}

void routes_cleanup(void)
{
    if (r.txn.a)
        r.txn.a->destroy(r.txn.a);
    for (int i = 0; i < MAX_PENDING; i++)
        free(r.pending[i].snap);
}

/* /labsw-route:routes of the session's view; a prefix with host bits is
 * rejected with its path (inet:ipv4-prefix allows them) */
static int read_wanted(sr_session_ctx_t *sess, struct lsw_routes *want, char *err, size_t len)
{
    sr_data_t *data = NULL;
    struct lyd_node *rt, *c;
    char path[96];
    int rc = SR_ERR_OK;

    lsw_routes_clear(want);
    if (sr_get_data(sess, "/" ROUTE_MODULE ":routes", 0, 0, 0, &data) != SR_ERR_OK) {
        snprintf(err, len, "cannot read /%s:routes", ROUTE_MODULE);
        sr_session_set_netconf_error(sess, "application", "operation-failed", NULL, NULL, err, 0);
        return SR_ERR_OPERATION_FAILED;
    }
    if (!data || !data->tree)
        goto end;
    LY_LIST_FOR(lyd_child(data->tree), rt) {
        const char *prefix = NULL, *gw = "";
        if (strcmp(LYD_NAME(rt), "route"))
            continue;
        LY_LIST_FOR(lyd_child(rt), c) {
            if (!strcmp(LYD_NAME(c), "prefix"))
                prefix = lyd_get_value(c);
            else if (!strcmp(LYD_NAME(c), "gateway"))
                gw = lyd_get_value(c);
        }
        if (!prefix || lsw_routes_set(want, prefix, gw, err, len)) {
            snprintf(path, sizeof(path), "/%s:routes/route[prefix='%s']/prefix", ROUTE_MODULE, prefix ? prefix : "");
            sr_session_set_netconf_error(sess, "application", "invalid-value", NULL, path, err, 0);
            rc = SR_ERR_VALIDATION_FAILED;
            break;
        }
    }
end:
    sr_release_data(data);
    return rc;
}

static int pending_slot(uint32_t op_id, int create)
{
    int free_slot = -1, used = 0, last = -1;

    for (int i = 0; i < MAX_PENDING; i++) {
        if (r.pending[i].used && r.pending[i].op_id == op_id)
            return i;
        if (r.pending[i].used) {
            used++;
            last = i;
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (!create) {
        /* see pending_slot() in main.c: sysrepo 5.1.0 self-generated ABORT */
        if (used == 1) {
            logf_("routes op %u: no such change, taking the only open one (op %u)", op_id, r.pending[last].op_id);
            return last;
        }
        return -1;
    }
    if (free_slot < 0)
        return -1;
    r.pending[free_slot].used = 1;
    r.pending[free_slot].op_id = op_id;
    return free_slot;
}

int routes_on_change(sr_session_ctx_t *sess, sr_event_t event, uint32_t op_id)
{
    struct lsw_routes *want = NULL, *snap = NULL;
    char err[1024] = "";
    int rc = SR_ERR_OK, slot;

    if (event == SR_EV_DONE || event == SR_EV_ABORT) {
        if ((slot = pending_slot(op_id, 0)) < 0) {
            if (event == SR_EV_ABORT)
                logf_("routes op %u: ABORT for a change we do not know", op_id);
            return SR_ERR_OK;
        }
        if (event == SR_EV_ABORT) {
            logf_("routes op %u: aborted by another subscriber, restoring", op_id);
            if (lsw_rtxn_restore(&r.txn, r.pending[slot].snap, err, sizeof(err))) {
                logf_("routes op %u: restore failed: %s", op_id, err);
                set_health(1, err);
            } else {
                set_health(0, "change aborted by another subscriber; previous routes restored");
            }
        }
        r.pending[slot].used = 0;
        return SR_ERR_OK;
    }
    if (event != SR_EV_CHANGE && event != SR_EV_ENABLED)
        return SR_ERR_OK;

    want = malloc(sizeof(*want));
    snap = malloc(sizeof(*snap));
    if (!want || !snap) {
        rc = SR_ERR_NO_MEMORY;
        goto end;
    }
    logf_("routes op %u: %s", op_id, event == SR_EV_ENABLED ? "start, applying running" : "change");
    if ((rc = read_wanted(sess, want, err, sizeof(err))) != SR_ERR_OK)
        goto end;
    switch (lsw_rtxn_apply(&r.txn, want, snap, err, sizeof(err))) {
    case LSW_TXN_OK:
        set_health(0, NULL);
        if (event == SR_EV_CHANGE) {
            if ((slot = pending_slot(op_id, 1)) >= 0)
                *r.pending[slot].snap = *snap;
            else
                logf_("routes op %u: too many open changes, an ABORT could not be undone", op_id);
        }
        break;
    case LSW_TXN_FAILED:
        set_health(0, err);
        sr_session_set_netconf_error(sess, "application", "operation-failed", NULL, NULL, err, 0);
        rc = SR_ERR_OPERATION_FAILED;
        break;
    case LSW_TXN_FAILED_DEGRADED:
        set_health(1, err);
        sr_session_set_netconf_error(sess, "application", "operation-failed", NULL, NULL, err, 0);
        rc = SR_ERR_OPERATION_FAILED;
        break;
    }
end:
    logf_("routes op %u: %s", op_id, rc == SR_ERR_OK ? "ok" : err);
    if (event == SR_EV_ENABLED && rc != SR_ERR_OK) {
        set_health(1, err);
        logf_("routes op %u: running is NOT in FRR; health degraded until a change succeeds", op_id);
        rc = SR_ERR_OK;
    }
    free(want);
    free(snap);
    return rc;
}

void routes_reconcile(sr_session_ctx_t *sess)
{
    struct lsw_routes *want = malloc(sizeof(*want)), *now = malloc(sizeof(*now)), *snap = malloc(sizeof(*snap));
    char err[1024], why[160];

    if (!want || !now || !snap)
        goto end;
    for (int i = 0; i < MAX_PENDING; i++)
        if (r.pending[i].used)
            goto end;
    if (read_wanted(sess, want, err, sizeof(err)) != SR_ERR_OK || r.txn.a->read(r.txn.a, now, err, sizeof(err))) {
        set_health(1, err);
        logf_("routes reconcile: %s", err);
        goto end;
    }
    if (!lsw_routes_diff(now, want, why, sizeof(why)))
        goto end;
    logf_("routes reconcile: FRR differs from running (%s), applying running", why);
    if (lsw_rtxn_apply(&r.txn, want, snap, err, sizeof(err)) == LSW_TXN_OK) {
        pthread_mutex_lock(&r.lock);
        r.repairs++;
        pthread_mutex_unlock(&r.lock);
        set_health(0, NULL);
        logf_("routes reconcile: ok");
    } else {
        set_health(1, err);
        logf_("routes reconcile: %s", err);
    }
end:
    free(want);
    free(now);
    free(snap);
}

int routes_on_oper(sr_session_ctx_t *sess, struct lyd_node **parent)
{
    const struct ly_ctx *ctx = sr_session_acquire_context(sess);
    const struct lys_module *mod = ly_ctx_get_module_implemented(ctx, ROUTE_MODULE);
    char err[1024], num[24];
    int degraded, rc = SR_ERR_OK;
    uint64_t repairs;

    pthread_mutex_lock(&r.lock);
    degraded = r.degraded;
    repairs = r.repairs;
    snprintf(err, sizeof(err), "%s", r.last_error);
    pthread_mutex_unlock(&r.lock);
    snprintf(num, sizeof(num), "%llu", (unsigned long long)repairs);
    if ((!*parent && lyd_new_inner(NULL, mod, "routes-state", 0, parent)) ||
        lyd_new_term(*parent, mod, "health", degraded ? "degraded" : "ok", 0, NULL) ||
        lyd_new_term(*parent, mod, "repairs", num, 0, NULL) ||
        (err[0] && lyd_new_term(*parent, mod, "last-error", err, 0, NULL)))
        rc = SR_ERR_LY;
    sr_session_release_context(sess);
    return rc;
}
