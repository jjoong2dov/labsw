/* rtxn.c
 * SPDX-License-Identifier: MIT */
#include "rtxn.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void logf_(struct lsw_rtxn *t, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    if (!t->log)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    t->log(buf);
}

static int run_step(struct lsw_rtxn *t, const struct lsw_route_step *s, char *err, size_t len)
{
    char txt[96];
    int rc;

    lsw_route_step_text(s, txt, sizeof(txt));
    if (!t->restoring && t->fail_at && ++t->cmd_count == t->fail_at) {
        snprintf(err, len, "injected failure at command %d (%s)", t->fail_at, txt);
        rc = -1;
    } else {
        rc = s->op == LSW_RT_ADD ? t->a->add(t->a, &s->r, err, len) : t->a->del(t->a, &s->r, err, len);
    }
    logf_(t, "%s%s -> %s", t->restoring ? "restore: " : "", txt, rc ? "FAILED" : "ok");
    return rc;
}

/* plan from what FRR has to target and run it; *done = steps that succeeded */
static int converge(struct lsw_rtxn *t, const struct lsw_routes *target, struct lsw_routes *now,
                    struct lsw_route_plan *plan, int *done, char *err, size_t len)
{
    *done = 0;
    if (t->a->read(t->a, now, err, len))
        return -1;
    lsw_route_plan(now, target, plan);
    for (int i = 0; i < plan->n; i++, (*done)++)
        if (run_step(t, &plan->s[i], err, len))
            return -1;
    return 0;
}

/* undo steps [0, done) in reverse order: add <-> del */
static int undo(struct lsw_rtxn *t, const struct lsw_route_plan *plan, int done, char *err, size_t len)
{
    t->restoring = 1;
    for (int i = done - 1; i >= 0; i--) {
        struct lsw_route_step s = plan->s[i];
        s.op = s.op == LSW_RT_ADD ? LSW_RT_DEL : LSW_RT_ADD;
        if (run_step(t, &s, err, len)) {
            t->restoring = 0;
            return -1;
        }
    }
    t->restoring = 0;
    return 0;
}

static int verify(struct lsw_rtxn *t, const struct lsw_routes *snap, struct lsw_routes *now, char *err, size_t len)
{
    char why[160];

    if (t->a->read(t->a, now, err, len))
        return -1;
    if (lsw_routes_diff(now, snap, why, sizeof(why))) {
        snprintf(err, len, "restore incomplete: %s", why);
        return -1;
    }
    return 0;
}

/* after a complete apply (ABORT): FRR is readable, plan back to snap */
int lsw_rtxn_restore(struct lsw_rtxn *t, const struct lsw_routes *snap, char *err, size_t errlen)
{
    struct lsw_routes *now = malloc(sizeof(*now));
    struct lsw_route_plan *plan = malloc(sizeof(*plan));
    int rc = -1, done;

    if (now && plan) {
        t->restoring = 1;
        rc = converge(t, snap, now, plan, &done, err, errlen);
        t->restoring = 0;
        if (!rc)
            rc = verify(t, snap, now, err, errlen);
    }
    free(now);
    free(plan);
    return rc;
}

enum lsw_txn_result lsw_rtxn_apply(struct lsw_rtxn *t, const struct lsw_routes *want, struct lsw_routes *snapshot,
                                   char *err, size_t errlen)
{
    struct lsw_routes *now = malloc(sizeof(*now));
    struct lsw_route_plan *plan = malloc(sizeof(*plan));
    char why[160], rerr[256];
    enum lsw_txn_result res = LSW_TXN_FAILED;
    int done = 0;

    err[0] = '\0';
    if (!now || !plan) {
        snprintf(err, errlen, "out of memory");
        goto end;
    }
    t->cmd_count = 0;
    if (t->a->read(t->a, snapshot, err, errlen))
        goto end; /* nothing was changed */
    if (converge(t, want, now, plan, &done, err, errlen) == 0 && t->a->read(t->a, now, err, errlen) == 0) {
        if (!lsw_routes_diff(now, want, why, sizeof(why))) {
            res = LSW_TXN_OK;
            goto end;
        }
        snprintf(err, errlen, "routes differ after apply: %s", why);
    }
    logf_(t, "apply failed (%s), restoring", err);
    /* undo exactly what ran: after "add new" without "del old" a prefix has
     * two next hops, which read() refuses, so planning from a read would fail */
    if (undo(t, plan, done, rerr, sizeof(rerr)) || verify(t, snapshot, now, rerr, sizeof(rerr))) {
        logf_(t, "restore failed: %s", rerr);
        snprintf(err + strlen(err), errlen - strlen(err), "; restore failed: %s", rerr);
        res = LSW_TXN_FAILED_DEGRADED;
    } else {
        logf_(t, "previous routes restored and verified");
    }
end:
    free(now);
    free(plan);
    return res;
}
