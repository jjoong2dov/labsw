/* ops.c - labsw-ops: backup running (labsw-switch, labsw-route) to a file, restore it
 *
 * backup   reads both modules in one sr_get_data and writes <dir>/<name>.json
 *          atomically (core/backup.c).
 * restore  parses the file, accepts only the config containers of the two
 *          modules, and replaces both in one edit. The agent's own change
 *          callbacks apply it to the device; if one fails, sysrepo rejects the
 *          whole edit and the callbacks undo the device (as for edit-config).
 *
 * The RPC callbacks call sysrepo, so they never take txn_lock (see main.c).
 * SPDX-License-Identifier: MIT */
#include "ops.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libyang/libyang.h>
#include <sysrepo/error_format.h>

#include "core/backup.h"
#include "routes.h"

#define SWITCH_MODULE "labsw-switch"

static struct {
    const char *dir;
    long fail_after;
    void (*log)(const char *msg);
    sr_session_ctx_t *sess; /* running; used only by the RPC thread */
    sr_subscription_ctx_t *sub;
} o;

static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    o.log(buf);
}

static const char *input_name(const struct lyd_node *input)
{
    struct lyd_node *n = NULL;

    return lyd_find_path(input, "name", 0, &n) ? NULL : lyd_get_value(n);
}

static int fail(sr_session_ctx_t *sess, const char *tag, const char *what, const char *name, const char *err)
{
    char msg[1200];

    snprintf(msg, sizeof(msg), "%s \"%s\": %s", what, name, err);
    logf_("%s", msg);
    sr_session_set_netconf_error(sess, "application", tag, NULL, NULL, msg, 0);
    return SR_ERR_OPERATION_FAILED;
}

static int on_backup(sr_session_ctx_t *sess, uint32_t sub_id, const char *op_path, const struct lyd_node *input,
                     sr_event_t event, uint32_t request_id, struct lyd_node *output, void *priv)
{
    const char *name = input_name(input);
    sr_data_t *data = NULL;
    char *json = NULL, err[1024], num[24];
    int rc;
    (void)sub_id; (void)op_path; (void)event; (void)request_id; (void)priv;

    if (!name || !lsw_backup_name_ok(name))
        return fail(sess, "invalid-value", "backup", name ? name : "", "bad name");
    /* one read: both modules from the same running */
    if ((rc = sr_get_data(o.sess, "/" SWITCH_MODULE ":switch | /" ROUTE_MODULE ":routes", 0, 0, 0, &data)))
        return fail(sess, "operation-failed", "backup", name, sr_strerror(rc));
    if (data && data->tree && lyd_print_mem(&json, data->tree, LYD_JSON, LYD_PRINT_SIBLINGS)) {
        sr_release_data(data);
        return fail(sess, "operation-failed", "backup", name, "cannot print running as JSON");
    }
    sr_release_data(data);
    if (!json && !(json = strdup("{}\n")))
        return SR_ERR_NO_MEMORY;
    if (lsw_backup_write(o.dir, name, json, strlen(json), o.fail_after, err, sizeof(err))) {
        free(json);
        return fail(sess, "operation-failed", "backup", name, err);
    }
    snprintf(num, sizeof(num), "%zu", strlen(json));
    free(json);
    logf_("backup \"%s\": %s bytes", name, num);
    if (lyd_new_term(output, NULL, "bytes", num, LYD_NEW_VAL_OUTPUT, NULL))
        return SR_ERR_LY;
    return SR_ERR_OK;
}

/* the backup as an edit: each module's container with operation replace; an
 * absent one is added empty, so that module is emptied */
static int restore_edit(const struct ly_ctx *ctx, const char *json, struct lyd_node **edit, char *err, size_t len)
{
    static const char *mods[] = {SWITCH_MODULE, ROUTE_MODULE};
    static const char *tops[] = {"switch", "routes"};
    struct lyd_node *tree = NULL, *n, *top;

    *edit = NULL;
    if (!*json) {
        snprintf(err, len, "not a valid backup: empty file");
        return -1;
    }
    if (lyd_parse_data_mem(ctx, json, LYD_JSON, LYD_PARSE_ONLY | LYD_PARSE_STRICT | LYD_PARSE_NO_STATE, 0, &tree)) {
        const struct ly_err_item *e = ly_err_last(ctx);
        snprintf(err, len, "not a valid backup: %s", e && e->msg ? e->msg : "cannot parse");
        return -1;
    }
    LY_LIST_FOR(tree, n) {
        int ok = 0;
        for (int i = 0; i < 2; i++)
            ok |= !strcmp(lyd_owner_module(n)->name, mods[i]) && !strcmp(LYD_NAME(n), tops[i]);
        if (!ok) {
            snprintf(err, len, "not a valid backup: has /%s:%s", lyd_owner_module(n)->name, LYD_NAME(n));
            lyd_free_all(tree);
            return -1;
        }
    }
    for (int i = 0; i < 2; i++) {
        const struct lys_module *mod = ly_ctx_get_module_implemented(ctx, mods[i]);
        top = NULL;
        LY_LIST_FOR(tree, n)
            if (lyd_owner_module(n) == mod)
                top = n;
        if ((!top && (!mod || lyd_new_inner(NULL, mod, tops[i], 0, &top) || lyd_insert_sibling(tree, top, &tree))) ||
            lyd_new_meta(ctx, top, NULL, "ietf-netconf:operation", "replace", 0, NULL)) {
            snprintf(err, len, "cannot build the edit for %s", mods[i]);
            lyd_free_all(tree);
            return -1;
        }
    }
    *edit = tree;
    return 0;
}

/* first error message of the last failed sysrepo call on o.sess */
static void last_error(int rc, char *err, size_t len)
{
    const sr_error_info_t *ei = NULL;

    if (!sr_session_get_error(o.sess, &ei) && ei && ei->err_count && ei->err[0].message)
        snprintf(err, len, "%s", ei->err[0].message);
    else
        snprintf(err, len, "%s", sr_strerror(rc));
}

static int on_restore(sr_session_ctx_t *sess, uint32_t sub_id, const char *op_path, const struct lyd_node *input,
                      sr_event_t event, uint32_t request_id, struct lyd_node *output, void *priv)
{
    const char *name = input_name(input);
    const struct ly_ctx *ctx;
    struct lyd_node *edit = NULL;
    char *json = NULL, err[1024];
    size_t len;
    int rc;
    (void)sub_id; (void)op_path; (void)event; (void)request_id; (void)output; (void)priv;

    if (!name || !lsw_backup_name_ok(name))
        return fail(sess, "invalid-value", "restore", name ? name : "", "bad name");
    if (lsw_backup_read(o.dir, name, &json, &len, err, sizeof(err)))
        return fail(sess, "operation-failed", "restore", name, err);
    ctx = sr_session_acquire_context(o.sess);
    rc = restore_edit(ctx, json, &edit, err, sizeof(err));
    free(json);
    if (rc) {
        sr_session_release_context(o.sess);
        return fail(sess, "operation-failed", "restore", name, err);
    }
    logf_("restore \"%s\": replacing running", name);
    /* one edit, one transaction: every module's subscribers see it together */
    if ((rc = sr_edit_batch(o.sess, edit, "merge")) || (rc = sr_apply_changes(o.sess, 0))) {
        last_error(rc, err, sizeof(err));
        sr_discard_changes(o.sess);
    }
    lyd_free_all(edit);
    sr_session_release_context(o.sess);
    if (rc)
        return fail(sess, "operation-failed", "restore", name, err);
    logf_("restore \"%s\": ok", name);
    return SR_ERR_OK;
}

int ops_start(sr_conn_ctx_t *conn, const char *dir, long fail_after, void (*log)(const char *msg))
{
    int rc, n;

    o.dir = dir;
    o.fail_after = fail_after;
    o.log = log;
    if ((n = lsw_backup_sweep(dir)) > 0)
        logf_("backup: removed %d unfinished file(s) in %s", n, dir);
    if ((rc = sr_session_start(conn, SR_DS_RUNNING, &o.sess)) ||
        (rc = sr_rpc_subscribe_tree(o.sess, "/" OPS_MODULE ":backup", on_backup, NULL, 0, 0, &o.sub)) ||
        (rc = sr_rpc_subscribe_tree(o.sess, "/" OPS_MODULE ":restore", on_restore, NULL, 0, 0, &o.sub))) {
        logf_("%s subscribe: %s", OPS_MODULE, sr_strerror(rc));
        return rc;
    }
    return SR_ERR_OK;
}

void ops_stop(void)
{
    sr_unsubscribe(o.sub);
    o.sub = NULL;
}
