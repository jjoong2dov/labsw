/* mock_route.c - static routes kept in a JSON file, for tests without FRR
 *   {"routes": {"192.0.2.0/24": ["198.51.100.1"], "203.0.113.0/24": ["blackhole"]}}
 * Like FRR, a prefix can have several next hops (add a second gateway, and the
 * first one stays); read() refuses those, as the FRR adapter does. Deleting a
 * route that is not there "succeeds", as vtysh does (exit code 0).
 * SPDX-License-Identifier: MIT */
#ifdef LSW_TESTING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "route_adapter.h"

struct mockr {
    char *state_file;
    char *trace_file;
};

static int load(struct mockr *m, struct lsw_routes *s, char *err, size_t len)
{
    struct json_object *root = json_object_from_file(m->state_file), *routes;
    int rc = 0;

    lsw_routes_clear(s);
    if (!root) {
        snprintf(err, len, "mock: cannot read %s", m->state_file);
        return -1;
    }
    if (json_object_object_get_ex(root, "routes", &routes)) {
        json_object_object_foreach(routes, prefix, nhs) {
            const char *g;
            if (json_object_array_length(nhs) != 1) {
                snprintf(err, len, "route %s: %zu next hops, only one is supported", prefix,
                         json_object_array_length(nhs));
                rc = -1;
                break;
            }
            g = json_object_get_string(json_object_array_get_idx(nhs, 0));
            if ((rc = lsw_routes_set(s, prefix, strcmp(g, "blackhole") ? g : "", err, len)))
                break;
        }
    }
    json_object_put(root);
    return rc;
}

static int mock_read(struct lsw_route_adapter *a, struct lsw_routes *out, char *err, size_t len)
{
    return load(a->priv, out, err, len);
}

static int change(struct lsw_route_adapter *a, const struct lsw_route *r, int del, char *err, size_t len)
{
    struct mockr *m = a->priv;
    struct json_object *root = json_object_from_file(m->state_file), *routes, *nhs;
    const char *gw = r->gateway[0] ? r->gateway : "blackhole";
    char tmp[4096];
    int rc = -1, at = -1;
    FILE *f;

    if (!root) {
        snprintf(err, len, "mock: cannot read %s", m->state_file);
        return -1;
    }
    if (!json_object_object_get_ex(root, "routes", &routes)) {
        routes = json_object_new_object();
        json_object_object_add(root, "routes", routes);
    }
    if (!json_object_object_get_ex(routes, r->prefix, &nhs)) {
        nhs = json_object_new_array();
        json_object_object_add(routes, r->prefix, nhs);
    }
    for (size_t i = 0; i < json_object_array_length(nhs); i++)
        if (!strcmp(json_object_get_string(json_object_array_get_idx(nhs, i)), gw))
            at = (int)i;
    if (del) {
        if (at < 0) {
            /* like FRR 10.3: "% Refusing to remove a non-existent route", exit code 0 */
            if (m->trace_file && (f = fopen(m->trace_file, "a"))) {
                fprintf(f, "del route %s %s (not there)\n", r->prefix, gw);
                fclose(f);
            }
            rc = 0;
            goto end;
        }
        json_object_array_del_idx(nhs, (size_t)at, 1);
        if (json_object_array_length(nhs) == 0)
            json_object_object_del(routes, r->prefix);
    } else if (at < 0) {
        json_object_array_add(nhs, json_object_new_string(gw));
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", m->state_file);
    if (json_object_to_file_ext(tmp, root, JSON_C_TO_STRING_PRETTY) || rename(tmp, m->state_file)) {
        snprintf(err, len, "mock: cannot write %s", m->state_file);
        goto end;
    }
    if (m->trace_file && (f = fopen(m->trace_file, "a"))) {
        fprintf(f, "%s route %s %s\n", del ? "del" : "add", r->prefix, gw);
        fclose(f);
    }
    rc = 0;
end:
    json_object_put(root);
    return rc;
}

static int mock_add(struct lsw_route_adapter *a, const struct lsw_route *r, char *err, size_t len)
{
    return change(a, r, 0, err, len);
}

static int mock_del(struct lsw_route_adapter *a, const struct lsw_route *r, char *err, size_t len)
{
    return change(a, r, 1, err, len);
}

static void mock_destroy(struct lsw_route_adapter *a)
{
    struct mockr *m = a->priv;
    free(m->state_file);
    free(m->trace_file);
    free(m);
    free(a);
}

struct lsw_route_adapter *lsw_mock_route_adapter(const char *state_file, const char *trace_file)
{
    struct lsw_route_adapter *a = calloc(1, sizeof(*a));
    struct mockr *m = calloc(1, sizeof(*m));

    if (!a || !m) {
        free(a);
        free(m);
        return NULL;
    }
    m->state_file = strdup(state_file);
    m->trace_file = trace_file ? strdup(trace_file) : NULL;
    a->name = "mock-route";
    a->read = mock_read;
    a->add = mock_add;
    a->del = mock_del;
    a->destroy = mock_destroy;
    a->priv = m;
    return a;
}

#endif
