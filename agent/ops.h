/* ops.h - labsw-ops RPCs (backup, restore)
 * SPDX-License-Identifier: MIT */
#ifndef LSW_OPS_H
#define LSW_OPS_H

#include <sysrepo.h>

/* subscribe the RPCs on a new subscription context *sub (own thread); backups are
 * kept in dir (made with the first backup); fail_after > 0 (tests) makes a backup write
 * fail after that many bytes */
int ops_init(sr_conn_ctx_t *conn, const char *dir, long fail_after, void (*log)(const char *msg),
             sr_subscription_ctx_t **sub);
void ops_cleanup(void);

#endif
