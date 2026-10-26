/* reject_sub.c - test helper: a second subscriber of labsw-switch that rejects
 * every change, so the agent sees an ABORT after its own CHANGE succeeded.
 *   reject_sub [-p priority]     prints "ready" when subscribed, stops on SIGTERM
 * SPDX-License-Identifier: MIT */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sysrepo.h>
#include <sysrepo/error_format.h>

static int on_change(sr_session_ctx_t *sess, uint32_t sub_id, const char *module, const char *xpath,
                     sr_event_t event, uint32_t op_id, void *priv)
{
    (void)sub_id; (void)module; (void)xpath; (void)priv;
    if (event != SR_EV_CHANGE)
        return SR_ERR_OK;
    printf("reject_sub: op %u rejected\n", op_id);
    sr_session_set_netconf_error(sess, "application", "operation-failed", NULL, NULL,
                                 "rejected by the test subscriber", 0);
    return SR_ERR_OPERATION_FAILED;
}

int main(int argc, char **argv)
{
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    sr_subscription_ctx_t *sub = NULL;
    uint32_t prio = 0;
    sigset_t sigs;
    int opt, sig, rc;

    setvbuf(stdout, NULL, _IOLBF, 0);
    while ((opt = getopt(argc, argv, "p:")) != -1)
        if (opt == 'p')
            prio = (uint32_t)atoi(optarg);
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);
    if ((rc = sr_connect(0, &conn)) || (rc = sr_session_start(conn, SR_DS_RUNNING, &sess)) ||
        (rc = sr_module_change_subscribe(sess, "labsw-switch", NULL, on_change, NULL, prio, 0, &sub))) {
        fprintf(stderr, "reject_sub: %s\n", sr_strerror(rc));
        return 1;
    }
    printf("ready\n");
    sigwait(&sigs, &sig);
    sr_unsubscribe(sub);
    sr_disconnect(conn);
    return 0;
}
