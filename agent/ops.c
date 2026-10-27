/* ops.c - labsw-ops RPCs: backup and restore of what labswd owns in running
 *
 * backup:  running of the managed modules -> <dir>/<name>.json
 *          written to <dir>/.<name>.tmp, fsync, rename, fsync the directory:
 *          a crash or a full disk leaves the old backup, never half a file
 * restore: <name>.json -> one edit that replaces every managed module in running
 *          (a module missing from the file becomes empty), applied in one
 *          transaction; if a change callback (the agent itself) rejects it,
 *          running and the device stay as they were
 *
 * The RPCs run on their own subscription (own thread): restore waits for the
 * change callbacks of the agent, which run on the other one.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libyang/libyang.h>
#include <sysrepo.h>
#include <sysrepo/error_format.h>

#include "ops.h"

#define OPS_MODULE "labsw-ops"
#define APPLY_TIMEOUT_MS 10000 /* the change callbacks have their own deadline below this */

static const char *const managed[] = {"labsw-switch:switch", "labsw-route:routes"};
#define NMANAGED (sizeof(managed) / sizeof(managed[0]))

static struct {
    char *dir;
    long fail_after; /* test build: a backup write fails after this many bytes */
    void (*log)(const char *msg);
    sr_session_ctx_t *run; /* running, for reading and the restore edit */
} o;

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    o.log(buf);
}

static int fail(sr_session_ctx_t *sess, const char *tag, const char *msg)
{
    logf_("%s", msg);
    sr_session_set_netconf_error(sess, "application", tag, NULL, NULL, msg, 0);
    return SR_ERR_OPERATION_FAILED;
}

static const char *input_name(const struct lyd_node *input)
{
    struct lyd_node *n = NULL;
    lyd_find_path(input, "name", 0, &n);
    return n ? lyd_get_value(n) : NULL;
}

/* mkdir -p with 0700 for every directory it makes */
static int make_dirs(const char *dir)
{
    char p[4096];
    size_t n = strlen(dir);

    if (n == 0 || n >= sizeof(p))
        return -1;
    memcpy(p, dir, n + 1);
    for (char *c = p + 1; *c; c++) {
        if (*c != '/')
            continue;
        *c = '\0';
        if (mkdir(p, 0700) && errno != EEXIST)
            return -1;
        *c = '/';
    }
    return mkdir(p, 0700) && errno != EEXIST ? -1 : 0;
}

/* write all of buf to path through a temporary file; 0 or -1 with err */
static int write_atomic(const char *name, const char *buf, size_t len, char *err, size_t errlen)
{
    char tmp[4096], path[4096];
    size_t done = 0, limit = len;
    int fd, dfd, rc = -1;

    snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", o.dir, name);
    snprintf(path, sizeof(path), "%s/%s.json", o.dir, name);
    if (o.fail_after > 0 && (size_t)o.fail_after < len)
        limit = (size_t)o.fail_after;
    if (make_dirs(o.dir)) {
        snprintf(err, errlen, "backup %s: cannot make %.512s: %s", name, o.dir, strerror(errno));
        return -1;
    }
    unlink(tmp); /* left by a crash */
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
        snprintf(err, errlen, "backup %s: cannot create %.512s: %s", name, tmp, strerror(errno));
        return -1;
    }
    while (done < limit) {
        ssize_t n = write(fd, buf + done, limit - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    if (done < len) {
        snprintf(err, errlen, "backup %s: write failed after %zu of %zu bytes: %s", name, done, len,
                 done < limit ? strerror(errno) : "injected failure");
        goto bad;
    }
    if (fsync(fd)) {
        snprintf(err, errlen, "backup %s: fsync: %s", name, strerror(errno));
        goto bad;
    }
    if (close(fd)) {
        fd = -1;
        snprintf(err, errlen, "backup %s: close: %s", name, strerror(errno));
        goto bad;
    }
    fd = -1;
    if (rename(tmp, path)) {
        snprintf(err, errlen, "backup %s: rename: %s", name, strerror(errno));
        goto bad;
    }
    /* the rename itself is on disk only after the directory is synced */
    if ((dfd = open(o.dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return 0;
bad:
    if (fd >= 0)
        close(fd);
    unlink(tmp);
    return rc;
}

static int on_backup(sr_session_ctx_t *sess, uint32_t sub_id, const char *op_path, const struct lyd_node *input,
                     sr_event_t event, uint32_t op_id, struct lyd_node *output, void *priv)
{
    const char *name = input_name(input);
    sr_data_t *data = NULL;
    char *buf = NULL, err[1024], num[24];
    int rc = SR_ERR_OK;
    (void)sub_id; (void)op_path; (void)event; (void)op_id; (void)priv;

    if (!name)
        return fail(sess, "missing-element", "backup: no name");
    /* "/a:x | /b:y": both modules in one read, one consistent snapshot */
    if ((rc = sr_get_data(o.run, "/labsw-switch:switch | /labsw-route:routes", 0, 0, 0, &data))) {
        snprintf(err, sizeof(err), "backup %s: cannot read running: %s", name, sr_strerror(rc));
        return fail(sess, "operation-failed", err);
    }
    if (data && data->tree) {
        if (lyd_print_mem(&buf, data->tree, LYD_JSON, LYD_PRINT_SIBLINGS | LYD_PRINT_WD_EXPLICIT)) {
            snprintf(err, sizeof(err), "backup %s: cannot print running", name);
            rc = fail(sess, "operation-failed", err);
            goto end;
        }
    } else if (!(buf = strdup("{}\n"))) { /* nothing configured is a valid backup too */
        rc = SR_ERR_NO_MEMORY;
        goto end;
    }
    if (write_atomic(name, buf, strlen(buf), err, sizeof(err))) {
        rc = fail(sess, "operation-failed", err);
        goto end;
    }
    snprintf(num, sizeof(num), "%zu", strlen(buf));
    if (lyd_new_path(output, NULL, "bytes", num, LYD_NEW_VAL_OUTPUT, NULL)) {
        rc = SR_ERR_LY;
        goto end;
    }
    logf_("backup %s: %s bytes", name, num);
end:
    free(buf);
    sr_release_data(data);
    return rc;
}

/* "module:name" of a top-level node */
static int node_is(const struct lyd_node *n, const char *mod_node)
{
    char have[128];
    snprintf(have, sizeof(have), "%s:%s", n->schema->module->name, LYD_NAME(n));
    return !strcmp(have, mod_node);
}

static int managed_node(const struct lyd_node *n)
{
    for (size_t i = 0; i < NMANAGED; i++)
        if (node_is(n, managed[i]))
            return 1;
    return 0;
}

static int has_top(const struct lyd_node *tree, const char *mod_node)
{
    const struct lyd_node *n;
    LY_LIST_FOR(tree, n)
        if (node_is(n, mod_node))
            return 1;
    return 0;
}

/* the backup as an edit that replaces every managed container */
static int restore_edit(const struct ly_ctx *ctx, const char *name, struct lyd_node **edit, char *err, size_t len)
{
    char path[4096];
    struct lyd_node *tree = NULL, *n;

    *edit = NULL;
    snprintf(path, sizeof(path), "%s/%s.json", o.dir, name);
    if (access(path, R_OK)) {
        snprintf(err, len, "restore %s: no such backup (%s)", name, strerror(errno));
        return -1;
    }
    /* types and unknown nodes are checked here; must/leafref when sysrepo applies it */
    if (lyd_parse_data_path(ctx, path, LYD_JSON, LYD_PARSE_ONLY | LYD_PARSE_STRICT | LYD_PARSE_NO_STATE, 0, &tree)) {
        const struct ly_err_item *e = ly_err_last(ctx);
        snprintf(err, len, "restore %s: not a valid backup: %s", name, e && e->msg ? e->msg : "parse error");
        return -1;
    }
    LY_LIST_FOR(tree, n) {
        if (!managed_node(n)) {
            snprintf(err, len, "restore %s: %s:%s is not part of a backup", name, n->schema->module->name,
                     LYD_NAME(n));
            lyd_free_all(tree);
            return -1;
        }
    }
    /* a module the file does not have is replaced by an empty container */
    for (size_t i = 0; i < NMANAGED; i++) {
        if (has_top(tree, managed[i]))
            continue;
        snprintf(path, sizeof(path), "/%s", managed[i]);
        if (lyd_new_path(NULL, ctx, path, NULL, 0, &n) || lyd_insert_sibling(tree, n, &tree)) {
            snprintf(err, len, "restore %s: cannot build the edit", name);
            lyd_free_all(tree);
            return -1;
        }
    }
    *edit = lyd_first_sibling(tree);
    return 0;
}

static int on_restore(sr_session_ctx_t *sess, uint32_t sub_id, const char *op_path, const struct lyd_node *input,
                      sr_event_t event, uint32_t op_id, struct lyd_node *output, void *priv)
{
    const char *name = input_name(input);
    const struct ly_ctx *ctx;
    const sr_error_info_t *ei = NULL;
    struct lyd_node *edit = NULL;
    char err[1024];
    int rc;
    (void)sub_id; (void)op_path; (void)event; (void)op_id; (void)output; (void)priv;

    if (!name)
        return fail(sess, "missing-element", "restore: no name");
    ctx = sr_session_acquire_context(o.run);
    if (restore_edit(ctx, name, &edit, err, sizeof(err))) {
        sr_session_release_context(o.run);
        return fail(sess, "invalid-value", err);
    }
    logf_("restore %s: replacing running", name);
    rc = sr_edit_batch(o.run, edit, "replace");
    if (!rc)
        rc = sr_apply_changes(o.run, APPLY_TIMEOUT_MS);
    lyd_free_all(edit);
    if (rc) {
        sr_session_get_error(o.run, &ei);
        snprintf(err, sizeof(err), "restore %s: running not changed: %s", name,
                 ei && ei->err_count && ei->err[0].message ? ei->err[0].message : sr_strerror(rc));
        sr_discard_changes(o.run);
        sr_session_release_context(o.run);
        return fail(sess, "operation-failed", err);
    }
    sr_session_release_context(o.run);
    logf_("restore %s: ok", name);
    return SR_ERR_OK;
}

int ops_init(sr_conn_ctx_t *conn, const char *dir, long fail_after, void (*log)(const char *msg),
             sr_subscription_ctx_t **sub)
{
    sr_session_ctx_t *s;
    int rc;

    o.log = log;
    o.fail_after = fail_after;
    if (!(o.dir = strdup(dir)))
        return SR_ERR_NO_MEMORY;
    /* the directory is made when the first backup is written: a bad -B must not
     * stop the agent (no subscriber would mean edits that never reach the device) */
    if ((rc = sr_session_start(conn, SR_DS_RUNNING, &o.run)) || (rc = sr_session_start(conn, SR_DS_RUNNING, &s)))
        return rc;
    /* own subscription context = own thread, see the top of the file */
    if ((rc = sr_rpc_subscribe_tree(s, "/" OPS_MODULE ":backup", on_backup, NULL, 0, 0, sub)) ||
        (rc = sr_rpc_subscribe_tree(s, "/" OPS_MODULE ":restore", on_restore, NULL, 0, 0, sub)))
        return rc;
    return SR_ERR_OK;
}

void ops_cleanup(void)
{
    free(o.dir);
    o.dir = NULL;
}
