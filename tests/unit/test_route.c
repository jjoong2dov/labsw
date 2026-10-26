/* test_route.c - prefixes, the route plan, FRR's JSON, and undoing a failed apply */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "adapter/route_adapter.h"
#include "core/route.h"
#include "core/rtxn.h"

static char *slurp(const char *name)
{
    char path[512];
    FILE *f;
    long n;
    char *buf;

    snprintf(path, sizeof(path), "%s/%s", FIXTURES, name);
    f = fopen(path, "r");
    assert_non_null(f);
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = calloc(1, (size_t)n + 1);
    assert_int_equal(fread(buf, 1, (size_t)n, f), (size_t)n);
    fclose(f);
    return buf;
}

static void test_prefix(void **st)
{
    char err[128];
    (void)st;
    assert_int_equal(lsw_route_prefix_ok("198.51.100.0/24", err, sizeof(err)), 0);
    assert_int_equal(lsw_route_prefix_ok("0.0.0.0/0", err, sizeof(err)), 0);
    assert_int_equal(lsw_route_prefix_ok("203.0.113.77/32", err, sizeof(err)), 0);
    /* FRR would store 203.0.113.0/24 without a word */
    assert_int_equal(lsw_route_prefix_ok("203.0.113.77/24", err, sizeof(err)), -1);
    assert_string_equal(err, "prefix 203.0.113.77/24 has host bits set");
    assert_int_equal(lsw_route_prefix_ok("198.51.100.0/33", err, sizeof(err)), -1);
    assert_int_equal(lsw_route_prefix_ok("198.51.100.0", err, sizeof(err)), -1);
    assert_int_equal(lsw_route_prefix_ok("198.51.100.0/", err, sizeof(err)), -1);
    assert_int_equal(lsw_route_prefix_ok("198.51.100.0/24x", err, sizeof(err)), -1);
}

static void routes(struct lsw_routes *s, const char *text)
{
    char buf[512], *save = NULL, err[128];

    lsw_routes_clear(s);
    snprintf(buf, sizeof(buf), "%s", text);
    for (char *tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(tok, '=');
        *eq = '\0';
        assert_int_equal(lsw_routes_set(s, tok, strcmp(eq + 1, "bh") ? eq + 1 : "", err, sizeof(err)), 0);
    }
}

static void plan_text(const struct lsw_route_plan *p, char *out, size_t len)
{
    out[0] = '\0';
    for (int i = 0; i < p->n; i++) {
        char t[96];
        lsw_route_step_text(&p->s[i], t, sizeof(t));
        snprintf(out + strlen(out), len - strlen(out), "%s%s", i ? "; " : "", t);
    }
}

static void test_plan(void **st)
{
    static struct lsw_routes now, want;
    static struct lsw_route_plan p;
    char txt[512];
    (void)st;

    routes(&now, "198.51.100.0/24=203.0.113.9 198.51.100.128/25=bh 192.0.2.0/24=bh");
    routes(&want, "198.51.100.0/24=203.0.113.10 198.51.100.128/25=bh 203.0.113.0/24=bh");
    lsw_route_plan(&now, &want, &p);
    plan_text(&p, txt, sizeof(txt));
    /* new next hop before the old one goes; new prefixes before old ones go */
    assert_string_equal(txt, "add route 198.51.100.0/24 203.0.113.10; add route 203.0.113.0/24 blackhole; "
                             "del route 198.51.100.0/24 203.0.113.9; del route 192.0.2.0/24 blackhole");
    lsw_route_plan(&want, &want, &p);
    assert_int_equal(p.n, 0);
}

static void test_parse_real(void **st)
{
    static struct lsw_routes r;
    char err[256], *j;
    (void)st;

    j = slurp("frr_running_two_routes.json");
    assert_int_equal(lsw_frr_parse(j, &r, err, sizeof(err)), 0);
    assert_int_equal(r.n, 2);
    assert_string_equal(lsw_routes_find(&r, "198.51.100.0/24")->gateway, "203.0.113.9");
    assert_string_equal(lsw_routes_find(&r, "198.51.100.128/25")->gateway, "");
    free(j);

    j = slurp("frr_running_empty.json");
    assert_int_equal(lsw_frr_parse(j, &r, err, sizeof(err)), 0);
    assert_int_equal(r.n, 0);
    free(j);

    j = slurp("frr_running_two_nexthops.json");
    assert_int_equal(lsw_frr_parse(j, &r, err, sizeof(err)), -1);
    assert_string_equal(err, "route 198.51.100.0/24: 2 next hops, only one is supported");
    free(j);
}

static void test_parse_bad(void **st)
{
    static struct lsw_routes r;
    char err[256], *j, *cut;
    (void)st;

    j = slurp("frr_running_two_routes.json");
    cut = strdup(j);
    cut[strlen(cut) / 2] = '\0'; /* output cut off in the middle */
    assert_int_equal(lsw_frr_parse(cut, &r, err, sizeof(err)), -1);
    assert_string_equal(err, "FRR output is not one JSON object");
    free(cut);
    assert_int_equal(asprintf(&cut, "%s\n%% Warning: something\n", j) > 0, 1);
    assert_int_equal(lsw_frr_parse(cut, &r, err, sizeof(err)), -1);
    free(cut);
    assert_int_equal(lsw_frr_parse("% Unknown command: show mgmt", &r, err, sizeof(err)), -1);
    assert_int_equal(lsw_frr_parse("[]", &r, err, sizeof(err)), -1);
    free(j);
    /* the same shape, edited: another VRF and an interface next hop are refused, not dropped */
    assert_int_equal(lsw_frr_parse("{\"frr-routing:routing\":{\"control-plane-protocols\":{\"control-plane-protocol\":[{"
                                   "\"type\":\"frr-staticd:staticd\",\"vrf\":\"red\",\"frr-staticd:staticd\":{\"route-list\":[{"
                                   "\"prefix\":\"192.0.2.0/24\",\"afi-safi\":\"frr-routing:ipv4-unicast\"}]}}]}}}",
                                   &r, err, sizeof(err)), -1);
    assert_string_equal(err, "static route 192.0.2.0/24 in VRF red is not supported");
    assert_int_equal(lsw_frr_parse("{\"frr-routing:routing\":{\"control-plane-protocols\":{\"control-plane-protocol\":[{"
                                   "\"type\":\"frr-staticd:staticd\",\"vrf\":\"default\",\"frr-staticd:staticd\":{\"route-list\":[{"
                                   "\"prefix\":\"192.0.2.0/24\",\"afi-safi\":\"frr-routing:ipv4-unicast\",\"path-list\":[{"
                                   "\"table-id\":0,\"distance\":1,\"frr-nexthops\":{\"nexthop\":[{\"nh-type\":\"ip4-ifindex\","
                                   "\"gateway\":\"198.51.100.1\",\"interface\":\"eth0\"}]}}]}]}}]}}}",
                                   &r, err, sizeof(err)), -1);
    assert_string_equal(err, "route 192.0.2.0/24: next hop interface eth0 is not supported");
}

/* rtxn on the mock: fail at each command of a gateway change; the routes come back */
static void test_rtxn_undo(void **st)
{
    static struct lsw_routes want, snap, now;
    char path[64] = "/tmp/labsw-rt-XXXXXX", err[512];
    (void)st;

    for (int fail = 1; fail <= 3; fail++) {
        FILE *f;
        struct lsw_route_adapter *a;
        struct lsw_rtxn t = {0};

        close(mkstemp(path));
        f = fopen(path, "w");
        fputs("{\"routes\": {\"198.51.100.0/24\": [\"203.0.113.9\"], \"192.0.2.0/24\": [\"blackhole\"]}}", f);
        fclose(f);
        a = lsw_mock_route_adapter(path, NULL);
        t.a = a;
        t.fail_at = fail;
        /* 3 commands: add 198.51.100.0/24 .10, del .9, del 192.0.2.0/24 */
        routes(&want, "198.51.100.0/24=203.0.113.10");
        assert_int_equal(lsw_rtxn_apply(&t, &want, &snap, err, sizeof(err)), LSW_TXN_FAILED);
        assert_non_null(strstr(err, "injected failure"));
        assert_int_equal(a->read(a, &now, err, sizeof(err)), 0);
        assert_int_equal(lsw_routes_diff(&now, &snap, err, sizeof(err)), 0);
        assert_int_equal(now.n, 2);
        a->destroy(a);
        unlink(path);
        strcpy(path, "/tmp/labsw-rt-XXXXXX");
    }
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_prefix),
        cmocka_unit_test(test_plan),
        cmocka_unit_test(test_parse_real),
        cmocka_unit_test(test_parse_bad),
        cmocka_unit_test(test_rtxn_undo),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
