/* exec.c
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "exec.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

struct sink {
    char *buf;
    size_t len;
    int over;
};

/* read what is there; returns 0 at EOF, 1 if more may come, -1 on error */
static int drain(int fd, struct sink *s)
{
    char tmp[16384];

    for (;;) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            return 1;
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
        if (s->len + n > LSW_EXEC_OUT_MAX) {
            s->over = 1; /* keep reading so the child is not blocked, but drop the bytes */
            continue;
        }
        memcpy(s->buf + s->len, tmp, n);
        s->len += n;
        s->buf[s->len] = '\0';
    }
}

int lsw_exec(char *const argv[], int timeout_ms, struct lsw_exec_result *res)
{
    static char *const envp[] = {"LC_ALL=C", "PATH=/usr/sbin:/usr/bin:/sbin:/bin", NULL};
    int po[2] = {-1, -1}, pe[2] = {-1, -1}, rc = -1, status = 0, killed = 0;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    struct sink so = {0}, se = {0};
    pid_t pid = -1;
    long deadline, kill_at = 0;

    memset(res, 0, sizeof(*res));
    res->status = -1;
    if (argv[0][0] != '/')
        return -1;
    so.buf = malloc(LSW_EXEC_OUT_MAX + 1);
    se.buf = malloc(LSW_EXEC_OUT_MAX + 1);
    if (!so.buf || !se.buf || pipe2(po, O_CLOEXEC) || pipe2(pe, O_CLOEXEC))
        goto fail;
    so.buf[0] = se.buf[0] = '\0';
    if (posix_spawn_file_actions_init(&fa))
        goto fail;
    if (posix_spawnattr_init(&at)) {
        posix_spawn_file_actions_destroy(&fa);
        goto fail;
    }
    posix_spawn_file_actions_adddup2(&fa, po[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pe[1], STDERR_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    rc = posix_spawn(&pid, argv[0], &fa, &at, argv, envp);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(po[1]);
    close(pe[1]);
    po[1] = pe[1] = -1;
    if (rc) {
        pid = -1;
        goto fail;
    }
    fcntl(po[0], F_SETFL, O_NONBLOCK);
    fcntl(pe[0], F_SETFL, O_NONBLOCK);

    deadline = now_ms() + timeout_ms;
    int open_o = 1, open_e = 1;
    while (open_o || open_e) {
        struct pollfd pf[2] = {{po[0], POLLIN, 0}, {pe[0], POLLIN, 0}};
        long now = now_ms(), wait = (killed ? kill_at : deadline) - now;
        if (wait <= 0) {
            if (!killed) {
                kill(-pid, SIGTERM);
                killed = 1;
                kill_at = now + 500;
                continue;
            }
            kill(-pid, SIGKILL);
            break; /* stop waiting for EOF; a grandchild may hold the pipe */
        }
        if (!open_o)
            pf[0].fd = -1;
        if (!open_e)
            pf[1].fd = -1;
        if (poll(pf, 2, (int)wait) < 0 && errno != EINTR)
            break;
        if (open_o && (pf[0].revents & (POLLIN | POLLHUP | POLLERR)))
            open_o = drain(po[0], &so) > 0;
        if (open_e && (pf[1].revents & (POLLIN | POLLHUP | POLLERR)))
            open_e = drain(pe[0], &se) > 0;
    }
    if (!killed && (open_o || open_e))
        kill(-pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            status = -1;
            break;
        }
    }
    res->timedout = killed;
    res->toolong = so.over || se.over;
    res->status = (status != -1 && WIFEXITED(status) && !killed) ? WEXITSTATUS(status) : -1;
    res->out = so.buf;
    res->err = se.buf;
    close(po[0]);
    close(pe[0]);
    return 0;
fail:
    for (int i = 0; i < 2; i++) {
        if (po[i] >= 0)
            close(po[i]);
        if (pe[i] >= 0)
            close(pe[i]);
    }
    free(so.buf);
    free(se.buf);
    return -1;
}

void lsw_exec_free(struct lsw_exec_result *res)
{
    free(res->out);
    free(res->err);
    res->out = res->err = NULL;
}
