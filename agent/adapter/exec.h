/* exec.h - run a program with an argv array (never a shell)
 * SPDX-License-Identifier: MIT */
#ifndef LSW_EXEC_H
#define LSW_EXEC_H

#include <stddef.h>

#define LSW_EXEC_OUT_MAX (1024 * 1024) /* per stream */

struct lsw_exec_result {
    int status;   /* exit code, or -1 if killed / not run */
    int timedout; /* 1 if killed after timeout_ms */
    int toolong;  /* 1 if stdout or stderr went over LSW_EXEC_OUT_MAX */
    char *out;    /* stdout, NUL terminated (free with lsw_exec_free) */
    char *err;    /* stderr */
};

/*
 * argv[0] must be an absolute path. The child gets a fixed environment
 * (LC_ALL=C, PATH=/usr/sbin:/usr/bin:/sbin:/bin) and its own process group,
 * which is killed (SIGTERM, then SIGKILL after 500 ms) when timeout_ms passes.
 * Returns 0 if the program ran (look at res->status), -1 if it could not be started.
 */
int lsw_exec(char *const argv[], int timeout_ms, struct lsw_exec_result *res);
void lsw_exec_free(struct lsw_exec_result *res);

#endif
