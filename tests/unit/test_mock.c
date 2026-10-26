/* test_mock.c - the fake bridge behaves like the kernel where it matters */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include <unistd.h>

#include "adapter/adapter.h"
#include "helpers.h"

static char path[64];

static int setup(void **st)
{
    FILE *f;
    strcpy(path, "/tmp/labsw-mock-XXXXXX");
    close(mkstemp(path));
    f = fopen(path, "w");
    fputs("{\"vlan_filtering\": true, \"ports\": {\"p1\": [{\"vid\": 10, \"pvid\": true, \"untagged\": true}]}}", f);
    fclose(f);
    *st = lsw_mock_adapter(path, NULL);
    return 0;
}

static int teardown(void **st)
{
    struct lsw_adapter *a = *st;
    a->destroy(a);
    unlink(path);
    return 0;
}

static const char *port_text(struct lsw_adapter *a, const char *port)
{
    static char buf[256];
    char err[128];
    struct lsw_state s;
    lsw_state_init(&s);
    assert_int_equal(a->read(a, &s, err, sizeof(err)), 0);
    lsw_port_text(lsw_state_find(&s, port), buf, sizeof(buf));
    lsw_state_free(&s);
    return buf;
}

static void new_pvid_takes_it_from_old_vlan(void **st)
{
    struct lsw_adapter *a = *st;
    char err[128];
    assert_int_equal(a->set(a, "p1", 20, LSW_F_PVID, err, sizeof(err)), 0);
    assert_string_equal(port_text(a, "p1"), "10U 20P");
}

static void set_replaces_flags(void **st)
{
    struct lsw_adapter *a = *st;
    char err[128];
    assert_int_equal(a->set(a, "p1", 10, 0, err, sizeof(err)), 0);
    assert_string_equal(port_text(a, "p1"), "10");
}

static void unknown_port_fails(void **st)
{
    struct lsw_adapter *a = *st;
    char err[128];
    assert_int_not_equal(a->set(a, "p9", 10, 0, err, sizeof(err)), 0);
    assert_string_equal(err, "mock: no port p9");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(new_pvid_takes_it_from_old_vlan, setup, teardown),
        cmocka_unit_test_setup_teardown(set_replaces_flags, setup, teardown),
        cmocka_unit_test_setup_teardown(unknown_port_fails, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
