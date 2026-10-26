/* test_plan.c - order of the steps from cur to want */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include "core/plan.h"
#include "helpers.h"

struct ctx {
    struct lsw_state cur, want;
    struct lsw_plan plan;
};

static int setup(void **st)
{
    struct ctx *c = calloc(1, sizeof(*c));
    lsw_state_init(&c->cur);
    lsw_state_init(&c->want);
    lsw_plan_init(&c->plan);
    *st = c;
    return 0;
}

static int teardown(void **st)
{
    struct ctx *c = *st;
    lsw_state_free(&c->cur);
    lsw_state_free(&c->want);
    lsw_plan_free(&c->plan);
    free(c);
    return 0;
}

/* plan as "set p1 vid 10 pvid untagged; del p1 vid 1" */
static const char *plan_text(struct ctx *c, const char *cur, const char *want)
{
    static char out[2048];
    static char err[128];
    char step[96];
    size_t off = 0;

    state_from_text(&c->cur, cur);
    state_from_text(&c->want, want);
    if (lsw_plan_build(&c->cur, &c->want, &c->plan, err, sizeof(err)))
        return err;
    out[0] = '\0';
    for (int i = 0; i < c->plan.n; i++) {
        lsw_step_text(&c->plan.step[i], step, sizeof(step));
        off += snprintf(out + off, sizeof(out) - off, "%s%s", i ? "; " : "", step);
    }
    return out;
}

static void same_state_no_steps(void **st)
{
    assert_string_equal(plan_text(*st, "p1=10PU,20 p2=1PU", "p1=10PU,20 p2=1PU"), "");
}

static void access_port_moves_to_new_vlan(void **st)
{
    /* the new PVID VLAN comes first: the kernel moves the PVID by itself, so the port never has none */
    assert_string_equal(plan_text(*st, "p1=1PU", "p1=10PU"), "set p1 vid 10 pvid untagged; del p1 vid 1");
}

static void pvid_moves_between_existing_vlans(void **st)
{
    /* grant the PVID to 20 before taking it from 10 */
    assert_string_equal(plan_text(*st, "p1=10PU,20", "p1=10,20PU"),
                        "set p1 vid 20 pvid untagged; set p1 vid 10");
}

static void port_missing_from_want_loses_all_vlans(void **st)
{
    assert_string_equal(plan_text(*st, "p1=1PU p2=10PU,20", "p1=1PU"), "del p2 vid 10; del p2 vid 20");
}

static void tagged_becomes_untagged(void **st)
{
    assert_string_equal(plan_text(*st, "p4=10,20", "p4=10U,20"), "set p4 vid 10 untagged");
}

static void unknown_port_is_an_error(void **st)
{
    assert_string_equal(plan_text(*st, "p1=1PU", "p9=10PU"), "port p9 is not a port of the bridge");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(same_state_no_steps, setup, teardown),
        cmocka_unit_test_setup_teardown(access_port_moves_to_new_vlan, setup, teardown),
        cmocka_unit_test_setup_teardown(pvid_moves_between_existing_vlans, setup, teardown),
        cmocka_unit_test_setup_teardown(port_missing_from_want_loses_all_vlans, setup, teardown),
        cmocka_unit_test_setup_teardown(tagged_becomes_untagged, setup, teardown),
        cmocka_unit_test_setup_teardown(unknown_port_is_an_error, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
