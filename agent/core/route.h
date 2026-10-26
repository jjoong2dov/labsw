/* route.h - IPv4 static routes: the set the agent wants or reads back, and the
 * plan from one set to another
 * SPDX-License-Identifier: MIT */
#ifndef LSW_ROUTE_H
#define LSW_ROUTE_H

#include <stddef.h>

#define LSW_ROUTES_MAX 1024
#define LSW_PREFIX_LEN 19 /* "255.255.255.255/32" + NUL */
#define LSW_ADDR_LEN 16

struct lsw_route {
    char prefix[LSW_PREFIX_LEN];
    char gateway[LSW_ADDR_LEN]; /* empty for a blackhole route */
};

struct lsw_routes {
    int n;
    struct lsw_route r[LSW_ROUTES_MAX];
};

/* normalized "a.b.c.d/len" with zero host bits; -1 if not (why in err) */
int lsw_route_prefix_ok(const char *prefix, char *err, size_t len);
int lsw_route_gateway_ok(const char *addr);

void lsw_routes_clear(struct lsw_routes *s);
/* add or replace; -1 on a bad value or when full */
int lsw_routes_set(struct lsw_routes *s, const char *prefix, const char *gateway, char *err, size_t len);
const struct lsw_route *lsw_routes_find(const struct lsw_routes *s, const char *prefix);
/* 0 if equal, else 1 and the first difference in why */
int lsw_routes_diff(const struct lsw_routes *a, const struct lsw_routes *b, char *why, size_t len);

enum lsw_route_op { LSW_RT_ADD, LSW_RT_DEL };
struct lsw_route_step {
    enum lsw_route_op op;
    struct lsw_route r;
};
struct lsw_route_plan {
    int n;
    struct lsw_route_step s[2 * LSW_ROUTES_MAX];
};

/*
 * Steps from now to want. A changed next hop is "add new, then delete old":
 * for a moment the prefix has two next hops (FRR installs both, ECMP) instead
 * of none. New prefixes are added before old ones are removed.
 */
void lsw_route_plan(const struct lsw_routes *now, const struct lsw_routes *want, struct lsw_route_plan *p);
void lsw_route_step_text(const struct lsw_route_step *s, char *buf, size_t len);

#endif
