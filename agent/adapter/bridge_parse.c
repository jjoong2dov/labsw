/* bridge_parse.c
 * SPDX-License-Identifier: MIT */
#include "bridge_parse.h"

#include <stdio.h>
#include <string.h>

#include <json-c/json.h>

static struct json_object *parse_array(const char *text, const char *what, char *err, size_t len)
{
    struct json_object *j = text ? json_tokener_parse(text) : NULL;

    if (!j || !json_object_is_type(j, json_type_array)) {
        snprintf(err, len, "%s: not a JSON array", what);
        json_object_put(j);
        return NULL;
    }
    return j;
}

int lsw_bridge_parse(const char *link_json, const char *vlan_json, const char *bridge, struct lsw_state *out,
                     char *err, size_t len)
{
    struct json_object *jl, *jv = NULL;
    int rc = -1;

    lsw_state_clear(out);
    if (!(jl = parse_array(link_json, "ip -j link", err, len)))
        return -1;
    if (!(jv = parse_array(vlan_json, "bridge -j vlan", err, len)))
        goto end;
    for (size_t i = 0; i < json_object_array_length(jl); i++) {
        struct json_object *o = json_object_array_get_idx(jl, i), *n;
        const char *name;
        if (!json_object_object_get_ex(o, "ifname", &n))
            continue;
        name = json_object_get_string(n);
        if (!lsw_state_port(out, name)) {
            snprintf(err, len, "port %s: name not supported or more than %d ports", name, LSW_MAX_PORTS);
            goto end;
        }
    }
    for (size_t i = 0; i < json_object_array_length(jv); i++) {
        struct json_object *o = json_object_array_get_idx(jv, i), *n, *vl;
        struct lsw_port *p;
        if (!json_object_object_get_ex(o, "ifname", &n) || !strcmp(json_object_get_string(n), bridge))
            continue;
        p = (struct lsw_port *)lsw_state_find(out, json_object_get_string(n));
        if (!p || !json_object_object_get_ex(o, "vlans", &vl))
            continue; /* a port of another bridge */
        for (size_t k = 0; k < json_object_array_length(vl); k++) {
            struct json_object *v = json_object_array_get_idx(vl, k), *id, *end, *fl;
            int lo, hi;
            uint8_t f = LSW_F_MEMBER;
            if (!json_object_object_get_ex(v, "vlan", &id))
                continue;
            lo = hi = json_object_get_int(id);
            if (json_object_object_get_ex(v, "vlanEnd", &end)) /* a range "10-20" */
                hi = json_object_get_int(end);
            if (lo < 1 || hi > LSW_VID_MAX || lo > hi) {
                snprintf(err, len, "port %s: bad VLAN range %d-%d", p->name, lo, hi);
                goto end;
            }
            if (json_object_object_get_ex(v, "flags", &fl)) {
                for (size_t m = 0; m < json_object_array_length(fl); m++) {
                    const char *s = json_object_get_string(json_object_array_get_idx(fl, m));
                    if (!strcmp(s, "PVID"))
                        f |= LSW_F_PVID;
                    else if (!strcmp(s, "Egress Untagged"))
                        f |= LSW_F_UNTAGGED;
                }
            }
            for (int vid = lo; vid <= hi; vid++)
                p->vid[vid] = f;
        }
    }
    rc = 0;
end:
    json_object_put(jl);
    json_object_put(jv);
    return rc;
}
