/* bridge.c - Linux bridge through iproute2 (`ip -j`, `bridge -j`)
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>

#include "adapter.h"
#include "bridge_parse.h"
#include "exec.h"

#ifndef LSW_BRIDGE_BIN
#define LSW_BRIDGE_BIN "/usr/sbin/bridge"
#endif
#ifndef LSW_IP_BIN
#define LSW_IP_BIN "/usr/sbin/ip"
#endif
#define TIMEOUT_MS 5000

struct br {
    char name[LSW_NAME_MAX];
};

/* run argv; on failure write "<what>: <stderr or reason>" to err */
static int run(char *const argv[], struct lsw_exec_result *r, char *err, size_t len)
{
    if (lsw_exec(argv, TIMEOUT_MS, r)) {
        snprintf(err, len, "cannot run %s", argv[0]);
        return -1;
    }
    if (r->timedout || r->toolong || r->status != 0) {
        char *e = r->err ? r->err : "";
        size_t n = strlen(e);
        while (n && (e[n - 1] == '\n' || e[n - 1] == ' '))
            e[--n] = '\0';
        snprintf(err, len, "%s %s %s: %s", argv[1], argv[2], argv[3],
                 r->timedout ? "timed out" : r->toolong ? "output too long" : n ? e : "failed");
        lsw_exec_free(r);
        return -1;
    }
    return 0;
}

static int br_check(struct lsw_adapter *a, char *err, size_t len)
{
    struct br *b = a->priv;
    char path[128], v = 0;
    FILE *f;

    snprintf(path, sizeof(path), "/sys/class/net/%s/bridge/vlan_filtering", b->name);
    f = fopen(path, "r");
    if (!f) {
        snprintf(err, len, "bridge %s does not exist", b->name);
        return -1;
    }
    if (fread(&v, 1, 1, f) != 1)
        v = 0;
    fclose(f);
    if (v != '1') {
        snprintf(err, len, "vlan_filtering is off on %s (ip link set %s type bridge vlan_filtering 1)", b->name, b->name);
        return -1;
    }
    return 0;
}

static int br_read(struct lsw_adapter *a, struct lsw_state *out, char *err, size_t len)
{
    struct br *b = a->priv;
    char *link_argv[] = {LSW_IP_BIN, "-j", "link", "show", "master", b->name, NULL};
    char *vlan_argv[] = {LSW_BRIDGE_BIN, "-j", "vlan", "show", NULL};
    struct lsw_exec_result rl, rv;
    int rc;

    if (run(link_argv, &rl, err, len))
        return -1;
    if (run(vlan_argv, &rv, err, len)) {
        lsw_exec_free(&rl);
        return -1;
    }
    rc = lsw_bridge_parse(rl.out, rv.out, b->name, out, err, len);
    lsw_exec_free(&rl);
    lsw_exec_free(&rv);
    return rc;
}

static int br_set(struct lsw_adapter *a, const char *port, int vid, uint8_t flags, char *err, size_t len)
{
    char vs[8], dev[LSW_NAME_MAX];
    char *argv[10] = {LSW_BRIDGE_BIN, "vlan", "add", "dev", dev, "vid", vs};
    int n = 7;
    struct lsw_exec_result r;
    (void)a;

    snprintf(dev, sizeof(dev), "%s", port);
    snprintf(vs, sizeof(vs), "%d", vid);
    if (flags & LSW_F_PVID)
        argv[n++] = "pvid";
    if (flags & LSW_F_UNTAGGED)
        argv[n++] = "untagged";
    argv[n] = NULL;
    if (run(argv, &r, err, len))
        return -1;
    lsw_exec_free(&r);
    return 0;
}

static int br_del(struct lsw_adapter *a, const char *port, int vid, char *err, size_t len)
{
    char vs[8], dev[LSW_NAME_MAX];
    char *argv[] = {LSW_BRIDGE_BIN, "vlan", "del", "dev", dev, "vid", vs, NULL};
    struct lsw_exec_result r;
    (void)a;

    snprintf(dev, sizeof(dev), "%s", port);
    snprintf(vs, sizeof(vs), "%d", vid);
    if (run(argv, &r, err, len))
        return -1;
    lsw_exec_free(&r);
    return 0;
}

static void br_destroy(struct lsw_adapter *a)
{
    free(a->priv);
    free(a);
}

struct lsw_adapter *lsw_bridge_adapter(const char *bridge)
{
    struct lsw_adapter *a;
    struct br *b;

    if (!lsw_valid_port_name(bridge))
        return NULL;
    a = calloc(1, sizeof(*a));
    b = calloc(1, sizeof(*b));
    if (!a || !b) {
        free(a);
        free(b);
        return NULL;
    }
    snprintf(b->name, sizeof(b->name), "%s", bridge);
    a->name = "bridge";
    a->check = br_check;
    a->read = br_read;
    a->set = br_set;
    a->del = br_del;
    a->destroy = br_destroy;
    a->priv = b;
    return a;
}
