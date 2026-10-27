/* frr_parse.c - static routes from FRR's own YANG data (mgmtd, JSON)
 *
 * `vtysh -c "show mgmt datastore-contents running json"` prints the running
 * configuration as frr-routing JSON. Static routes are in
 *   frr-routing:routing / control-plane-protocols / control-plane-protocol[]
 *     (type frr-staticd:staticd, vrf) / frr-staticd:staticd / route-list[]
 *       prefix, afi-safi, src-prefix, path-list[] (table-id, distance,
 *       frr-nexthops / nexthop[] (nh-type, gateway, vrf, interface, bh-type))
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

#include <json-c/json.h>

#include "route_adapter.h"

static const char *str(struct json_object *o, const char *key)
{
    struct json_object *v;
    return json_object_object_get_ex(o, key, &v) && json_object_is_type(v, json_type_string) ? json_object_get_string(v)
                                                                                           : NULL;
}

static struct json_object *get(struct json_object *o, const char *key, enum json_type type)
{
    struct json_object *v;
    return o && json_object_object_get_ex(o, key, &v) && json_object_is_type(v, type) ? v : NULL;
}

/* every key of o is in the NULL-terminated list; else the first other key */
static const char *only_keys(struct json_object *o, const char *const *keys)
{
    json_object_object_foreach(o, k, v) {
        int known = 0;
        (void)v;
        for (const char *const *p = keys; *p; p++)
            known |= !strcmp(k, *p);
        if (!known)
            return k;
    }
    return NULL;
}

static const char *const ROUTE_KEYS[] = {"prefix", "src-prefix", "afi-safi", "path-list", NULL};
static const char *const PATH_KEYS[] = {"table-id", "distance", "frr-nexthops", NULL};
static const char *const NH_KEYS[] = {"nh-type", "vrf", "gateway", "interface", "bh-type", NULL};

static int one_route(struct json_object *rt, struct lsw_routes *out, char *err, size_t len)
{
    const char *prefix = str(rt, "prefix"), *afi = str(rt, "afi-safi"), *src = str(rt, "src-prefix"), *k;
    struct json_object *paths = get(rt, "path-list", json_type_array), *path, *nhs, *nh, *v;
    const char *type, *gw, *ifname, *vrf, *bh;

    if (!prefix || !afi) {
        snprintf(err, len, "FRR route without prefix or afi-safi");
        return -1;
    }
    if (strcmp(afi, "frr-routing:ipv4-unicast"))
        return 0; /* not IPv4: not ours, leave it alone */
    /* anything we do not know (a tag, a metric, onlink, ...) means we cannot express the route */
    if ((k = only_keys(rt, ROUTE_KEYS))) {
        snprintf(err, len, "route %s: unknown attribute %s", prefix, k);
        return -1;
    }
    if (src && strcmp(src, "::/0") && strcmp(src, "0.0.0.0/0")) {
        snprintf(err, len, "route %s: source prefix %s is not supported", prefix, src);
        return -1;
    }
    if (!paths || json_object_array_length(paths) != 1) {
        snprintf(err, len, "route %s: %zu paths (distances), only one is supported", prefix,
                 paths ? json_object_array_length(paths) : 0);
        return -1;
    }
    path = json_object_array_get_idx(paths, 0);
    if ((k = only_keys(path, PATH_KEYS))) {
        snprintf(err, len, "route %s: unknown attribute %s", prefix, k);
        return -1;
    }
    if (!(v = get(path, "table-id", json_type_int)) || json_object_get_int(v) != 0 ||
        !(v = get(path, "distance", json_type_int)) || json_object_get_int(v) != 1) {
        snprintf(err, len, "route %s: only table 0 and distance 1 are supported", prefix);
        return -1;
    }
    nhs = get(get(path, "frr-nexthops", json_type_object), "nexthop", json_type_array);
    if (!nhs || json_object_array_length(nhs) != 1) {
        snprintf(err, len, "route %s: %zu next hops, only one is supported", prefix,
                 nhs ? json_object_array_length(nhs) : 0);
        return -1;
    }
    nh = json_object_array_get_idx(nhs, 0);
    if ((k = only_keys(nh, NH_KEYS))) {
        snprintf(err, len, "route %s: unknown next hop attribute %s", prefix, k);
        return -1;
    }
    type = str(nh, "nh-type");
    gw = str(nh, "gateway");
    ifname = str(nh, "interface");
    vrf = str(nh, "vrf");
    bh = str(nh, "bh-type");
    /* FRR 10.3 prints an unset interface as the text "(null)" */
    if (ifname && *ifname && strcmp(ifname, "(null)")) {
        snprintf(err, len, "route %s: next hop interface %s is not supported", prefix, ifname);
        return -1;
    }
    if (vrf && strcmp(vrf, "default")) {
        snprintf(err, len, "route %s: next hop in VRF %s is not supported", prefix, vrf);
        return -1;
    }
    if (lsw_routes_find(out, prefix)) {
        snprintf(err, len, "route %s appears twice", prefix);
        return -1;
    }
    if (type && !strcmp(type, "blackhole")) {
        if (bh && strcmp(bh, "unspec")) { /* reject, null: not the same as our blackhole */
            snprintf(err, len, "route %s: blackhole type %s is not supported", prefix, bh);
            return -1;
        }
        return lsw_routes_set(out, prefix, "", err, len);
    }
    if (type && !strcmp(type, "ip4") && gw)
        return lsw_routes_set(out, prefix, gw, err, len);
    snprintf(err, len, "route %s: next hop type %s is not supported", prefix, type ? type : "(none)");
    return -1;
}

int lsw_frr_parse(const char *json, struct lsw_routes *out, char *err, size_t len)
{
    struct json_tokener *tok = json_tokener_new();
    struct json_object *root, *protos;
    int rc = -1;

    lsw_routes_clear(out);
    if (!tok)
        return -1;
    root = json_tokener_parse_ex(tok, json, (int)strlen(json));
    /* a cut-off output does not parse; text after the object is not accepted either */
    if (!root || json_tokener_get_error(tok) != json_tokener_success || !json_object_is_type(root, json_type_object) ||
        json[json_tokener_get_parse_end(tok) + strspn(json + json_tokener_get_parse_end(tok), " \r\n\t")]) {
        snprintf(err, len, "FRR output is not one JSON object");
        goto end;
    }
    /* FRR 10.3 prints {} when nothing was configured since it started: no routes.
     * Any other shape we do not know is an error, never "no routes" (that would add
     * every route of running on top of what FRR has): without frr-routing:routing
     * only an empty object is accepted. */
    if (!json_object_object_get_ex(root, "frr-routing:routing", &protos)) {
        if (json_object_object_length(root) == 0)
            rc = 0;
        else
            snprintf(err, len, "FRR output has neither frr-routing:routing nor is it empty");
        goto end;
    }
    protos = get(get(get(root, "frr-routing:routing", json_type_object), "control-plane-protocols", json_type_object),
                 "control-plane-protocol", json_type_array);
    if (!protos) {
        snprintf(err, len, "FRR output has no frr-routing:routing/control-plane-protocols/control-plane-protocol");
        goto end;
    }
    for (size_t i = 0; i < json_object_array_length(protos); i++) {
        struct json_object *p = json_object_array_get_idx(protos, i), *sd, *routes;
        const char *type = str(p, "type"), *vrf = str(p, "vrf");
        if (!type || strcmp(type, "frr-staticd:staticd"))
            continue;
        if (!vrf || strcmp(vrf, "default")) {
            if ((routes = get(get(p, "frr-staticd:staticd", json_type_object), "route-list", json_type_array)) &&
                json_object_array_length(routes)) {
                snprintf(err, len, "static route %s in VRF %s is not supported",
                         str(json_object_array_get_idx(routes, 0), "prefix"), vrf ? vrf : "(none)");
                goto end;
            }
            continue;
        }
        if (!json_object_object_get_ex(p, "frr-staticd:staticd", &sd))
            continue; /* no static routes at all */
        if (!json_object_is_type(sd, json_type_object) ||
            (json_object_object_get_ex(sd, "route-list", &routes) && !json_object_is_type(routes, json_type_array))) {
            snprintf(err, len, "FRR output: unexpected frr-staticd:staticd");
            goto end;
        }
        routes = get(sd, "route-list", json_type_array);
        for (size_t j = 0; routes && j < json_object_array_length(routes); j++)
            if (one_route(json_object_array_get_idx(routes, j), out, err, len))
                goto end;
    }
    rc = 0; /* no staticd entry: no static routes */
end:
    json_object_put(root);
    json_tokener_free(tok);
    return rc;
}
