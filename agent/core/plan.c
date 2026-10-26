/* plan.c
 * SPDX-License-Identifier: MIT */
#include "plan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int lsw_plan_init(struct lsw_plan *p)
{
    p->n = 0;
    p->step = calloc(LSW_MAX_STEPS, sizeof(*p->step));
    return p->step ? 0 : -1;
}

void lsw_plan_free(struct lsw_plan *p)
{
    free(p->step);
    p->step = NULL;
    p->n = 0;
}

static int push(struct lsw_plan *p, enum lsw_op op, const char *port, int vid, uint8_t flags, uint8_t old)
{
    struct lsw_step *s;

    if (p->n >= LSW_MAX_STEPS)
        return -1;
    s = &p->step[p->n++];
    s->op = op;
    snprintf(s->port, sizeof(s->port), "%s", port);
    s->vid = vid;
    s->flags = flags;
    s->old = old;
    return 0;
}

int lsw_plan_build(const struct lsw_state *cur, const struct lsw_state *want, struct lsw_plan *out,
                   char *err, size_t errlen)
{
    static const struct lsw_port empty;

    out->n = 0;
    for (int i = 0; i < want->nports; i++) {
        if (!lsw_state_find(cur, want->port[i].name)) {
            snprintf(err, errlen, "port %s is not a port of the bridge", want->port[i].name);
            return -1;
        }
    }

    /* phase 1 and 2 run twice: first for steps that give a PVID, then the rest */
    for (int phase = 1; phase <= 3; phase++) {
        for (int pvid_pass = 0; pvid_pass < (phase == 3 ? 1 : 2); pvid_pass++) {
            for (int i = 0; i < cur->nports; i++) {
                const struct lsw_port *c = &cur->port[i];
                const struct lsw_port *w = lsw_state_find(want, c->name);
                if (!w)
                    w = &empty;
                for (int v = 1; v <= LSW_VID_MAX; v++) {
                    uint8_t cf = c->vid[v], wf = w->vid[v];
                    int gives_pvid = (wf & LSW_F_PVID) != 0;
                    int r = 0;

                    if (cf == wf)
                        continue;
                    if (phase == 1 && !cf && wf && gives_pvid == !pvid_pass)
                        r = push(out, LSW_OP_SET, c->name, v, wf, cf);
                    else if (phase == 2 && cf && wf && gives_pvid == !pvid_pass)
                        r = push(out, LSW_OP_SET, c->name, v, wf, cf);
                    else if (phase == 3 && cf && !wf)
                        r = push(out, LSW_OP_DEL, c->name, v, 0, cf);
                    if (r) {
                        snprintf(err, errlen, "too many steps (more than %d)", LSW_MAX_STEPS);
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}

void lsw_step_text(const struct lsw_step *s, char *buf, size_t len)
{
    if (s->op == LSW_OP_DEL)
        snprintf(buf, len, "del %s vid %d", s->port, s->vid);
    else
        snprintf(buf, len, "set %s vid %d%s%s", s->port, s->vid, s->flags & LSW_F_PVID ? " pvid" : "",
                 s->flags & LSW_F_UNTAGGED ? " untagged" : "");
}
