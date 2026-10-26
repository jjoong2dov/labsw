/* txn.h - apply a wanted state, and put the old one back if anything fails
 * SPDX-License-Identifier: MIT */
#ifndef LSW_TXN_H
#define LSW_TXN_H

#include "../adapter/adapter.h"
#include "state.h"

enum lsw_txn_result {
    LSW_TXN_OK = 0,
    LSW_TXN_FAILED,          /* failed, the previous state was restored and read back */
    LSW_TXN_FAILED_DEGRADED, /* failed, and the previous state could not be restored */
};

struct lsw_txn {
    struct lsw_adapter *a;
    int fail_at;    /* tests: make the n-th device command of an apply fail (0 = off) */
    int cmd_count;  /* device commands in the current apply */
    int restoring;  /* commands of a restore are never made to fail */
    int deadline_ms; /* an apply that runs longer stops and is undone (0 = no limit) */
    long long started_ms;
    void (*log)(const char *msg);
};

/* CLOCK_MONOTONIC in ms */
long long lsw_now_ms(void);

/*
 * Make the device equal to want. The state read before the first command is
 * copied to snapshot (so a later ABORT can restore it).
 * On a failure the device is restored to the snapshot and read back.
 */
enum lsw_txn_result lsw_txn_apply(struct lsw_txn *t, const struct lsw_state *want, struct lsw_state *snapshot,
                                  char *err, size_t errlen);

/* make the device equal to snap again; 0 if it reads back equal */
int lsw_txn_restore(struct lsw_txn *t, const struct lsw_state *snap, char *err, size_t errlen);

#endif
