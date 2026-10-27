/* state.c
 * SPDX-License-Identifier: MIT */
#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int lsw_state_init(struct lsw_state *s)
{
    s->nports = 0;
    s->port = calloc(LSW_MAX_PORTS, sizeof(*s->port));
    return s->port ? 0 : -1;
}

void lsw_state_free(struct lsw_state *s)
{
    free(s->port);
    s->port = NULL;
    s->nports = 0;
}

void lsw_state_clear(struct lsw_state *s)
{
    memset(s->port, 0, sizeof(*s->port) * s->nports);
    s->nports = 0;
}

int lsw_state_copy(struct lsw_state *dst, const struct lsw_state *src)
{
    memcpy(dst->port, src->port, sizeof(*src->port) * src->nports);
    dst->nports = src->nports;
    return 0;
}

int lsw_valid_port_name(const char *name)
{
    size_t n = strlen(name);

    if (n == 0 || n >= LSW_NAME_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
            return 0;
    }
    return 1;
}

const struct lsw_port *lsw_state_find(const struct lsw_state *s, const char *name)
{
    for (int i = 0; i < s->nports; i++)
        if (!strcmp(s->port[i].name, name))
            return &s->port[i];
    return NULL;
}

struct lsw_port *lsw_state_port(struct lsw_state *s, const char *name)
{
    struct lsw_port *p = (struct lsw_port *)lsw_state_find(s, name);

    if (p)
        return p;
    if (!lsw_valid_port_name(name) || s->nports >= LSW_MAX_PORTS)
        return NULL;
    p = &s->port[s->nports++];
    memset(p, 0, sizeof(*p));
    strcpy(p->name, name);
    return p;
}

void lsw_port_text(const struct lsw_port *p, char *buf, size_t len)
{
    size_t off = 0;

    buf[0] = '\0';
    for (int v = 1; v <= LSW_VID_MAX && off < len; v++) {
        uint8_t f = p->vid[v];
        if (!f)
            continue;
        off += snprintf(buf + off, len - off, "%s%d%s%s", off ? " " : "", v,
                        f & LSW_F_PVID ? "P" : "", f & LSW_F_UNTAGGED ? "U" : "");
    }
}

/* first vid where the two ports differ (a missing port has no VLANs), or 0 */
static int diff_vid(const struct lsw_port *a, const struct lsw_port *b)
{
    static const struct lsw_port empty;

    if (!a)
        a = &empty;
    if (!b)
        b = &empty;
    for (int v = 1; v <= LSW_VID_MAX; v++)
        if (a->vid[v] != b->vid[v])
            return v;
    return 0;
}

/* first difference as port name and vid; 0 if equal */
static int first_diff(const struct lsw_state *a, const struct lsw_state *b, const char **name, int *vid)
{
    for (int i = 0; i < a->nports; i++)
        if ((*vid = diff_vid(&a->port[i], lsw_state_find(b, a->port[i].name)))) {
            *name = a->port[i].name;
            return 1;
        }
    for (int i = 0; i < b->nports; i++)
        if (!lsw_state_find(a, b->port[i].name) && (*vid = diff_vid(NULL, &b->port[i]))) {
            *name = b->port[i].name;
            return 1;
        }
    return 0;
}

static uint8_t flags_of(const struct lsw_state *s, const char *name, int vid)
{
    const struct lsw_port *p = lsw_state_find(s, name);
    return p ? p->vid[vid] : 0;
}

int lsw_state_diff(const struct lsw_state *a, const struct lsw_state *b, char *why, size_t len)
{
    const char *name;
    int vid;

    if (!first_diff(a, b, &name, &vid))
        return 0;
    snprintf(why, len, "port %s vid %d: flags 0x%x != 0x%x", name, vid, flags_of(a, name, vid), flags_of(b, name, vid));
    return 1;
}

static const char *flags_text(uint8_t f)
{
    return f & LSW_F_PVID ? (f & LSW_F_UNTAGGED ? "pvid, untagged" : "pvid, tagged")
                          : (f & LSW_F_UNTAGGED ? "untagged" : "tagged");
}

int lsw_state_drift_text(const struct lsw_state *dev, const struct lsw_state *want, char *buf, size_t len)
{
    const char *name;
    uint8_t d, w;
    int vid;

    if (!first_diff(dev, want, &name, &vid))
        return 0;
    d = flags_of(dev, name, vid);
    w = flags_of(want, name, vid);
    if (!w)
        snprintf(buf, len, "port %s: VLAN %d (%s) is on the device but not in running", name, vid, flags_text(d));
    else if (!d)
        snprintf(buf, len, "port %s: VLAN %d (%s) is in running but not on the device", name, vid, flags_text(w));
    else
        snprintf(buf, len, "port %s: VLAN %d is %s on the device but %s in running", name, vid, flags_text(d),
                 flags_text(w));
    return 1;
}
