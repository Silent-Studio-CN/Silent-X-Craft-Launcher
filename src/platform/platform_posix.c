/* POSIX 侧文件系统实现(Linux / Android)。与 platform_win32.c 一一对应。
 * 说明:本文件在开发机(Windows)上不参与编译,由 CI/Linux 侧编译验证。 */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "sxcl/fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int sxcl_fs_stat(const char *path, int64_t *size, int64_t *mtime_ns)
{
    if (!path) {
        return -3;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return (errno == ENOENT || errno == ENOTDIR) ? -1 : -2;
    }
    if (!S_ISREG(st.st_mode)) {
        return -1;
    }
    if (size) {
        *size = (int64_t)st.st_size;
    }
    if (mtime_ns) {
        *mtime_ns = (int64_t)st.st_mtime * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
    }
    return 0;
}

int sxcl_fs_is_dir(const char *path)
{
    if (!path) {
        return 0;
    }
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
}

int sxcl_fs_exists(const char *path)
{
    if (!path) {
        return 0;
    }
    struct stat st;
    return (stat(path, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
}

int sxcl_fs_mkdirs(const char *path)
{
    if (!path || path[0] == '\0') {
        return -3;
    }
    const size_t len = strlen(path);
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        return -2;
    }
    memcpy(buf, path, len + 1);
    int rc = 0;
    for (size_t i = 1; i <= len; ++i) {
        if (i != len && buf[i] != '/') {
            continue;
        }
        const char saved = buf[i];
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
            rc = -2;
        }
        buf[i] = saved;
        if (rc != 0) {
            break;
        }
    }
    free(buf);
    return rc;
}

int sxcl_fs_mkdirs_for_file(const char *path)
{
    if (!path) {
        return -3;
    }
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        return 0;
    }
    const size_t len = (size_t)(slash - path);
    char *dir = (char *)malloc(len + 1);
    if (!dir) {
        return -2;
    }
    memcpy(dir, path, len);
    dir[len] = '\0';
    const int rc = sxcl_fs_mkdirs(dir);
    free(dir);
    return rc;
}

int sxcl_fs_rename_replace(const char *src, const char *dst)
{
    if (!src || !dst) {
        return -3;
    }
    return rename(src, dst) == 0 ? 0 : -2;
}

int sxcl_fs_remove(const char *path)
{
    if (!path) {
        return -3;
    }
    if (unlink(path) == 0 || errno == ENOENT) {
        return 0;
    }
    return -2;
}
