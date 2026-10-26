/* route.c
 * SPDX-License-Identifier: MIT */
#include "route.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int lsw_route_gateway_ok(const char *addr)
{
    struct in_addr a;
    return strlen(addr) < LSW_ADDR_LEN && inet_pton(AF_INET, addr, &a) == 1;
}

int lsw_route_prefix_ok(const char *prefix, char *err, size_t len)
{
    char buf[LSW_PREFIX_LEN], *slash, *end;
    struct in_addr a;
    long plen;
    uint32_t host;

    if (strlen(prefix) >= sizeof(buf)) {
        snprintf(err, len, "bad prefix %s", prefix);
        return -1;
    }
    strcpy(buf, prefix);
    slash = strchr(buf, '/');
    if (!slash) {
        snprintf(err, len, "bad prefix %s", prefix);
        return -1;
    }
    *slash = '\0';
    plen = strtol(slash + 1, &end, 10);
    if (*end || slash[1] == '\0' || plen < 0 || plen > 32 || inet_pton(AF_INET, buf, &a) != 1) {
        snprintf(err, len, "bad prefix %s", prefix);
        return -1;
    }
    host = plen == 32 ? 0 : ntohl(a.s_addr) & (0xffffffffu >> plen);
    if (host) {
        snprintf(err, len, "prefix %s has host bits set", prefix);
        return -1;
    }
    return 0;
}

void lsw_routes_clear(struct lsw_routes *s)
{
    s->n = 0;
}

const struct lsw_route *lsw_routes_find(const struct lsw_routes *s, const char *prefix)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->r[i].prefix, prefix))
            return &s->r[i];
    return NULL;
}

int lsw_routes_set(struct lsw_routes *s, const char *prefix, const char *gateway, char *err, size_t len)
{
    struct lsw_route *r = (struct lsw_route *)lsw_routes_find(s, prefix);

    if (lsw_route_prefix_ok(prefix, err, len))
        return -1;
    if (gateway[0] && !lsw_route_gateway_ok(gateway)) {
        snprintf(err, len, "route %s: bad gateway %s", prefix, gateway);
        return -1;
    }
    if (!r) {
        if (s->n == LSW_ROUTES_MAX) {
            snprintf(err, len, "more than %d routes", LSW_ROUTES_MAX);
            return -1;
        }
        r = &s->r[s->n++];
        snprintf(r->prefix, sizeof(r->prefix), "%s", prefix);
    }
    snprintf(r->gateway, sizeof(r->gateway), "%s", gateway);
    return 0;
}

static void route_text(const struct lsw_route *r, char *buf, size_t len)
{
    snprintf(buf, len, "%s %s", r->prefix, r->gateway[0] ? r->gateway : "blackhole");
}

int lsw_routes_diff(const struct lsw_routes *a, const struct lsw_routes *b, char *why, size_t len)
{
    char t1[48], t2[48];

    for (int i = 0; i < a->n; i++) {
        const struct lsw_route *o = lsw_routes_find(b, a->r[i].prefix);
        route_text(&a->r[i], t1, sizeof(t1));
        if (!o) {
            snprintf(why, len, "%s only on one side", t1);
            return 1;
        }
        if (strcmp(o->gateway, a->r[i].gateway)) {
            route_text(o, t2, sizeof(t2));
            snprintf(why, len, "%s vs %s", t1, t2);
            return 1;
        }
    }
    for (int i = 0; i < b->n; i++) {
        if (!lsw_routes_find(a, b->r[i].prefix)) {
            route_text(&b->r[i], t1, sizeof(t1));
            snprintf(why, len, "%s only on one side", t1);
            return 1;
        }
    }
    return 0;
}

static void step(struct lsw_route_plan *p, enum lsw_route_op op, const struct lsw_route *r)
{
    p->s[p->n].op = op;
    p->s[p->n].r = *r;
    p->n++;
}

void lsw_route_plan(const struct lsw_routes *now, const struct lsw_routes *want, struct lsw_route_plan *p)
{
    p->n = 0;
    /* 1. new prefixes and new next hops */
    for (int i = 0; i < want->n; i++) {
        const struct lsw_route *o = lsw_routes_find(now, want->r[i].prefix);
        if (!o || strcmp(o->gateway, want->r[i].gateway))
            step(p, LSW_RT_ADD, &want->r[i]);
    }
    /* 2. old next hops of changed prefixes, and prefixes no longer wanted */
    for (int i = 0; i < now->n; i++) {
        const struct lsw_route *w = lsw_routes_find(want, now->r[i].prefix);
        if (!w || strcmp(w->gateway, now->r[i].gateway))
            step(p, LSW_RT_DEL, &now->r[i]);
    }
}

void lsw_route_step_text(const struct lsw_route_step *s, char *buf, size_t len)
{
    char t[48];
    route_text(&s->r, t, sizeof(t));
    snprintf(buf, len, "%s route %s", s->op == LSW_RT_ADD ? "add" : "del", t);
}
