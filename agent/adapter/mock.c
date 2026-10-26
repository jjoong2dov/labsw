/* mock.c - a fake bridge kept in a JSON file, for tests without root
 *
 * File format:
 *   {"vlan_filtering": true,
 *    "ports": {"p1": [{"vid": 1, "pvid": true, "untagged": true}], "p2": []}}
 *
 * Like the kernel, giving a VLAN the PVID takes the PVID away from the old one.
 * SPDX-License-Identifier: MIT */
#ifdef LSW_TESTING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "adapter.h"

struct mock {
    char *state_file;
    char *trace_file;
};

static void trace(struct mock *m, const char *fmt, const char *port, int vid, uint8_t flags)
{
    FILE *f;

    if (!m->trace_file || !(f = fopen(m->trace_file, "a")))
        return;
    fprintf(f, fmt, port, vid, flags & LSW_F_PVID ? " pvid" : "", flags & LSW_F_UNTAGGED ? " untagged" : "");
    fputc('\n', f);
    fclose(f);
}

static int load(struct mock *m, struct lsw_state *s, int *filtering, char *err, size_t len)
{
    struct json_object *root = json_object_from_file(m->state_file), *ports, *vf;

    lsw_state_clear(s);
    if (!root) {
        snprintf(err, len, "mock: cannot read %s", m->state_file);
        return -1;
    }
    *filtering = json_object_object_get_ex(root, "vlan_filtering", &vf) && json_object_get_boolean(vf);
    if (json_object_object_get_ex(root, "ports", &ports)) {
        json_object_object_foreach(ports, name, list) {
            struct lsw_port *p = lsw_state_port(s, name);
            if (!p) {
                snprintf(err, len, "mock: bad port name %s", name);
                json_object_put(root);
                return -1;
            }
            for (size_t i = 0; i < json_object_array_length(list); i++) {
                struct json_object *e = json_object_array_get_idx(list, i), *o;
                int vid = json_object_object_get_ex(e, "vid", &o) ? json_object_get_int(o) : 0;
                if (vid < 1 || vid > LSW_VID_MAX)
                    continue;
                p->vid[vid] = LSW_F_MEMBER;
                if (json_object_object_get_ex(e, "pvid", &o) && json_object_get_boolean(o))
                    p->vid[vid] |= LSW_F_PVID;
                if (json_object_object_get_ex(e, "untagged", &o) && json_object_get_boolean(o))
                    p->vid[vid] |= LSW_F_UNTAGGED;
            }
        }
    }
    json_object_put(root);
    return 0;
}

static int save(struct mock *m, const struct lsw_state *s, int filtering, char *err, size_t len)
{
    struct json_object *root = json_object_new_object(), *ports = json_object_new_object();
    char tmp[4096];
    int rc;

    json_object_object_add(root, "vlan_filtering", json_object_new_boolean(filtering));
    for (int i = 0; i < s->nports; i++) {
        struct json_object *list = json_object_new_array();
        for (int v = 1; v <= LSW_VID_MAX; v++) {
            uint8_t f = s->port[i].vid[v];
            if (!f)
                continue;
            struct json_object *e = json_object_new_object();
            json_object_object_add(e, "vid", json_object_new_int(v));
            json_object_object_add(e, "pvid", json_object_new_boolean(f & LSW_F_PVID));
            json_object_object_add(e, "untagged", json_object_new_boolean(f & LSW_F_UNTAGGED));
            json_object_array_add(list, e);
        }
        json_object_object_add(ports, s->port[i].name, list);
    }
    json_object_object_add(root, "ports", ports);
    /* write a new file and rename it, so a reader never sees half a file */
    snprintf(tmp, sizeof(tmp), "%s.tmp", m->state_file);
    rc = json_object_to_file_ext(tmp, root, JSON_C_TO_STRING_PRETTY);
    json_object_put(root);
    if (rc || rename(tmp, m->state_file)) {
        snprintf(err, len, "mock: cannot write %s", m->state_file);
        return -1;
    }
    return 0;
}

static int mock_check(struct lsw_adapter *a, char *err, size_t len)
{
    struct lsw_state s;
    int filtering = 0, rc;

    if (lsw_state_init(&s))
        return -1;
    rc = load(a->priv, &s, &filtering, err, len);
    lsw_state_free(&s);
    if (!rc && !filtering) {
        snprintf(err, len, "vlan_filtering is off on the bridge");
        return -1;
    }
    return rc;
}

static int mock_read(struct lsw_adapter *a, struct lsw_state *out, char *err, size_t len)
{
    int filtering;
    return load(a->priv, out, &filtering, err, len);
}

static int change(struct lsw_adapter *a, const char *port, int vid, int del, uint8_t flags, char *err, size_t len)
{
    struct mock *m = a->priv;
    struct lsw_state s;
    struct lsw_port *p;
    int filtering, rc = -1;

    if (lsw_state_init(&s))
        return -1;
    if (load(m, &s, &filtering, err, len))
        goto end;
    p = (struct lsw_port *)lsw_state_find(&s, port);
    if (!p) {
        snprintf(err, len, "mock: no port %s", port);
        goto end;
    }
    if (del) {
        trace(m, "del %s vid %d%s%s", port, vid, 0);
        p->vid[vid] = 0;
    } else {
        trace(m, "set %s vid %d%s%s", port, vid, flags);
        if (flags & LSW_F_PVID) /* only one PVID per port, as in the kernel */
            for (int v = 1; v <= LSW_VID_MAX; v++)
                p->vid[v] &= ~LSW_F_PVID;
        p->vid[vid] = LSW_F_MEMBER | (flags & (LSW_F_PVID | LSW_F_UNTAGGED));
    }
    rc = save(m, &s, filtering, err, len);
end:
    lsw_state_free(&s);
    return rc;
}

static int mock_set(struct lsw_adapter *a, const char *port, int vid, uint8_t flags, char *err, size_t len)
{
    return change(a, port, vid, 0, flags, err, len);
}

static int mock_del(struct lsw_adapter *a, const char *port, int vid, char *err, size_t len)
{
    return change(a, port, vid, 1, 0, err, len);
}

static void mock_destroy(struct lsw_adapter *a)
{
    struct mock *m = a->priv;
    free(m->state_file);
    free(m->trace_file);
    free(m);
    free(a);
}

struct lsw_adapter *lsw_mock_adapter(const char *state_file, const char *trace_file)
{
    struct lsw_adapter *a = calloc(1, sizeof(*a));
    struct mock *m = calloc(1, sizeof(*m));

    if (!a || !m) {
        free(a);
        free(m);
        return NULL;
    }
    m->state_file = strdup(state_file);
    m->trace_file = trace_file ? strdup(trace_file) : NULL;
    a->name = "mock";
    a->check = mock_check;
    a->read = mock_read;
    a->set = mock_set;
    a->del = mock_del;
    a->destroy = mock_destroy;
    a->priv = m;
    return a;
}

#endif /* LSW_TESTING */
