/* test_bridge_parse.c - parse saved outputs of ip/bridge */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include "adapter/bridge_parse.h"
#include "helpers.h"

#ifndef FIXTURES
#define FIXTURES "tests/fixtures"
#endif

static char *slurp(const char *name)
{
    char path[256];
    FILE *f;
    long n;
    char *buf;
    snprintf(path, sizeof(path), "%s/%s", FIXTURES, name);
    f = fopen(path, "r");
    assert_non_null(f);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = calloc(1, n + 1);
    assert_int_equal(fread(buf, 1, n, f), (size_t)n);
    fclose(f);
    return buf;
}

static const char *port(struct lsw_state *s, const char *name)
{
    static char buf[512];
    const struct lsw_port *p = lsw_state_find(s, name);
    if (!p)
        return "(none)";
    lsw_port_text(p, buf, sizeof(buf));
    return buf;
}

static void real_output(void **st)
{
    struct lsw_state s;
    char err[128], *link = slurp("ip_link_master_br0.json"), *vlan = slurp("bridge_vlan_show.json");
    (void)st;
    lsw_state_init(&s);
    assert_int_equal(lsw_bridge_parse(link, vlan, "br0", &s, err, sizeof(err)), 0);
    assert_int_equal(s.nports, 4);
    assert_string_equal(port(&s, "p1"), "10PU");
    assert_string_equal(port(&s, "p3"), "1PU 30");
    assert_string_equal(port(&s, "p4"), "10U 100 101 102 103 104 105");
    assert_string_equal(port(&s, "br0"), "(none)"); /* the bridge itself is not a port */
    lsw_state_free(&s);
    free(link);
    free(vlan);
}

static void vlan_range(void **st)
{
    struct lsw_state s;
    char err[128], *link = slurp("ip_link_master_br0.json"), *vlan = slurp("synthetic_vlan_range.json");
    (void)st;
    lsw_state_init(&s);
    assert_int_equal(lsw_bridge_parse(link, vlan, "br0", &s, err, sizeof(err)), 0);
    assert_string_equal(port(&s, "p4"), "100 101 102 103 104 105");
    assert_string_equal(port(&s, "p1"), ""); /* a port without VLANs is still a port */
    lsw_state_free(&s);
    free(link);
    free(vlan);
}

static void truncated_json_is_an_error(void **st)
{
    struct lsw_state s;
    char err[128], *link = slurp("ip_link_master_br0.json"), *vlan = slurp("bridge_vlan_show.json");
    (void)st;
    vlan[strlen(vlan) / 2] = '\0';
    lsw_state_init(&s);
    assert_int_equal(lsw_bridge_parse(link, vlan, "br0", &s, err, sizeof(err)), -1);
    assert_string_equal(err, "bridge -j vlan: not a JSON array");
    lsw_state_free(&s);
    free(link);
    free(vlan);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(real_output),
        cmocka_unit_test(vlan_range),
        cmocka_unit_test(truncated_json_is_an_error),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
