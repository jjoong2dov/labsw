/* state.h - VLAN membership of the ports of one bridge (observed or wanted)
 * SPDX-License-Identifier: MIT */
#ifndef LSW_STATE_H
#define LSW_STATE_H

#include <stddef.h>
#include <stdint.h>

#define LSW_VID_MAX 4094
#define LSW_MAX_PORTS 256
#define LSW_NAME_MAX 16 /* IFNAMSIZ, including the terminating NUL */

/* flags of one (port, vid) membership; 0 means "not a member" */
#define LSW_F_MEMBER 0x1
#define LSW_F_PVID 0x2
#define LSW_F_UNTAGGED 0x4

struct lsw_port {
    char name[LSW_NAME_MAX];
    uint8_t vid[LSW_VID_MAX + 1]; /* index 1..4094 */
};

struct lsw_state {
    int nports;
    struct lsw_port *port; /* LSW_MAX_PORTS entries */
};

int lsw_state_init(struct lsw_state *s);
void lsw_state_free(struct lsw_state *s);
void lsw_state_clear(struct lsw_state *s);
int lsw_state_copy(struct lsw_state *dst, const struct lsw_state *src);

/* find a port, or add an empty one; NULL if the name is invalid or the table is full */
struct lsw_port *lsw_state_port(struct lsw_state *s, const char *name);
const struct lsw_port *lsw_state_find(const struct lsw_state *s, const char *name);

/* 1 if a valid Linux interface name we accept (see typedef port-name in the YANG module) */
int lsw_valid_port_name(const char *name);

/* compare: returns 0 if equal, else writes the first difference to why */
int lsw_state_diff(const struct lsw_state *a, const struct lsw_state *b, char *why, size_t len);

/* "10PU 20" style text of one port, for logs and tests */
void lsw_port_text(const struct lsw_port *p, char *buf, size_t len);

#endif
