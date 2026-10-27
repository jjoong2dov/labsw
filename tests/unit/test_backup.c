/* test_backup.c - backup names, atomic replace under injected failure, read, sweep
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "core/backup.h"

static char dir[64];

static int setup(void **st)
{
    (void)st;
    snprintf(dir, sizeof(dir), "/tmp/lsw-backup-XXXXXX");
    return mkdtemp(dir) ? 0 : -1;
}

static int teardown(void **st)
{
    char cmd[128];
    (void)st;
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    return system(cmd) == 0 ? 0 : -1;
}

/* names in dir, sorted, joined by ' ' */
static void listing(const char *d, char *out, size_t len)
{
    struct dirent **ents;
    int n = scandir(d, &ents, NULL, alphasort);

    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (strcmp(ents[i]->d_name, ".") && strcmp(ents[i]->d_name, ".."))
            snprintf(out + strlen(out), len - strlen(out), "%s%s", out[0] ? " " : "", ents[i]->d_name);
        free(ents[i]);
    }
    free(ents);
}

static void names(void **st)
{
    (void)st;
    assert_true(lsw_backup_name_ok("a"));
    assert_true(lsw_backup_name_ok("before-upgrade_2026.10"));
    assert_true(lsw_backup_name_ok("12345678901234567890123456789012"));
    assert_false(lsw_backup_name_ok("123456789012345678901234567890123"));
    assert_false(lsw_backup_name_ok(""));
    assert_false(lsw_backup_name_ok(NULL));
    assert_false(lsw_backup_name_ok("."));
    assert_false(lsw_backup_name_ok(".."));
    assert_false(lsw_backup_name_ok("../x"));
    assert_false(lsw_backup_name_ok("a/b"));
    assert_false(lsw_backup_name_ok(".hidden"));
    assert_false(lsw_backup_name_ok("a b"));
}

static void write_then_read(void **st)
{
    char err[256], *buf, ls[256];
    size_t len;
    (void)st;
    assert_int_equal(lsw_backup_write(dir, "a", "{\"x\":1}", 7, 0, err, sizeof(err)), 0);
    assert_int_equal(lsw_backup_read(dir, "a", &buf, &len, err, sizeof(err)), 0);
    assert_int_equal(len, 7);
    assert_string_equal(buf, "{\"x\":1}");
    free(buf);
    listing(dir, ls, sizeof(ls));
    assert_string_equal(ls, "a.json");
}

static void failed_write_keeps_old_file(void **st)
{
    char err[256], *buf, ls[256], big[4096];
    size_t len;
    (void)st;
    memset(big, 'n', sizeof(big));
    assert_int_equal(lsw_backup_write(dir, "a", "old", 3, 0, err, sizeof(err)), 0);
    for (long at = 1; at < (long)sizeof(big); at *= 3) {
        assert_int_equal(lsw_backup_write(dir, "a", big, sizeof(big), at, err, sizeof(err)), -1);
        assert_non_null(strstr(err, "injected failure"));
        assert_int_equal(lsw_backup_read(dir, "a", &buf, &len, err, sizeof(err)), 0);
        assert_string_equal(buf, "old");
        free(buf);
        listing(dir, ls, sizeof(ls));
        assert_string_equal(ls, "a.json"); /* no temp file left */
    }
    /* a limit above the size does not fire */
    assert_int_equal(lsw_backup_write(dir, "a", big, sizeof(big), sizeof(big) + 1, err, sizeof(err)), 0);
}

static void makes_missing_dir(void **st)
{
    char sub[128], err[256];
    (void)st;
    snprintf(sub, sizeof(sub), "%s/x/y", dir);
    assert_int_equal(lsw_backup_write(sub, "a", "{}", 2, 0, err, sizeof(err)), 0);
    assert_int_equal(access(sub, F_OK), 0);
}

static void read_errors(void **st)
{
    char err[256], *buf, path[128];
    size_t len;
    (void)st;
    assert_int_equal(lsw_backup_read(dir, "none", &buf, &len, err, sizeof(err)), -1);
    assert_string_equal(err, "no backup named \"none\"");
    assert_int_equal(lsw_backup_read(dir, "../x", &buf, &len, err, sizeof(err)), -1);
    /* a symlink is not followed */
    snprintf(path, sizeof(path), "%s/l.json", dir);
    assert_int_equal(symlink("/etc/hostname", path), 0);
    assert_int_equal(lsw_backup_read(dir, "l", &buf, &len, err, sizeof(err)), -1);
    assert_null(buf);
}

static void sweep_removes_only_temp_files(void **st)
{
    char err[256], ls[256], path[128];
    FILE *f;
    (void)st;
    assert_int_equal(lsw_backup_write(dir, "a", "{}", 2, 0, err, sizeof(err)), 0);
    snprintf(path, sizeof(path), "%s/.tmp-a.AbC123", dir); /* as a crash leaves it */
    assert_non_null(f = fopen(path, "w"));
    fclose(f);
    assert_int_equal(lsw_backup_sweep(dir), 1);
    listing(dir, ls, sizeof(ls));
    assert_string_equal(ls, "a.json");
    assert_int_equal(lsw_backup_sweep("/nonexistent/dir"), 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(names),
        cmocka_unit_test_setup_teardown(write_then_read, setup, teardown),
        cmocka_unit_test_setup_teardown(failed_write_keeps_old_file, setup, teardown),
        cmocka_unit_test_setup_teardown(makes_missing_dir, setup, teardown),
        cmocka_unit_test_setup_teardown(read_errors, setup, teardown),
        cmocka_unit_test_setup_teardown(sweep_removes_only_temp_files, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
