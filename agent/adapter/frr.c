/* frr.c - static routes in FRR through vtysh
 *
 * read:  vtysh -c "show mgmt datastore-contents running json"
 * add:   vtysh -c "configure terminal" -c "ip route P GW|blackhole"
 * del:   vtysh -c "configure terminal" -c "no ip route P GW|blackhole"
 * vtysh exits with 1 when FRR rejects a command ("% Unknown command", ...).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exec.h"
#include "route_adapter.h"

#define TIMEOUT_MS 2000 /* one vtysh; an apply as a whole has the deadline in rtxn */

struct frr {
    char vtysh[128];
    char ns[64]; /* FRR instance name (vtysh -N), empty for the default one */
};

/* argv: vtysh [-N ns] then the given -c commands */
static int vtysh(struct frr *f, const char *c1, const char *c2, struct lsw_exec_result *r, char *err, size_t len)
{
    char *argv[9];
    int n = 0;

    argv[n++] = f->vtysh;
    if (f->ns[0]) {
        argv[n++] = "-N";
        argv[n++] = f->ns;
    }
    argv[n++] = "-c";
    argv[n++] = (char *)c1;
    if (c2) {
        argv[n++] = "-c";
        argv[n++] = (char *)c2;
    }
    argv[n] = NULL;
    if (lsw_exec(argv, TIMEOUT_MS, r)) {
        snprintf(err, len, "cannot run %s", f->vtysh);
        return -1;
    }
    if (r->timedout || r->toolong || r->status != 0) {
        /* FRR prints its error to stdout ("% Unknown command: ...") */
        const char *e = r->out && r->out[0] ? r->out : r->err ? r->err : "";
        size_t m = strcspn(e, "\n");
        snprintf(err, len, "vtysh \"%s\": %.*s", c2 ? c2 : c1, (int)(m > 200 ? 200 : m),
                 r->timedout ? "timed out" : r->toolong ? "output too long" : m ? e : "failed");
        lsw_exec_free(r);
        return -1;
    }
    return 0;
}

static int frr_read(struct lsw_route_adapter *a, struct lsw_routes *out, char *err, size_t len)
{
    struct lsw_exec_result r;
    int rc;

    if (vtysh(a->priv, "show mgmt datastore-contents running json", NULL, &r, err, len))
        return -1;
    rc = lsw_frr_parse(r.out, out, err, len);
    lsw_exec_free(&r);
    return rc;
}

static int change(struct lsw_route_adapter *a, const struct lsw_route *rt, int del, char *err, size_t len)
{
    struct lsw_exec_result r;
    char cmd[80];

    snprintf(cmd, sizeof(cmd), "%sip route %s %s", del ? "no " : "", rt->prefix,
             rt->gateway[0] ? rt->gateway : "blackhole");
    if (vtysh(a->priv, "configure terminal", cmd, &r, err, len))
        return -1;
    lsw_exec_free(&r);
    return 0;
}

static int frr_add(struct lsw_route_adapter *a, const struct lsw_route *r, char *err, size_t len)
{
    return change(a, r, 0, err, len);
}

static int frr_del(struct lsw_route_adapter *a, const struct lsw_route *r, char *err, size_t len)
{
    return change(a, r, 1, err, len);
}

static void frr_destroy(struct lsw_route_adapter *a)
{
    free(a->priv);
    free(a);
}

struct lsw_route_adapter *lsw_frr_adapter(const char *path, const char *ns)
{
    struct lsw_route_adapter *a = calloc(1, sizeof(*a));
    struct frr *f = calloc(1, sizeof(*f));

    if (!a || !f || path[0] != '/' || strlen(path) >= sizeof(f->vtysh) || (ns && strlen(ns) >= sizeof(f->ns))) {
        free(a);
        free(f);
        return NULL;
    }
    snprintf(f->vtysh, sizeof(f->vtysh), "%s", path);
    snprintf(f->ns, sizeof(f->ns), "%s", ns ? ns : "");
    a->name = "frr";
    a->read = frr_read;
    a->add = frr_add;
    a->del = frr_del;
    a->destroy = frr_destroy;
    a->priv = f;
    return a;
}
