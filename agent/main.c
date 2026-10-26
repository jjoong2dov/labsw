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
 * With -R n the agent also compares running with the device every n seconds and
 * puts back what someone changed outside NETCONF (policy: NETCONF wins). It
 * skips a round while a change is between CHANGE and DONE/ABORT.
 *
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <getopt.h>
#include <pthread.h>
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
#include "routes.h"

#define MODULE "labsw-switch"
#define MAX_PENDING 8

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
        struct lsw_state snap;
    } pending[MAX_PENDING];
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

static int pending_slot(uint32_t op_id, int create)
{
    int free_slot = -1, used = 0, last = -1;
    for (int i = 0; i < MAX_PENDING; i++) {
        if (g.pending[i].used && g.pending[i].op_id == op_id)
            return i;
        if (g.pending[i].used) {
            used++;
            last = i;
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (!create) {
        /* sysrepo 5.1.0 passes the request id, not the operation id, to an ABORT it
         * makes itself (a subscriber of the same priority failed; shm_sub.c). Changes
         * of one module are serialized, so a single open change is the one aborted. */
        if (used == 1 && op_id != g.pending[last].op_id) {
            logf_("op %u: no such change, taking the only open one (op %u)", op_id, g.pending[last].op_id);
            return last;
        }
        return -1;
    }
    if (free_slot < 0)
        return -1;
    g.pending[free_slot].used = 1;
    g.pending[free_slot].op_id = op_id;
    return free_slot;
}

static int on_change_locked(sr_session_ctx_t *sess, sr_event_t event, uint32_t op_id)
{
    struct lsw_state want, snap;
    char err[1024] = "";
    int rc = SR_ERR_OK, slot;

    if (event == SR_EV_DONE || event == SR_EV_ABORT) {
        slot = pending_slot(op_id, 0);
        if (slot < 0) {
            /* e.g. the DONE that follows ENABLED; an ABORT here means we lost a snapshot */
            if (event == SR_EV_ABORT)
                logf_("op %u: ABORT for a change we do not know", op_id);
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
    logf_("op %u: %s", op_id, event == SR_EV_ENABLED ? "start, applying running" : "change");
    if (read_wanted(sess, &want, err, sizeof(err))) {
        sr_session_set_netconf_error(sess, "application", "operation-failed", NULL, NULL, err, 0);
        rc = SR_ERR_OPERATION_FAILED;
        goto end;
    }
    if ((rc = validate(sess, &want, err, sizeof(err))) != SR_ERR_OK)
        goto end;
    switch (lsw_txn_apply(&g.txn, &want, &snap, err, sizeof(err))) {
    case LSW_TXN_OK:
        set_health(0, NULL);
        if (event == SR_EV_CHANGE) {
            slot = pending_slot(op_id, 1);
            if (slot >= 0)
                lsw_state_copy(&g.pending[slot].snap, &snap);
            else
                logf_("op %u: too many open changes, an ABORT could not be undone", op_id);
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

    pthread_mutex_lock(&g.txn_lock);
    rc = on_change_locked(sess, event, op_id);
    pthread_mutex_unlock(&g.txn_lock);
    return rc;
}

/* make the device equal to running again if something else changed it */
static void reconcile(sr_session_ctx_t *sess)
{
    struct lsw_state want, dev, snap;
    char err[1024], why[160];

    if (lsw_state_init(&want) || lsw_state_init(&dev) || lsw_state_init(&snap))
        goto end;
    pthread_mutex_lock(&g.txn_lock);
    for (int i = 0; i < MAX_PENDING; i++)
        if (g.pending[i].used)
            goto unlock; /* a change is open; its DONE or ABORT decides */
    if (read_wanted(sess, &want, err, sizeof(err)) || g.a->read(g.a, &dev, err, sizeof(err))) {
        logf_("reconcile: %s", err);
        goto unlock;
    }
    if (!lsw_state_diff(&dev, &want, why, sizeof(why)))
        goto unlock;
    logf_("reconcile: device differs from running (%s), applying running", why);
    switch (lsw_txn_apply(&g.txn, &want, &snap, err, sizeof(err))) {
    case LSW_TXN_OK:
        pthread_mutex_lock(&g.lock);
        g.repairs++;
        pthread_mutex_unlock(&g.lock);
        set_health(0, NULL);
        logf_("reconcile: ok");
        break;
    case LSW_TXN_FAILED:
        set_health(1, err); /* the device still differs from running */
        logf_("reconcile: %s", err);
        break;
    case LSW_TXN_FAILED_DEGRADED:
        set_health(1, err);
        logf_("reconcile: %s", err);
        break;
    }
unlock:
    pthread_mutex_unlock(&g.txn_lock);
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
    if (g.a->read(g.a, &dev, err, sizeof(err))) {
        logf_("oper: %s", err);
        rc = SR_ERR_OPERATION_FAILED;
        goto end;
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
    fprintf(stderr, "usage: labswd [-b bridge] [-f /path/to/vtysh [-N frr-instance]] [-P priority] [-R seconds] [-v]\n"
#ifdef LSW_TESTING
                    "       labswd -a mock -m state.json [-r routes.json] [-t trace.txt] [-F n] [-G n]   (test build)\n"
#endif
    );
}

int main(int argc, char **argv)
{
    const char *bridge = "br0", *vtysh = NULL, *frr_ns = NULL;
#ifdef LSW_TESTING
    const char *adapter = "bridge", *mock_state = NULL, *trace = NULL, *mock_routes = NULL;
#endif
    struct lsw_route_adapter *ra = NULL;
    int route_fail_at = 0;
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    sr_subscription_ctx_t *sub = NULL;
    sigset_t sigs;
    uint32_t prio = 0;
    int opt, rc = 1, sig, every = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    while ((opt = getopt(argc, argv, "b:f:N:P:R:a:m:r:t:F:G:vh")) != -1) {
        switch (opt) {
        case 'b': bridge = optarg; break;
        case 'v': g.verbose = 1; break;
        case 'P': prio = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'R': every = atoi(optarg); break;
        case 'f': vtysh = optarg; break;
        case 'N': frr_ns = optarg; break;
#ifdef LSW_TESTING
        case 'a': adapter = optarg; break;
        case 'm': mock_state = optarg; break;
        case 't': trace = optarg; break;
        case 'F': g.txn.fail_at = atoi(optarg); break;
        case 'G': route_fail_at = atoi(optarg); break;
        case 'r': mock_routes = optarg; break;
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
    if (ra && routes_init(ra, route_fail_at, log_line))
        return 1;
    for (int i = 0; i < MAX_PENDING; i++)
        if (lsw_state_init(&g.pending[i].snap))
            return 1;
    g.txn.a = g.a;
    g.txn.log = log_line;

    /* signals are taken by sigwait() below, not by a handler */
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL); /* before sysrepo starts its threads */

    if ((rc = sr_connect(0, &conn)) || (rc = sr_session_start(conn, SR_DS_RUNNING, &sess))) {
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
    logf_("running (adapter %s%s%s)", g.a->name, ra ? ", " : "", ra ? ra->name : "");
    for (;;) {
        struct timespec ts = {.tv_sec = every};
        sig = every > 0 ? sigtimedwait(&sigs, NULL, &ts) : sigwaitinfo(&sigs, NULL);
        if (sig > 0)
            break;
        if (every > 0) { /* timeout (or EINTR): one round */
            reconcile(sess);
            if (ra) {
                pthread_mutex_lock(&g.txn_lock);
                routes_reconcile(sess);
                pthread_mutex_unlock(&g.txn_lock);
            }
        }
    }
    logf_("signal %d, stopping", sig);
    rc = 0;
end:
    sr_unsubscribe(sub);
    sr_disconnect(conn);
    g.a->destroy(g.a);
    routes_cleanup();
    for (int i = 0; i < MAX_PENDING; i++)
        lsw_state_free(&g.pending[i].snap);
    return rc ? 1 : 0;
}
