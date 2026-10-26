/* plan.h - ordered steps that turn the current bridge state into the wanted one
 * SPDX-License-Identifier: MIT */
#ifndef LSW_PLAN_H
#define LSW_PLAN_H

#include "state.h"

enum lsw_op {
    LSW_OP_SET, /* add the membership, or replace its flags */
    LSW_OP_DEL, /* remove the membership */
};

struct lsw_step {
    enum lsw_op op;
    char port[LSW_NAME_MAX];
    uint16_t vid;
    uint8_t flags; /* new flags for LSW_OP_SET (always includes LSW_F_MEMBER) */
    uint8_t old;   /* flags before the step (0 = not a member) */
};

#define LSW_MAX_STEPS 8192

struct lsw_plan {
    int n;
    struct lsw_step *step; /* LSW_MAX_STEPS entries */
};

int lsw_plan_init(struct lsw_plan *p);
void lsw_plan_free(struct lsw_plan *p);

/*
 * Build the steps from cur to want. Ports of cur that are not in want end up
 * without VLANs. A port of want that is not in cur is an error (the caller
 * validates this first).
 *
 * Order, chosen so that a port never loses its PVID in the middle:
 *   1. new memberships, those carrying the PVID first
 *      (the kernel moves the PVID to the new VLAN by itself)
 *   2. flag changes of existing memberships, those that grant the PVID first
 *   3. removals
 * Returns 0, or -1 with a message in err.
 */
int lsw_plan_build(const struct lsw_state *cur, const struct lsw_state *want, struct lsw_plan *out,
                   char *err, size_t errlen);

void lsw_step_text(const struct lsw_step *s, char *buf, size_t len);

#endif
