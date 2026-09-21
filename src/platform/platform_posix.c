/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "sxcl/fs.h"

#include <errno.h>
#include <fcntl.h>
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
#if defined(__APPLE__)
        /* macOS 的 struct stat 用 st_mtimespec,没有 st_mtim */
        *mtime_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + (int64_t)st.st_mtimespec.tv_nsec;
#else
        *mtime_ns = (int64_t)st.st_mtime * 1000000000LL + (int64_t)st.st_mtim.tv_nsec;
#endif
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

/* ── 下载落盘:sxcl_file(POSIX) ── */

struct sxcl_file {
    int fd;
};

sxcl_file *sxcl_file_open_write(const char *path, int64_t final_size)
{
    if (!path) {
        return NULL;
    }
    const int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        return NULL;
    }
    if (final_size > 0 && ftruncate(fd, (off_t)final_size) != 0) {
        close(fd);
        return NULL;
    }
    sxcl_file *f = (sxcl_file *)malloc(sizeof(sxcl_file));
    if (!f) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    return f;
}

int64_t sxcl_file_write_at(sxcl_file *file, const void *data, size_t len, int64_t offset)
{
    if (!file || !data) {
        return -1;
    }
    size_t done = 0;
    while (done < len) {
        const ssize_t n = pwrite(file->fd, (const char *)data + done, len - done,
                                 (off_t)(offset + (int64_t)done));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }
    return (int64_t)done;
}

int sxcl_file_flush(sxcl_file *file)
{
    if (!file) {
        return -1;
    }
    return fsync(file->fd) == 0 ? 0 : -1;
}

int sxcl_file_close(sxcl_file *file)
{
    if (!file) {
        return -1;
    }
    const int rc = close(file->fd);
    free(file);
    return rc == 0 ? 0 : -1;
}
