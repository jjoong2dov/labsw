/* helpers.h - build states from text like "p1=10PU,20 p2=" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/state.h"

/* "p1=10PU,20 p2=" : p1 has VLAN 10 (PVID, untagged) and VLAN 20 (tagged), p2 has none */
static inline void state_from_text(struct lsw_state *s, const char *text)
{
    char buf[1024], *save1 = NULL;

    lsw_state_clear(s);
    snprintf(buf, sizeof(buf), "%s", text);
    for (char *tok = strtok_r(buf, " ", &save1); tok; tok = strtok_r(NULL, " ", &save1)) {
        char *eq = strchr(tok, '='), *save2 = NULL;
        *eq = '\0';
        struct lsw_port *p = lsw_state_port(s, tok);
        for (char *v = strtok_r(eq + 1, ",", &save2); v; v = strtok_r(NULL, ",", &save2)) {
            char *end;
            long vid = strtol(v, &end, 10);
            p->vid[vid] = LSW_F_MEMBER;
            for (; *end; end++)
                p->vid[vid] |= *end == 'P' ? LSW_F_PVID : *end == 'U' ? LSW_F_UNTAGGED : 0;
        }
    }
}
