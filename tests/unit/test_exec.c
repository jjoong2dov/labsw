/* test_exec.c - the command runner: exit code, stderr, timeout, output limit */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include <string.h>
#include <time.h>

#include "adapter/exec.h"

static void exit_code_and_streams(void **st)
{
    char *argv[] = {"/bin/sh", "-c", "echo out; echo err >&2; exit 3", NULL};
    struct lsw_exec_result r;
    (void)st;
    assert_int_equal(lsw_exec(argv, 2000, &r), 0);
    assert_int_equal(r.status, 3);
    assert_string_equal(r.out, "out\n");
    assert_string_equal(r.err, "err\n");
    lsw_exec_free(&r);
}

static void timeout_kills_the_group(void **st)
{
    /* the shell starts a child that also sleeps; both must go */
    char *argv[] = {"/bin/sh", "-c", "sleep 30 & sleep 30", NULL};
    struct lsw_exec_result r;
    struct timespec a, b;
    (void)st;
    clock_gettime(CLOCK_MONOTONIC, &a);
    assert_int_equal(lsw_exec(argv, 300, &r), 0);
    clock_gettime(CLOCK_MONOTONIC, &b);
    assert_int_equal(r.timedout, 1);
    assert_int_equal(r.status, -1);
    assert_true(b.tv_sec - a.tv_sec < 3);
    lsw_exec_free(&r);
}

static void output_limit(void **st)
{
    char *argv[] = {"/bin/sh", "-c", "head -c 2000000 /dev/zero | tr '\\0' x", NULL};
    struct lsw_exec_result r;
    (void)st;
    assert_int_equal(lsw_exec(argv, 5000, &r), 0);
    assert_int_equal(r.toolong, 1);
    assert_int_equal(strlen(r.out), LSW_EXEC_OUT_MAX);
    lsw_exec_free(&r);
}

static void relative_path_refused(void **st)
{
    char *argv[] = {"sh", "-c", "true", NULL};
    struct lsw_exec_result r;
    (void)st;
    assert_int_equal(lsw_exec(argv, 1000, &r), -1);
}

static void fixed_environment(void **st)
{
    char *argv[] = {"/usr/bin/env", NULL};
    struct lsw_exec_result r;
    (void)st;
    assert_int_equal(lsw_exec(argv, 1000, &r), 0);
    assert_string_equal(r.out, "LC_ALL=C\nPATH=/usr/sbin:/usr/bin:/sbin:/bin\n");
    lsw_exec_free(&r);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(exit_code_and_streams),
        cmocka_unit_test(timeout_kills_the_group),
        cmocka_unit_test(output_limit),
        cmocka_unit_test(relative_path_refused),
        cmocka_unit_test(fixed_environment),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
