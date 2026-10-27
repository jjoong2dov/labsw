/* routes.c - /labsw-route:routes <-> FRR static routes
 * Same events and rules as the switch part in main.c.
 * SPDX-License-Identifier: MIT */
#include "routes.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libyang/libyang.h>
#include <sysrepo/error_format.h>

#include "core/rtxn.h"

#define MAX_PENDING 8
#define PENDING_MAX_AGE 60

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
        time_t since;
        struct lsw_routes *snap; /* before the change */
        struct lsw_routes *want; /* what it applied */
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

int routes_init(struct lsw_route_adapter *a, int fail_at, int deadline_ms, void (*log)(const char *msg))
{
    r.txn.a = a;
    r.txn.deadline_ms = deadline_ms;
    r.txn.fail_at = fail_at;
    r.txn.log = log;
    r.log = log;
    for (int i = 0; i < MAX_PENDING; i++)
        if (!(r.pending[i].snap = malloc(sizeof(struct lsw_routes))) ||
            !(r.pending[i].want = malloc(sizeof(struct lsw_routes))))
            return -1;
    return 0;
}

void routes_cleanup(void)
{
    if (r.txn.a)
        r.txn.a->destroy(r.txn.a);
    for (int i = 0; i < MAX_PENDING; i++) {
        free(r.pending[i].snap);
        free(r.pending[i].want);
    }
}

/* /labsw-route:routes of the session's view. libyang already stores a prefix
 * with host bits in canonical form (198.51.100.77/24 -> 198.51.100.0/24); the
 * check here only guards against a value that bypassed that */
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

static int pending_find(uint32_t op_id)
{
    for (int i = 0; i < MAX_PENDING; i++)
        if (r.pending[i].used && r.pending[i].op_id == op_id)
            return i;
    return -1;
}

static int pending_new(uint32_t op_id)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!r.pending[i].used) {
            r.pending[i].used = 1;
            r.pending[i].op_id = op_id;
            r.pending[i].since = time(NULL);
            return i;
        }
    }
    return -1;
}

/* see pending_for_abort() in main.c (sysrepo 5.1.0 self-generated ABORT) */
static int pending_for_abort(uint32_t op_id)
{
    struct lsw_routes *now;
    char err[256], why[160];
    int i = pending_find(op_id), only = -1, n = 0;

    if (i >= 0)
        return i;
    for (int k = 0; k < MAX_PENDING; k++)
        if (r.pending[k].used && n++ == 0)
            only = k;
    if (n != 1 || !(now = malloc(sizeof(*now))))
        return -1;
    if (r.txn.a->read(r.txn.a, now, err, sizeof(err)) || lsw_routes_diff(now, r.pending[only].want, why, sizeof(why))) {
        logf_("routes op %u: ABORT names no open change, and FRR is not what op %u applied", op_id,
              r.pending[only].op_id);
        only = -1;
    } else {
        logf_("routes op %u: ABORT names no open change; the only open one is op %u and FRR has what it "
              "applied, restoring that", op_id, r.pending[only].op_id);
    }
    free(now);
    return only;
}

int routes_on_change(sr_session_ctx_t *sess, sr_event_t event, uint32_t op_id)
{
    struct lsw_routes *want = NULL, *snap = NULL;
    char err[1024] = "";
    int rc = SR_ERR_OK, slot;

    if (event == SR_EV_DONE || event == SR_EV_ABORT) {
        if ((slot = event == SR_EV_ABORT ? pending_for_abort(op_id) : pending_find(op_id)) < 0) {
            if (event == SR_EV_ABORT) {
                logf_("routes op %u: ABORT for a change we do not know; reconcile decides", op_id);
                set_health(1, "an ABORT could not be matched to a change");
            }
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

    slot = -1;
    want = malloc(sizeof(*want));
    snap = malloc(sizeof(*snap));
    if (!want || !snap) {
        rc = SR_ERR_NO_MEMORY;
        goto end;
    }
    logf_("routes op %u: %s", op_id, event == SR_EV_ENABLED ? "start, applying running" : "change");
    if ((rc = read_wanted(sess, want, err, sizeof(err))) != SR_ERR_OK)
        goto end;
    if (event == SR_EV_CHANGE && (slot = pending_new(op_id)) < 0) {
        snprintf(err, sizeof(err), "too many open changes (%d)", MAX_PENDING);
        sr_session_set_netconf_error(sess, "application", "resource-denied", NULL, NULL, err, 0);
        rc = SR_ERR_OPERATION_FAILED;
        goto end;
    }
    switch (lsw_rtxn_apply(&r.txn, want, snap, err, sizeof(err))) {
    case LSW_TXN_OK:
        set_health(0, NULL);
        if (slot >= 0) {
            *r.pending[slot].snap = *snap;
            *r.pending[slot].want = *want;
            slot = -1;
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
    if (slot >= 0)
        r.pending[slot].used = 0;
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

int routes_read_running(sr_session_ctx_t *sess, struct lsw_routes *want, char *err, size_t len)
{
    return read_wanted(sess, want, err, len) == SR_ERR_OK ? 0 : -1;
}

void routes_reconcile_failed(const char *err)
{
    set_health(1, err);
    logf_("routes reconcile: %s", err);
}

int routes_pending(void)
{
    time_t now = time(NULL);
    int any = 0;

    for (int i = 0; i < MAX_PENDING; i++) {
        if (!r.pending[i].used)
            continue;
        if (now - r.pending[i].since > PENDING_MAX_AGE) {
            logf_("routes op %u: no DONE or ABORT after %d s, forgetting it", r.pending[i].op_id, PENDING_MAX_AGE);
            r.pending[i].used = 0;
        } else {
            any = 1;
        }
    }
    return any;
}

void routes_reconcile_locked(const struct lsw_routes *want)
{
    struct lsw_routes *now = malloc(sizeof(*now)), *snap = malloc(sizeof(*snap));
    char err[1024], why[160];

    if (!now || !snap)
        goto end;
    if (r.txn.a->read(r.txn.a, now, err, sizeof(err))) {
        routes_reconcile_failed(err);
        goto end;
    }
    if (!lsw_routes_diff(now, want, why, sizeof(why))) {
        pthread_mutex_lock(&r.lock);
        int was = r.degraded;
        pthread_mutex_unlock(&r.lock);
        if (was) /* e.g. someone removed an ECMP route by hand: FRR is right again */
            set_health(0, "FRR equals running again");
        goto end;
    }
    logf_("routes reconcile: FRR differs from running (%s), applying running", why);
    if (lsw_rtxn_apply(&r.txn, want, snap, err, sizeof(err)) == LSW_TXN_OK) {
        pthread_mutex_lock(&r.lock);
        r.repairs++;
        pthread_mutex_unlock(&r.lock);
        set_health(0, NULL);
        logf_("routes reconcile: ok");
    } else {
        routes_reconcile_failed(err);
    }
end:
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
