/* test_state.c - comparing device and running, and the operator's text for it
 * SPDX-License-Identifier: MIT */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include "helpers.h"

static const char *drift(const char *dev, const char *want)
{
    static char buf[256];
    struct lsw_state d, w;
    char why[160];

    lsw_state_init(&d);
    lsw_state_init(&w);
    state_from_text(&d, dev);
    state_from_text(&w, want);
    if (!lsw_state_drift_text(&d, &w, buf, sizeof(buf)))
        snprintf(buf, sizeof(buf), "equal");
    /* both functions must agree on whether there is a difference */
    assert_int_equal(lsw_state_diff(&d, &w, why, sizeof(why)), strcmp(buf, "equal") != 0);
    lsw_state_free(&d);
    lsw_state_free(&w);
    return buf;
}

static void equal_states(void **st)
{
    (void)st;
    assert_string_equal(drift("p1=10PU p2=", "p1=10PU"), "equal");
}

static void vlan_added_on_device(void **st)
{
    (void)st;
    assert_string_equal(drift("p1=10PU p3=99", "p1=10PU p3="),
                        "port p3: VLAN 99 (tagged) is on the device but not in running");
}

static void vlan_removed_from_device(void **st)
{
    (void)st;
    assert_string_equal(drift("p1= p2=20", "p1=10PU p2=20"),
                        "port p1: VLAN 10 (pvid, untagged) is in running but not on the device");
}

static void port_only_in_running(void **st)
{
    (void)st;
    assert_string_equal(drift("p2=", "p1=10U"), "port p1: VLAN 10 (untagged) is in running but not on the device");
}

static void flags_differ(void **st)
{
    (void)st;
    assert_string_equal(drift("p2=20P", "p2=20U"),
                        "port p2: VLAN 20 is pvid, tagged on the device but untagged in running");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(equal_states),
        cmocka_unit_test(vlan_added_on_device),
        cmocka_unit_test(vlan_removed_from_device),
        cmocka_unit_test(port_only_in_running),
        cmocka_unit_test(flags_differ),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
