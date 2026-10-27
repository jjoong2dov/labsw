/* labsw-dump.c - print every explicitly set node of a sysrepo datastore, one per line
 *
 *   labsw-dump running|startup
 *
 * Lines are "<path>" (list entry, presence container, leaf-list value) or
 * "<path> = <value>" (leaf), with the path of libyang (module names, keys as
 * predicates). The same configuration gives the same lines in another revision
 * of a module, so two dumps can be compared line by line (labsw-upgrade-check).
 * The repository is the one of SYSREPO_REPOSITORY_PATH / SYSREPO_SHM_PREFIX.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libyang/libyang.h>
#include <sysrepo.h>

static void put_escaped(const char *s)
{
    /* one node per line, also for a string value with a newline */
    for (; *s; s++) {
        if (*s == '\n')
            fputs("\\n", stdout);
        else if (*s == '\\')
            fputs("\\\\", stdout);
        else
            putchar(*s);
    }
}

static void dump_node(const struct lyd_node *n)
{
    const struct lysc_node *s = n->schema;
    char *path;

    if (n->flags & LYD_DEFAULT)
        return;
    if (s) {
        if (s->nodetype == LYS_CONTAINER && lysc_is_np_cont(s))
            return;
        if (s->nodetype == LYS_LEAF && lysc_is_key(s))
            return; /* in the path of its list entry */
    }
    path = lyd_path(n, LYD_PATH_STD, NULL, 0);
    if (!path)
        return;
    put_escaped(path);
    if (s && s->nodetype == LYS_LEAF) {
        fputs(" = ", stdout);
        put_escaped(lyd_get_value(n));
    } else if (s && (s->nodetype & LYD_NODE_ANY)) {
        fputs(" = (anydata)", stdout);
    }
    putchar('\n');
    free(path);
}

static void dump_tree(const struct lyd_node *first)
{
    const struct lyd_node *n;

    for (n = first; n; n = n->next) {
        dump_node(n);
        dump_tree(lyd_child(n));
    }
}

int main(int argc, char **argv)
{
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    sr_data_t *data = NULL;
    sr_datastore_t ds;
    int r;

    if (argc != 2 || (strcmp(argv[1], "running") && strcmp(argv[1], "startup"))) {
        fprintf(stderr, "usage: labsw-dump running|startup\n");
        return 1;
    }
    ds = strcmp(argv[1], "running") ? SR_DS_STARTUP : SR_DS_RUNNING;
    if ((r = sr_connect(SR_CONN_DEFAULT, &conn)) || (r = sr_session_start(conn, ds, &sess)) ||
        (r = sr_get_data(sess, "/*", 0, 10000, 0, &data))) {
        fprintf(stderr, "labsw-dump: %s\n", sr_strerror(r));
        sr_disconnect(conn);
        return 1;
    }
    if (data)
        dump_tree(data->tree);
    sr_release_data(data);
    sr_disconnect(conn);
    return fflush(stdout) ? 1 : 0;
}
