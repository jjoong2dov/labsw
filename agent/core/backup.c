/* backup.c - backup files: safe names, atomic replace (temp + fsync + rename), bounded read
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "backup.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* temp files start with '.', which a backup name cannot */
#define TMP_PREFIX ".tmp-"

int lsw_backup_name_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;

    if (n < 1 || n > LSW_BACKUP_NAME_MAX || name[0] == '.')
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
              c == '.'))
            return 0;
    }
    return 1;
}

static int mkdir_p(const char *dir)
{
    char path[4096];
    size_t n = strlen(dir);

    if (n == 0 || n >= sizeof(path))
        return -1;
    memcpy(path, dir, n + 1);
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(path, 0700) && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return mkdir(path, 0700) && errno != EEXIST ? -1 : 0;
}

static int write_all(int fd, const char *buf, size_t len, long fail_after)
{
    size_t done = 0;

    while (done < len) {
        size_t chunk = len - done;
        ssize_t w;
        if (fail_after > 0 && done + chunk > (size_t)fail_after)
            chunk = (size_t)fail_after - done;
        if (chunk == 0) {
            errno = EIO;
            return -2; /* injected */
        }
        if ((w = write(fd, buf + done, chunk)) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

int lsw_backup_write(const char *dir, const char *name, const char *buf, size_t len, long fail_after, char *err,
                     size_t errlen)
{
    char tmp[4096], path[4096];
    int fd = -1, dfd = -1, rc = -1, made = 0, w;

    if (!lsw_backup_name_ok(name)) {
        snprintf(err, errlen, "bad backup name");
        return -1;
    }
    if (snprintf(path, sizeof(path), "%s/%s.json", dir, name) >= (int)sizeof(path) ||
        snprintf(tmp, sizeof(tmp), "%s/" TMP_PREFIX "%s.XXXXXX", dir, name) >= (int)sizeof(tmp)) {
        snprintf(err, errlen, "backup directory path too long");
        return -1;
    }
    if (mkdir_p(dir) || (dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
        snprintf(err, errlen, "%s: %s", dir, strerror(errno));
        return -1;
    }
    if ((fd = mkostemp(tmp, O_CLOEXEC)) < 0) {
        snprintf(err, errlen, "%s: %s", dir, strerror(errno));
        goto end;
    }
    made = 1;
    /* the data must be on disk before the name points at it */
    if ((w = write_all(fd, buf, len, fail_after)) || fsync(fd)) {
        if (w == -2)
            snprintf(err, errlen, "injected failure after %ld bytes", fail_after);
        else
            snprintf(err, errlen, "writing %s: %s", tmp, strerror(errno));
        goto end;
    }
    if (close(fd)) {
        fd = -1;
        snprintf(err, errlen, "writing %s: %s", tmp, strerror(errno));
        goto end;
    }
    fd = -1;
    if (rename(tmp, path)) {
        snprintf(err, errlen, "rename to %s: %s", path, strerror(errno));
        goto end;
    }
    made = 0;
    /* make the rename itself survive a power loss */
    if (fsync(dfd)) {
        snprintf(err, errlen, "%s: %s", dir, strerror(errno));
        goto end;
    }
    rc = 0;
end:
    if (fd >= 0)
        close(fd);
    if (made)
        unlink(tmp);
    close(dfd);
    return rc;
}

int lsw_backup_read(const char *dir, const char *name, char **buf, size_t *len, char *err, size_t errlen)
{
    char path[4096];
    struct stat st;
    char *b = NULL;
    size_t got = 0;
    int fd, rc = -1;

    *buf = NULL;
    *len = 0;
    if (!lsw_backup_name_ok(name)) {
        snprintf(err, errlen, "bad backup name");
        return -1;
    }
    if (snprintf(path, sizeof(path), "%s/%s.json", dir, name) >= (int)sizeof(path)) {
        snprintf(err, errlen, "backup directory path too long");
        return -1;
    }
    if ((fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) < 0) {
        if (errno == ENOENT)
            snprintf(err, errlen, "no backup named \"%s\"", name);
        else
            snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > LSW_BACKUP_SIZE_MAX) {
        snprintf(err, errlen, "%s: not a regular file of at most %d bytes", path, LSW_BACKUP_SIZE_MAX);
        goto end;
    }
    if (!(b = malloc((size_t)st.st_size + 1))) {
        snprintf(err, errlen, "out of memory");
        goto end;
    }
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            snprintf(err, errlen, "reading %s: %s", path, r ? strerror(errno) : "file shrank");
            goto end;
        }
        got += (size_t)r;
    }
    b[got] = '\0';
    *buf = b;
    *len = got;
    b = NULL;
    rc = 0;
end:
    free(b);
    close(fd);
    return rc;
}

int lsw_backup_sweep(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;

    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (!strncmp(e->d_name, TMP_PREFIX, strlen(TMP_PREFIX)) && !unlinkat(dirfd(d), e->d_name, 0))
            n++;
    closedir(d);
    return n;
}
