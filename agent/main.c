/* main.c - labswd: keeps a Linux bridge equal to /labsw-switch:switch in running
 *
 * sysrepo events (module change subscription with SR_SUBSCR_ENABLED):
 *   ENABLED  agent start: apply what running already has (if that fails, keep
 *            running with health degraded; the next successful change fixes it)
 *   CHANGE   validate, apply to the device, read back; on any failure restore
 *            the device here (a failing subscriber gets no ABORT) and reject
 *   ABORT    another subscriber rejected the change after we applied it: restore
 *   DONE     the change is committed: forget the snapshot
 * If the agent dies between CHANGE and DONE, nothing restores the device; the
 * next start (ENABLED) makes it equal to running again.
 *
 * With -f vtysh the agent also keeps /labsw-route:routes in FRR (routes.c).
 *
 * labsw-ops RPCs backup and restore (ops.c) keep copies of running in files
 * under -B dir (default /var/lib/labsw/backup).
 *
 * With -R n the agent also compares running with the device every n seconds and
 * puts back what someone changed outside NETCONF (policy: NETCONF wins). It
 * skips a round while a change is between CHANGE and DONE/ABORT.
 *
 * Notifications (labsw-switch): health-changed when switch-state/health changes,
 * repaired after a reconcile round put running back. They are sent after the
 * work is done and txn_lock is released; a health that flips and flips back
 * before that is not sent.
 *
 * What this is not: a transaction across the bridge and FRR. A change is applied
 * module by module during CHANGE and undone on failure or ABORT as well as the
 * agent can (best effort); in between, one may have the new state and the other
 * the old one. If the agent dies, the next start or reconcile round fixes it.
 *
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libyang/libyang.h>
#include <sysrepo.h>
#include <sysrepo/error_format.h>

#include "adapter/adapter.h"
#include "core/txn.h"
#include "ops.h"
#include "routes.h"

#define MODULE "labsw-switch"
#define MAX_PENDING 8
#define PENDING_MAX_AGE 60 /* s: a change with no DONE/ABORT after this is dropped */

static struct {
    struct lsw_adapter *a;
    struct lsw_txn txn;
    int verbose;
    pthread_mutex_t lock;     /* health, last_error, repairs (read by on_oper) */
    pthread_mutex_t txn_lock; /* the device, g.txn and pending: callbacks vs. reconcile */
    int degraded;
    char last_error[1024];
    uint64_t repairs;
    struct {
        int used;
        uint32_t op_id;
        time_t since;
        struct lsw_state snap; /* before the change: what ABORT restores */
        struct lsw_state want; /* what the change applied */
    } pending[MAX_PENDING];
    atomic_uint callbacks; /* bumped when a change callback starts; reconcile checks it */
    struct lsw_state seen; /* last device state read while no change ran (g.lock) */
    int seen_valid;
    sr_session_ctx_t *nsess;  /* notifications are sent on this session */
    int sent_degraded;        /* health in the last health-changed sent (g.lock) */
    uint64_t sent_repairs;    /* repairs in the last repaired sent (g.lock) */
    char difference[160];     /* the difference the last repair fixed (g.lock) */
} g = {.lock = PTHREAD_MUTEX_INITIALIZER, .txn_lock = PTHREAD_MUTEX_INITIALIZER};

static void log_line(const char *msg)
{
    printf("labswd: %s\n", msg);
}

static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_line(buf);
}

static void set_health(int degraded, const char *err)
{
    pthread_mutex_lock(&g.lock);
    g.degraded = degraded;
    snprintf(g.last_error, sizeof(g.last_error), "%s", err ? err : "");
    pthread_mutex_unlock(&g.lock);
}

static int send_notif(const char *name, const char *k1, const char *v1, const char *k2, const char *v2)
{
    const struct ly_ctx *ctx = sr_session_acquire_context(g.nsess);
    const struct lys_module *mod = ly_ctx_get_module_implemented(ctx, MODULE);
    struct lyd_node *n = NULL;
    int rc = SR_ERR_LY;

    if (!lyd_new_inner(NULL, mod, name, 0, &n) && !lyd_new_term(n, mod, k1, v1, 0, NULL) &&
        (!v2 || !*v2 || !lyd_new_term(n, mod, k2, v2, 0, NULL)))
        rc = sr_notif_send_tree(g.nsess, n, 0, 0); /* do not wait for the subscribers */
    lyd_free_all(n);
    sr_session_release_context(g.nsess);
    if (rc)
        logf_("notification %s: %s", name, sr_strerror(rc));
    return rc;
}

/* send what changed since the last notifications; call without txn_lock */
static void notify(void)
{
    char reason[1024], diff[160], num[24];
    int degraded, health = 0, repaired = 0;
    uint64_t repairs;

    if (!g.nsess)
        return;
    pthread_mutex_lock(&g.lock);
    degraded = g.degraded;
    if (degraded != g.sent_degraded) {
        health = 1;
        g.sent_degraded = degraded;
        snprintf(reason, sizeof(reason), "%s", g.last_error);
    }
    repairs = g.repairs;
    if (repairs != g.sent_repairs) {
        repaired = 1;
        g.sent_repairs = repairs;
        snprintf(diff, sizeof(diff), "%s", g.difference);
    }
    pthread_mutex_unlock(&g.lock);
    if (health) {
        logf_("notification health-changed: %s", degraded ? "degraded" : "ok");
        send_notif("health-changed", "health", degraded ? "degraded" : "ok", "reason", reason);
    }
    if (repaired) {
        snprintf(num, sizeof(num), "%llu", (unsigned long long)repairs);
        logf_("notification repaired: %s", num);
        send_notif("repaired", "repairs", num, "difference", diff);
    }
}

/* /labsw-switch:switch of the session's view -> wanted device state */
static int read_wanted(sr_session_ctx_t *sess, struct lsw_state *want, char *err, size_t len)
{
    sr_data_t *data = NULL;
    struct lyd_node *port, *n, *v, *c;
    int rc = -1;

    lsw_state_clear(want);
    if (sr_get_data(sess, "/" MODULE ":switch", 0, 0, 0, &data) != SR_ERR_OK) {
        snprintf(err, len, "cannot read /%s:switch", MODULE);
        return -1;
    }
    if (!data || !data->tree) {
        rc = 0; /* empty config: every port without VLANs */
        goto end;
    }
    LY_LIST_FOR(lyd_child(data->tree), port) {
        struct lsw_port *p = NULL;
        if (strcmp(LYD_NAME(port), "port"))
            continue;
        LY_LIST_FOR(lyd_child(port), n) {
            if (!strcmp(LYD_NAME(n), "name") && !(p = lsw_state_port(want, lyd_get_value(n)))) {
                snprintf(err, len, "port %s: too many ports", lyd_get_value(n));
                goto end;
            }
        }
        LY_LIST_FOR(lyd_child(port), v) {
            int vid = 0;
            uint8_t f = LSW_F_MEMBER;
            if (strcmp(LYD_NAME(v), "vlan"))
                continue;
            LY_LIST_FOR(lyd_child(v), c) {
                const char *val = lyd_get_value(c);
                if (!strcmp(LYD_NAME(c), "vid"))
                    vid = atoi(val);
                else if (!strcmp(LYD_NAME(c), "pvid") && !strcmp(val, "true"))
                    f |= LSW_F_PVID;
                else if (!strcmp(LYD_NAME(c), "egress") && !strcmp(val, "untagged"))
                    f |= LSW_F_UNTAGGED;
            }
            if (p && vid >= 1 && vid <= LSW_VID_MAX)
                p->vid[vid] = f;
        }
    }
    rc = 0;
end:
    sr_release_data(data);
    return rc;
}

/* device facts YANG cannot check: every configured port must exist on the bridge */
static int validate(sr_session_ctx_t *sess, const struct lsw_state *want, char *err, size_t len)
{
    struct lsw_state dev;
    char path[128];
    int rc = SR_ERR_OK;

    if (lsw_state_init(&dev))
        return SR_ERR_NO_MEMORY;
    if (g.a->read(g.a, &dev, err, len)) {
        sr_session_set_netconf_error(sess, "application", "operation-failed", NULL, NULL, err, 0);
        rc = SR_ERR_OPERATION_FAILED;
        goto end;
    }
    for (int i = 0; i < want->nports; i++) {
        if (lsw_state_find(&dev, want->port[i].name))
            continue;
        /* the name matched the YANG pattern, so it cannot contain a quote */
        snprintf(path, sizeof(path), "/%s:switch/port[name='%s']", MODULE, want->port[i].name);
        snprintf(err, len, "Port \"%s\" is not a port of the bridge.", want->port[i].name);
        sr_session_set_netconf_error(sess, "application", "invalid-value", NULL, path, err, 0);
        rc = SR_ERR_VALIDATION_FAILED;
        break;
    }
end:
    lsw_state_free(&dev);
    return rc;
}

/* open change by operation id, or -1 */
static int pending_find(uint32_t op_id)
{
    for (int i = 0; i < MAX_PENDING; i++)
        if (g.pending[i].used && g.pending[i].op_id == op_id)
            return i;
    return -1;
}

/* the only open change, or -1 if there are none or several */
static int pending_only(void)
{
    int found = -1;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!g.pending[i].used)
            continue;
        if (found >= 0)
            return -1;
        found = i;
    }
    return found;
}

/* reserve a slot before touching the device; -1 if all are taken */
static int pending_new(uint32_t op_id)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!g.pending[i].used) {
            g.pending[i].used = 1;
            g.pending[i].op_id = op_id;
            g.pending[i].since = time(NULL);
            return i;
        }
    }
    return -1;
}

/*
 * sysrepo 5.1.0 passes the request id, not the operation id, to an ABORT it
 * makes itself (a subscriber of the same priority failed; shm_sub.c). Then the
 * ABORT names no open change. Take the only open change, but only if the device
 * still has exactly what that change applied; otherwise say we do not know.
 */
static int pending_for_abort(uint32_t op_id)
{
    struct lsw_state dev;
    char err[256], why[160];
    int i = pending_find(op_id);

    if (i >= 0 || (i = pending_only()) < 0)
        return i;
    if (lsw_state_init(&dev))
        return -1;
    if (g.a->read(g.a, &dev, err, sizeof(err)) || lsw_state_diff(&dev, &g.pending[i].want, why, sizeof(why))) {
        logf_("op %u: ABORT names no open change, and the device is not what op %u applied", op_id,
              g.pending[i].op_id);
        i = -1;
    } else {
        logf_("op %u: ABORT names no open change; the only open one is op %u and the device has what it "
              "applied, restoring that", op_id, g.pending[i].op_id);
    }
    lsw_state_free(&dev);
    return i;
}

static int on_change_locked(sr_session_ctx_t *sess, sr_event_t event, uint32_t op_id)
{
    struct lsw_state want, snap;
    char err[1024] = "";
    int rc = SR_ERR_OK, slot;

    if (event == SR_EV_DONE || event == SR_EV_ABORT) {
        /* a DONE that names no open change (e.g. the one after ENABLED) is ignored:
         * forgetting the wrong snapshot would be worse than keeping it until it expires */
        slot = event == SR_EV_ABORT ? pending_for_abort(op_id) : pending_find(op_id);
        if (slot < 0) {
            if (event == SR_EV_ABORT) {
                logf_("op %u: ABORT for a change we do not know; reconcile decides", op_id);
                set_health(1, "an ABORT could not be matched to a change");
            }
            return SR_ERR_OK;
        }
        if (event == SR_EV_ABORT) {
            logf_("op %u: aborted by another subscriber, restoring", op_id);
            if (lsw_txn_restore(&g.txn, &g.pending[slot].snap, err, sizeof(err))) {
                logf_("op %u: restore failed: %s", op_id, err);
                set_health(1, err);
            } else {
                set_health(0, "change aborted by another subscriber; previous state restored");
            }
        }
        g.pending[slot].used = 0;
        return SR_ERR_OK;
    }
    if (event != SR_EV_CHANGE && event != SR_EV_ENABLED)
        return SR_ERR_OK;

    if (lsw_state_init(&want) || lsw_state_init(&snap))
        return SR_ERR_NO_MEMORY;
    slot = -1;
    logf_("op %u: %s", op_id, event == SR_EV_ENABLED ? "start, applying running" : "change");
    if (read_wanted(sess, &want, err, sizeof(err))) {
        sr_session_set_netconf_error(sess, "application", "operation-failed", NULL, NULL, err, 0);
        rc = SR_ERR_OPERATION_FAILED;
        goto end;
    }
    if ((rc = validate(sess, &want, err, sizeof(err))) != SR_ERR_OK)
        goto end;
    /* the slot for a later ABORT is taken before the device is touched */
    if (event == SR_EV_CHANGE && (slot = pending_new(op_id)) < 0) {
        snprintf(err, sizeof(err), "too many open changes (%d)", MAX_PENDING);
        sr_session_set_netconf_error(sess, "application", "resource-denied", NULL, NULL, err, 0);
        rc = SR_ERR_OPERATION_FAILED;
        goto end;
    }
    switch (lsw_txn_apply(&g.txn, &want, &snap, err, sizeof(err))) {
    case LSW_TXN_OK:
        set_health(0, NULL);
        if (slot >= 0) {
            lsw_state_copy(&g.pending[slot].snap, &snap);
            lsw_state_copy(&g.pending[slot].want, &want);
            slot = -1; /* kept until DONE or ABORT */
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
        g.pending[slot].used = 0; /* failed: nothing to keep */
    logf_("op %u: %s", op_id, rc == SR_ERR_OK ? "ok" : err);
    if (event == SR_EV_ENABLED && rc != SR_ERR_OK) {
        /* failing here would stop the agent, and with no subscriber sysrepo takes every
         * edit without touching the device. Keep running, say so, reject changes. */
        set_health(1, err);
        logf_("op %u: running is NOT on the device; health degraded until a change succeeds", op_id);
        rc = SR_ERR_OK;
    }
    lsw_state_free(&want);
    lsw_state_free(&snap);
    return rc;
}

static int on_change(sr_session_ctx_t *sess, uint32_t sub_id, const char *module, const char *xpath,
                     sr_event_t event, uint32_t op_id, void *priv)
{
    int rc;
    (void)sub_id; (void)module; (void)xpath; (void)priv;

    atomic_fetch_add(&g.callbacks, 1);
    pthread_mutex_lock(&g.txn_lock);
    rc = on_change_locked(sess, event, op_id);
    pthread_mutex_unlock(&g.txn_lock);
    notify();
    return rc;
}

/* drop open changes that never got DONE/ABORT; the caller holds txn_lock */
static void pending_expire(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g.pending[i].used && now - g.pending[i].since > PENDING_MAX_AGE) {
            logf_("op %u: no DONE or ABORT after %d s, forgetting it", g.pending[i].op_id, PENDING_MAX_AGE);
            g.pending[i].used = 0;
        }
    }
}

static int pending_any(void)
{
    for (int i = 0; i < MAX_PENDING; i++)
        if (g.pending[i].used)
            return 1;
    return 0;
}

/*
 * Make the device equal to running again if something else changed it.
 * Running is read before txn_lock is taken: a change callback runs while sysrepo
 * holds its own locks and then waits for txn_lock, so calling sysrepo with
 * txn_lock held could deadlock. If a callback started meanwhile, skip the round.
 */
static void reconcile(sr_session_ctx_t *sess, struct lsw_routes *rwant)
{
    struct lsw_state want, dev, snap;
    char err[1024], rerr[1024], why[160];
    unsigned start = atomic_load(&g.callbacks);
    int sw_ok, rt_ok = 0;

    if (lsw_state_init(&want) || lsw_state_init(&dev) || lsw_state_init(&snap))
        goto end;
    sw_ok = read_wanted(sess, &want, err, sizeof(err)) == 0;
    if (rwant)
        rt_ok = routes_read_running(sess, rwant, rerr, sizeof(rerr)) == 0;
    pthread_mutex_lock(&g.txn_lock);
    pending_expire();
    if (atomic_load(&g.callbacks) != start || pending_any() || (rwant && routes_pending()))
        goto unlock; /* a change is running or open; its DONE or ABORT decides */
    if (rwant) {
        if (rt_ok)
            routes_reconcile_locked(rwant);
        else
            routes_reconcile_failed(rerr);
    }
    /* vlan_filtering off makes the VLAN table meaningless even if it equals running */
    if (!sw_ok || g.a->check(g.a, err, sizeof(err)) || g.a->read(g.a, &dev, err, sizeof(err))) {
        pthread_mutex_lock(&g.lock);
        int same = g.degraded && !strcmp(g.last_error, err);
        pthread_mutex_unlock(&g.lock);
        if (!same) /* once, not every round */
            logf_("reconcile: %s", err);
        set_health(1, err);
        goto unlock;
    }
    if (!lsw_state_diff(&dev, &want, why, sizeof(why))) {
        pthread_mutex_lock(&g.lock);
        int was = g.degraded;
        pthread_mutex_unlock(&g.lock);
        if (was) /* e.g. a read failed before, or a restore; now the device is right */
            set_health(0, "the bridge equals running again");
        goto unlock;
    }
    logf_("reconcile: device differs from running (%s), applying running", why);
    if (lsw_txn_apply(&g.txn, &want, &snap, err, sizeof(err)) == LSW_TXN_OK) {
        pthread_mutex_lock(&g.lock);
        g.repairs++;
        snprintf(g.difference, sizeof(g.difference), "%s", why);
        pthread_mutex_unlock(&g.lock);
        set_health(0, NULL);
        logf_("reconcile: ok");
    } else {
        set_health(1, err); /* the device still differs from running */
        logf_("reconcile: %s", err);
    }
unlock:
    pthread_mutex_unlock(&g.txn_lock);
    notify();
end:
    lsw_state_free(&want);
    lsw_state_free(&dev);
    lsw_state_free(&snap);
}

static int on_route_change(sr_session_ctx_t *sess, uint32_t sub_id, const char *module, const char *xpath,
                           sr_event_t event, uint32_t op_id, void *priv)
{
    int rc;
    (void)sub_id; (void)module; (void)xpath; (void)priv;

    atomic_fetch_add(&g.callbacks, 1);
    pthread_mutex_lock(&g.txn_lock);
    rc = routes_on_change(sess, event, op_id);
    pthread_mutex_unlock(&g.txn_lock);
    return rc;
}

static int on_route_oper(sr_session_ctx_t *sess, uint32_t sub_id, const char *module, const char *path,
                         const char *request_xpath, uint32_t op_id, struct lyd_node **parent, void *priv)
{
    (void)sub_id; (void)module; (void)path; (void)request_xpath; (void)op_id; (void)priv;
    return routes_on_oper(sess, parent);
}

static int on_oper(sr_session_ctx_t *sess, uint32_t sub_id, const char *module, const char *path,
                   const char *request_xpath, uint32_t op_id, struct lyd_node **parent, void *priv)
{
    const struct ly_ctx *ctx;
    const struct lys_module *mod;
    struct lsw_state dev;
    struct lyd_node *pn, *vn;
    char err[1024], vid[8], num[24];
    int rc = SR_ERR_OK, degraded;
    uint64_t repairs;
    (void)sub_id; (void)module; (void)path; (void)request_xpath; (void)op_id; (void)priv;

    if (lsw_state_init(&dev))
        return SR_ERR_NO_MEMORY;
    ctx = sr_session_acquire_context(sess);
    mod = ly_ctx_get_module_implemented(ctx, MODULE);
    if (!*parent && lyd_new_inner(NULL, mod, "switch-state", 0, parent)) {
        rc = SR_ERR_LY;
        goto end;
    }
    pthread_mutex_lock(&g.lock);
    degraded = g.degraded;
    repairs = g.repairs;
    snprintf(err, sizeof(err), "%s", g.last_error);
    pthread_mutex_unlock(&g.lock);
    snprintf(num, sizeof(num), "%llu", (unsigned long long)repairs);
    if (lyd_new_term(*parent, mod, "health", degraded ? "degraded" : "ok", 0, NULL) ||
        lyd_new_term(*parent, mod, "repairs", num, 0, NULL) ||
        (err[0] && lyd_new_term(*parent, mod, "last-error", err, 0, NULL))) {
        rc = SR_ERR_LY;
        goto end;
    }
    /* while a change or a repair runs the device is half way; show the last state
     * read outside one instead of waiting (waiting here could stall sysrepo) */
    if (pthread_mutex_trylock(&g.txn_lock) == 0) {
        int bad = g.a->read(g.a, &dev, err, sizeof(err));
        pthread_mutex_unlock(&g.txn_lock);
        if (bad) {
            logf_("oper: %s", err);
            rc = SR_ERR_OPERATION_FAILED;
            goto end;
        }
        pthread_mutex_lock(&g.lock);
        g.seen_valid = lsw_state_copy(&g.seen, &dev) == 0;
        pthread_mutex_unlock(&g.lock);
    } else {
        pthread_mutex_lock(&g.lock);
        if (g.seen_valid)
            lsw_state_copy(&dev, &g.seen);
        pthread_mutex_unlock(&g.lock);
    }
    for (int i = 0; i < dev.nports; i++) {
        const struct lsw_port *p = &dev.port[i];
        /* keys are passed as values, never pasted into a path string */
        if (lyd_new_list(*parent, mod, "port", 0, &pn, p->name)) {
            rc = SR_ERR_LY;
            goto end;
        }
        for (int v = 1; v <= LSW_VID_MAX; v++) {
            if (!p->vid[v])
                continue;
            snprintf(vid, sizeof(vid), "%d", v);
            if (lyd_new_list(pn, mod, "vlan", 0, &vn, vid) ||
                lyd_new_term(vn, mod, "pvid", p->vid[v] & LSW_F_PVID ? "true" : "false", 0, NULL) ||
                lyd_new_term(vn, mod, "egress", p->vid[v] & LSW_F_UNTAGGED ? "untagged" : "tagged", 0, NULL)) {
                rc = SR_ERR_LY;
                goto end;
            }
        }
    }
end:
    sr_session_release_context(sess);
    lsw_state_free(&dev);
    return rc;
}

static void usage(void)
{
    fprintf(stderr, "usage: labswd [-b bridge] [-f /path/to/vtysh [-N frr-instance]] [-P priority] [-R seconds] [-D ms] [-B backup-dir] [-v]\n"
#ifdef LSW_TESTING
                    "       labswd -a mock -m state.json [-r routes.json] [-t trace.txt] [-F n] [-G n] [-K n]   (test build)\n"
#endif
    );
}

int main(int argc, char **argv)
{
    const char *bridge = "br0", *vtysh = NULL, *frr_ns = NULL, *backup_dir = "/var/lib/labsw/backup";
#ifdef LSW_TESTING
    const char *adapter = "bridge", *mock_state = NULL, *trace = NULL, *mock_routes = NULL;
#endif
    struct lsw_route_adapter *ra = NULL;
    struct lsw_routes *rwant = NULL;
    int route_fail_at = 0;
    long backup_fail_at = 0;
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    sr_subscription_ctx_t *sub = NULL;
    sigset_t sigs;
    uint32_t prio = 0;
    int opt, rc = 1, sig, every = 0, deadline = 3000;

    setvbuf(stdout, NULL, _IOLBF, 0);
    while ((opt = getopt(argc, argv, "b:f:N:P:R:D:B:a:m:r:t:F:G:K:vh")) != -1) {
        switch (opt) {
        case 'b': bridge = optarg; break;
        case 'v': g.verbose = 1; break;
        case 'P': prio = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'R': every = atoi(optarg); break;
        case 'D': deadline = atoi(optarg); break;
        case 'f': vtysh = optarg; break;
        case 'N': frr_ns = optarg; break;
        case 'B': backup_dir = optarg; break;
#ifdef LSW_TESTING
        case 'a': adapter = optarg; break;
        case 'm': mock_state = optarg; break;
        case 't': trace = optarg; break;
        case 'F': g.txn.fail_at = atoi(optarg); break;
        case 'G': route_fail_at = atoi(optarg); break;
        case 'r': mock_routes = optarg; break;
        case 'K': backup_fail_at = atol(optarg); break;
#endif
        default: usage(); return 2;
        }
    }
#ifdef LSW_TESTING
    if (!strcmp(adapter, "mock"))
        g.a = mock_state ? lsw_mock_adapter(mock_state, trace) : NULL;
    else
#endif
        g.a = lsw_bridge_adapter(bridge);
    if (!g.a) {
        usage();
        return 2;
    }
#ifdef LSW_TESTING
    if (mock_routes)
        ra = lsw_mock_route_adapter(mock_routes, trace);
    else
#endif
    if (vtysh && !(ra = lsw_frr_adapter(vtysh, frr_ns))) {
        fprintf(stderr, "labswd: -f needs an absolute path to vtysh\n");
        return 2;
    }
    if (ra && (routes_init(ra, route_fail_at, deadline, log_line) || !(rwant = malloc(sizeof(*rwant)))))
        return 1;
    for (int i = 0; i < MAX_PENDING; i++)
        if (lsw_state_init(&g.pending[i].snap) || lsw_state_init(&g.pending[i].want))
            return 1;
    if (lsw_state_init(&g.seen))
        return 1;
    g.txn.a = g.a;
    g.txn.deadline_ms = deadline; /* below sysrepo's 5 s callback timeout */
    g.txn.log = log_line;

    /* signals are taken by sigwait() below, not by a handler */
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL); /* before sysrepo starts its threads */

    if ((rc = sr_connect(0, &conn)) || (rc = sr_session_start(conn, SR_DS_RUNNING, &sess)) ||
        (rc = sr_session_start(conn, SR_DS_OPERATIONAL, &g.nsess))) {
        logf_("sysrepo: %s", sr_strerror(rc));
        goto end;
    }
    if ((rc = sr_oper_get_subscribe(sess, MODULE, "/" MODULE ":switch-state", on_oper, NULL, 0, &sub))) {
        logf_("oper subscribe: %s", sr_strerror(rc));
        goto end;
    }
    if ((rc = sr_module_change_subscribe(sess, MODULE, NULL, on_change, NULL, prio, SR_SUBSCR_ENABLED, &sub))) {
        logf_("change subscribe: %s (could not apply the running configuration)", sr_strerror(rc));
        goto end;
    }
    if (ra && ((rc = sr_oper_get_subscribe(sess, ROUTE_MODULE, "/" ROUTE_MODULE ":routes-state", on_route_oper, NULL,
                                           0, &sub)) ||
               (rc = sr_module_change_subscribe(sess, ROUTE_MODULE, NULL, on_route_change, NULL, prio,
                                                SR_SUBSCR_ENABLED, &sub)))) {
        logf_("%s subscribe: %s", ROUTE_MODULE, sr_strerror(rc));
        goto end;
    }
    if ((rc = ops_start(conn, backup_dir, backup_fail_at, log_line)))
        goto end;
    logf_("running (adapter %s%s%s)", g.a->name, ra ? ", " : "", ra ? ra->name : "");
    for (;;) {
        struct timespec ts = {.tv_sec = every};
        sig = every > 0 ? sigtimedwait(&sigs, NULL, &ts) : sigwaitinfo(&sigs, NULL);
        if (sig > 0)
            break;
        if (every > 0) /* timeout (or EINTR): one round */
            reconcile(sess, rwant);
    }
    logf_("signal %d, stopping", sig);
    rc = 0;
end:
    ops_stop();
    sr_unsubscribe(sub);
    sr_disconnect(conn);
    g.a->destroy(g.a);
    routes_cleanup();
    for (int i = 0; i < MAX_PENDING; i++) {
        lsw_state_free(&g.pending[i].snap);
        lsw_state_free(&g.pending[i].want);
    }
    free(rwant);
    lsw_state_free(&g.seen);
    return rc ? 1 : 0;
}
