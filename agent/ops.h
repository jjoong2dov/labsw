/* ops.h - the labsw-ops part of labswd (backup and restore RPCs)
 * SPDX-License-Identifier: MIT */
#ifndef LSW_OPS_H
#define LSW_OPS_H

#include <sysrepo.h>

#define OPS_MODULE "labsw-ops"

/* subscribe backup and restore in a subscription of their own: restore edits
 * running, and the change callbacks must run in another thread meanwhile.
 * fail_after > 0 (tests): backup writes fail after that many bytes. */
int ops_start(sr_conn_ctx_t *conn, const char *dir, long fail_after, void (*log)(const char *msg));
void ops_stop(void);

#endif
