/* backup.h - backup files: safe names, atomic replace, bounded read
 * SPDX-License-Identifier: MIT */
#ifndef LSW_BACKUP_H
#define LSW_BACKUP_H

#include <stddef.h>

#define LSW_BACKUP_NAME_MAX 32
#define LSW_BACKUP_SIZE_MAX (4 * 1024 * 1024)

/* 1 if name is 1..32 of [A-Za-z0-9_.-] and does not start with '.' (as labsw-ops) */
int lsw_backup_name_ok(const char *name);

/* write buf to dir/name.json so that the file is either the old one or the
 * complete new one, also after a crash; makes dir if missing.
 * fail_after > 0 (tests): the write fails after that many bytes. */
int lsw_backup_write(const char *dir, const char *name, const char *buf, size_t len, long fail_after, char *err,
                     size_t errlen);

/* read dir/name.json into a NUL-terminated malloc'ed buffer; -1 with err */
int lsw_backup_read(const char *dir, const char *name, char **buf, size_t *len, char *err, size_t errlen);

/* remove temporary files a crash left in dir; returns how many */
int lsw_backup_sweep(const char *dir);

#endif
