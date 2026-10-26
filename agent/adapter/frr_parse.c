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
    return json_object_object_get_ex(o, key, &v) ? json_object_get_string(v) : NULL;
}

static struct json_object *get(struct json_object *o, const char *key)
{
    struct json_object *v;
    return o && json_object_object_get_ex(o, key, &v) ? v : NULL;
}

static int one_route(struct json_object *rt, struct lsw_routes *out, char *err, size_t len)
{
    const char *prefix = str(rt, "prefix"), *afi = str(rt, "afi-safi"), *src = str(rt, "src-prefix");
    struct json_object *paths = get(rt, "path-list"), *path, *nhs;
    const char *type, *gw, *ifname;

    if (!prefix || !afi) {
        snprintf(err, len, "FRR route without prefix or afi-safi");
        return -1;
    }
    if (strcmp(afi, "frr-routing:ipv4-unicast"))
        return 0; /* not IPv4: not ours, leave it alone */
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
    if (json_object_get_int(get(path, "table-id")) != 0 || json_object_get_int(get(path, "distance")) != 1) {
        snprintf(err, len, "route %s: only table 0 and distance 1 are supported", prefix);
        return -1;
    }
    nhs = get(get(path, "frr-nexthops"), "nexthop");
    if (!nhs || json_object_array_length(nhs) != 1) {
        snprintf(err, len, "route %s: %zu next hops, only one is supported", prefix,
                 nhs ? json_object_array_length(nhs) : 0);
        return -1;
    }
    type = str(json_object_array_get_idx(nhs, 0), "nh-type");
    gw = str(json_object_array_get_idx(nhs, 0), "gateway");
    ifname = str(json_object_array_get_idx(nhs, 0), "interface");
    /* FRR 10.3 prints an unset interface as the text "(null)" */
    if (ifname && *ifname && strcmp(ifname, "(null)")) {
        snprintf(err, len, "route %s: next hop interface %s is not supported", prefix, ifname);
        return -1;
    }
    if (type && !strcmp(type, "blackhole"))
        return lsw_routes_set(out, prefix, "", err, len);
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
    protos = get(get(get(root, "frr-routing:routing"), "control-plane-protocols"), "control-plane-protocol");
    rc = 0;
    for (size_t i = 0; protos && i < json_object_array_length(protos); i++) {
        struct json_object *p = json_object_array_get_idx(protos, i), *routes;
        const char *type = str(p, "type"), *vrf = str(p, "vrf");
        if (!type || strcmp(type, "frr-staticd:staticd"))
            continue;
        routes = get(get(p, "frr-staticd:staticd"), "route-list");
        for (size_t j = 0; routes && j < json_object_array_length(routes); j++) {
            struct json_object *rt = json_object_array_get_idx(routes, j);
            if (vrf && strcmp(vrf, "default")) {
                snprintf(err, len, "static route %s in VRF %s is not supported", str(rt, "prefix"), vrf);
                rc = -1;
                goto end;
            }
            if ((rc = one_route(rt, out, err, len)))
                goto end;
        }
    }
end:
    json_object_put(root);
    json_tokener_free(tok);
    return rc;
}
