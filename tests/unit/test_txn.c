/* test_txn.c - a failure at any command leaves the device as it was */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include <unistd.h>

#include "adapter/adapter.h"
#include "core/plan.h"
#include "core/txn.h"
#include "helpers.h"

static const char *START = "{\"vlan_filtering\": true, \"ports\": {"
                           "\"p1\": [{\"vid\": 1, \"pvid\": true, \"untagged\": true}],"
                           "\"p2\": [{\"vid\": 1, \"pvid\": true, \"untagged\": true}],"
                           "\"p3\": [{\"vid\": 10, \"pvid\": true, \"untagged\": true}, {\"vid\": 20}],"
                           "\"p4\": []}}";
static const char *WANT = "p1=10PU p2=10PU,20 p3=20PU p4=10,20";

struct ctx {
    char path[64];
    struct lsw_adapter *a;
    struct lsw_state want, snap, now;
    struct lsw_txn t;
    /* wrapper: fail every command of a restore after the first fail_restore_after ones */
    int (*real_set)(struct lsw_adapter *, const char *, int, uint8_t, char *, size_t);
};

static struct ctx *C;
static int restore_cmds, fail_restore_after = -1;

static int flaky_set(struct lsw_adapter *a, const char *port, int vid, uint8_t flags, char *err, size_t len)
{
    if (C->t.restoring && fail_restore_after >= 0 && restore_cmds++ >= fail_restore_after) {
        snprintf(err, len, "port %s disappeared", port);
        return -1;
    }
    return C->real_set(a, port, vid, flags, err, len);
}

static int setup(void **st)
{
    struct ctx *c = calloc(1, sizeof(*c));
    FILE *f;
    strcpy(c->path, "/tmp/labsw-txn-XXXXXX");
    close(mkstemp(c->path));
    f = fopen(c->path, "w");
    fputs(START, f);
    fclose(f);
    c->a = lsw_mock_adapter(c->path, NULL);
    c->real_set = c->a->set;
    c->a->set = flaky_set;
    lsw_state_init(&c->want);
    lsw_state_init(&c->snap);
    lsw_state_init(&c->now);
    state_from_text(&c->want, WANT);
    c->t.a = c->a;
    restore_cmds = 0;
    fail_restore_after = -1;
    C = c;
    *st = c;
    return 0;
}

static int teardown(void **st)
{
    struct ctx *c = *st;
    c->a->destroy(c->a);
    unlink(c->path);
    lsw_state_free(&c->want);
    lsw_state_free(&c->snap);
    lsw_state_free(&c->now);
    free(c);
    return 0;
}

static void apply_ok(void **st)
{
    struct ctx *c = *st;
    char err[512], why[160];
    assert_int_equal(lsw_txn_apply(&c->t, &c->want, &c->snap, err, sizeof(err)), LSW_TXN_OK);
    c->a->read(c->a, &c->now, err, sizeof(err));
    assert_int_equal(lsw_state_diff(&c->now, &c->want, why, sizeof(why)), 0);
}

static void failure_at_every_command_restores(void **st)
{
    struct ctx *c = *st;
    char err[512], why[160];
    struct lsw_state start;
    struct lsw_plan plan;

    lsw_state_init(&start);
    lsw_plan_init(&plan);
    c->a->read(c->a, &start, err, sizeof(err));
    assert_int_equal(lsw_plan_build(&start, &c->want, &plan, err, sizeof(err)), 0);
    assert_true(plan.n > 5);
    for (int n = 1; n <= plan.n; n++) {
        c->t.fail_at = n;
        assert_int_equal(lsw_txn_apply(&c->t, &c->want, &c->snap, err, sizeof(err)), LSW_TXN_FAILED);
        c->a->read(c->a, &c->now, err, sizeof(err));
        if (lsw_state_diff(&c->now, &start, why, sizeof(why)))
            fail_msg("fail_at=%d: %s", n, why);
    }
    lsw_state_free(&start);
    lsw_plan_free(&plan);
}

static void restore_that_fails_is_reported_degraded(void **st)
{
    struct ctx *c = *st;
    char err[512];
    c->t.fail_at = 3;
    fail_restore_after = 0;
    assert_int_equal(lsw_txn_apply(&c->t, &c->want, &c->snap, err, sizeof(err)), LSW_TXN_FAILED_DEGRADED);
    assert_non_null(strstr(err, "restore failed"));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(apply_ok, setup, teardown),
        cmocka_unit_test_setup_teardown(failure_at_every_command_restores, setup, teardown),
        cmocka_unit_test_setup_teardown(restore_that_fails_is_reported_degraded, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
