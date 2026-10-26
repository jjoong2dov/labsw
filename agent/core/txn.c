/* txn.c
 * SPDX-License-Identifier: MIT */
#include "txn.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "plan.h"

static void logf_(struct lsw_txn *t, const char *fmt, ...)
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

static int run_step(struct lsw_txn *t, const struct lsw_step *s, char *err, size_t len)
{
    char txt[96];
    int rc;

    lsw_step_text(s, txt, sizeof(txt));
    if (!t->restoring && t->fail_at && ++t->cmd_count == t->fail_at) {
        snprintf(err, len, "injected failure at command %d (%s)", t->fail_at, txt);
        rc = -1;
    } else if (s->op == LSW_OP_DEL) {
        rc = t->a->del(t->a, s->port, s->vid, err, len);
    } else {
        rc = t->a->set(t->a, s->port, s->vid, s->flags, err, len);
    }
    logf_(t, "%s%s -> %s", t->restoring ? "restore: " : "", txt, rc ? "FAILED" : "ok");
    return rc;
}

/* plan from what the device has now to target, run it, stop at the first failure */
static int converge(struct lsw_txn *t, const struct lsw_state *target, struct lsw_state *now, char *err, size_t len)
{
    struct lsw_plan plan;
    int rc = -1;

    if (lsw_plan_init(&plan))
        return -1;
    if (t->a->read(t->a, now, err, len))
        goto end;
    if (lsw_plan_build(now, target, &plan, err, len))
        goto end;
    for (int i = 0; i < plan.n; i++)
        if (run_step(t, &plan.step[i], err, len))
            goto end;
    rc = 0;
end:
    lsw_plan_free(&plan);
    return rc;
}

int lsw_txn_restore(struct lsw_txn *t, const struct lsw_state *snap, char *err, size_t errlen)
{
    struct lsw_state now;
    char why[160];
    int rc = -1;

    if (lsw_state_init(&now))
        return -1;
    t->restoring = 1;
    if (converge(t, snap, &now, err, errlen))
        goto end;
    /* trust nothing: read the device again and compare */
    if (t->a->read(t->a, &now, err, errlen))
        goto end;
    if (lsw_state_diff(&now, snap, why, sizeof(why))) {
        snprintf(err, errlen, "restore incomplete: %s", why);
        goto end;
    }
    rc = 0;
end:
    t->restoring = 0;
    lsw_state_free(&now);
    return rc;
}

enum lsw_txn_result lsw_txn_apply(struct lsw_txn *t, const struct lsw_state *want, struct lsw_state *snapshot,
                                  char *err, size_t errlen)
{
    struct lsw_state now;
    char why[160], rerr[256];
    enum lsw_txn_result res = LSW_TXN_FAILED;

    err[0] = '\0';
    if (lsw_state_init(&now))
        return LSW_TXN_FAILED;
    t->cmd_count = 0;
    if (t->a->check(t->a, err, errlen))
        goto end; /* nothing was changed */
    if (t->a->read(t->a, snapshot, err, errlen))
        goto end;
    if (converge(t, want, &now, err, errlen) == 0 && t->a->read(t->a, &now, err, errlen) == 0) {
        if (!lsw_state_diff(&now, want, why, sizeof(why))) {
            res = LSW_TXN_OK;
            goto end;
        }
        snprintf(err, errlen, "device differs after apply: %s", why);
    }
    logf_(t, "apply failed (%s), restoring", err);
    if (lsw_txn_restore(t, snapshot, rerr, sizeof(rerr))) {
        logf_(t, "restore failed: %s", rerr);
        snprintf(err + strlen(err), errlen - strlen(err), "; restore failed: %s", rerr);
        res = LSW_TXN_FAILED_DEGRADED;
    } else {
        logf_(t, "previous state restored and verified");
    }
end:
    lsw_state_free(&now);
    return res;
}
