/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* _wremove/_wfopen 系列在 MSVC 下被标记弃用;C4996 在 /WX 下会打挂构建 */
#endif

#include "io_internal.h"

#include "sxcl/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

static void io_set_err(char *err, size_t err_len, const char *msg)
{
    if (err == NULL || err_len == 0) {
        return;
    }
    (void)snprintf(err, err_len, "%s", msg ? msg : "");
}

int sxcl_dir_join(char *out, size_t out_len, const char *a, const char *b)
{
    if (out == NULL || out_len == 0 || a == NULL || b == NULL) {
        return -1;
    }
    size_t n = strlen(a);
    while (n > 0 && (a[n - 1] == '/' || a[n - 1] == '\\')) {
        --n;
    }
    if (n == 0) {
        return (int)snprintf(out, out_len, "%s", b);
    }
    return (int)snprintf(out, out_len, "%.*s/%s", (int)n, a, b);
}

int sxcl_dir_read_file(const char *path, char **out, size_t *out_len, char *err, size_t err_len)
{
    if (out == NULL || out_len == NULL || path == NULL || *path == '\0') {
        io_set_err(err, err_len, "读文件参数不合法");
        return -1;
    }
    *out = NULL;
    *out_len = 0;

    FILE *fh = sxcl_fs_fopen(path, "rb");
    if (fh == NULL) {
        io_set_err(err, err_len, "打不开文件(不存在或没有权限)");
        return -2;
    }
    size_t cap = 65536;
    size_t len = 0;
    char *buf = (char *)malloc(cap + 1);
    if (buf == NULL) {
        fclose(fh);
        io_set_err(err, err_len, "内存不足");
        return -4;
    }
    for (;;) {
        if (len == cap) {
            size_t next = cap * 2;
            char *grown = (char *)realloc(buf, next + 1);
            if (grown == NULL) {
                free(buf);
                fclose(fh);
                io_set_err(err, err_len, "文件太大,内存不足");
                return -4;
            }
            buf = grown;
            cap = next;
        }
        const size_t got = fread(buf + len, 1, cap - len, fh);
        len += got;
        if (got == 0) {
            if (ferror(fh) != 0) {
                free(buf);
                fclose(fh);
                io_set_err(err, err_len, "读文件中途出错");
                return -7;
            }
            break;
        }
    }
    (void)fclose(fh);
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return 0;
}

int sxcl_dir_name_compare(const char *a, const char *b)
{
    if (a == NULL) { a = ""; }
    if (b == NULL) { b = ""; }
    for (;;) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        unsigned char la = (ca >= 'A' && ca <= 'Z') ? (unsigned char)(ca - 'A' + 'a') : ca;
        unsigned char lb = (cb >= 'A' && cb <= 'Z') ? (unsigned char)(cb - 'A' + 'a') : cb;
        if (la != lb) {
            return (la < lb) ? -1 : 1;
        }
        if (ca == '\0') {
            return 0;
        }
        ++a;
        ++b;
    }
}

#if defined(_WIN32)

static wchar_t *io_utf8_to_wide(const char *utf8)
{
    if (utf8 == NULL) {
        return NULL;
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (need <= 0) {
        return NULL;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (wide == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, need) != need) {
        free(wide);
        return NULL;
    }
    return wide;
}

static int io_wide_to_utf8(const wchar_t *wide, char *out, size_t out_len)
{
    if (wide == NULL || out == NULL || out_len == 0) {
        return -1;
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (need <= 0) {
        return -1;
    }
    if ((size_t)need > out_len) {
        return -1;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, need, NULL, NULL) != need) {
        return -1;
    }
    return 0;
}

int sxcl_dir_list_open(const char *path, sxcl_dir_list *out, char *err, size_t err_len)
{
    if (out == NULL || path == NULL || *path == '\0') {
        io_set_err(err, err_len, "列目录参数不合法");
        return -1;
    }
    out->items = NULL;
    out->count = 0;

    wchar_t *wdir = io_utf8_to_wide(path);
    if (wdir == NULL) {
        io_set_err(err, err_len, "路径不是合法的 UTF-8");
        return -2;
    }
    const size_t wlen = wcslen(wdir);
    wchar_t *wpattern = (wchar_t *)malloc((wlen + 3) * sizeof(wchar_t));
    if (wpattern == NULL) {
        free(wdir);
        io_set_err(err, err_len, "内存不足");
        return -4;
    }
    (void)memcpy(wpattern, wdir, wlen * sizeof(wchar_t));
    wpattern[wlen] = L'\\';
    wpattern[wlen + 1] = L'*';
    wpattern[wlen + 2] = L'\0';
    free(wdir);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    free(wpattern);
    if (h == INVALID_HANDLE_VALUE) {
        io_set_err(err, err_len, "目录打不开(不存在或没有权限)");
        return -2;
    }

    size_t cap = 32;
    sxcl_dir_entry *items = (sxcl_dir_entry *)malloc(cap * sizeof(sxcl_dir_entry));
    if (items == NULL) {
        (void)FindClose(h);
        io_set_err(err, err_len, "内存不足");
        return -4;
    }
    size_t count = 0;
    for (;;) {
        if (fd.cFileName[0] != L'\0' && !(fd.cFileName[0] == L'.' && fd.cFileName[1] == L'\0') &&
            !(fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')) {
            char name[SXCL_DIR_NAME_MAX];
            if (io_wide_to_utf8(fd.cFileName, name, sizeof(name)) == 0) {
                if (count == cap) {
                    size_t next = cap * 2;
                    sxcl_dir_entry *grown = (sxcl_dir_entry *)realloc(items, next * sizeof(sxcl_dir_entry));
                    if (grown == NULL) {
                        free(items);
                        (void)FindClose(h);
                        io_set_err(err, err_len, "内存不足");
                        return -4;
                    }
                    items = grown;
                    cap = next;
                }
                sxcl_dir_entry *entry = &items[count++];
                (void)memcpy(entry->name, name, strlen(name) + 1);
                entry->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
                entry->size = (int64_t)((uint64_t)fd.nFileSizeHigh << 32 | (uint64_t)fd.nFileSizeLow);
                if (entry->is_dir) {
                    entry->size = 0;
                }
            }
        }
        if (FindNextFileW(h, &fd) == 0) {
            break;
        }
    }
    (void)FindClose(h);
    out->items = items;
    out->count = count;
    return 0;
}

int sxcl_dir_remove_tree(const char *path)
{
    if (path == NULL || *path == '\0') {
        return -1;
    }
    if (!sxcl_fs_is_dir(path)) {
        return sxcl_fs_remove(path); /* 文件/不存在:都交给它 */
    }
    sxcl_dir_list list;
    char err[SXCL_DIR_ERROR_MAX];
    if (sxcl_dir_list_open(path, &list, err, sizeof(err)) != 0) {
        return -2;
    }
    int rc = 0;
    for (size_t i = 0; i < list.count; ++i) {
        char child[2048];
        (void)sxcl_dir_join(child, sizeof(child), path, list.items[i].name);
        if (list.items[i].is_dir) {
            if (sxcl_dir_remove_tree(child) != 0) {
                rc = -2;
            }
        } else if (sxcl_fs_remove(child) != 0) {
            rc = -2;
        }
    }
    sxcl_dir_list_free(&list);

    wchar_t *wdir = io_utf8_to_wide(path);
    if (wdir == NULL || _wrmdir(wdir) != 0) {
        rc = -2;
    }
    free(wdir);
    return rc;
}

#else /* POSIX */

int sxcl_dir_list_open(const char *path, sxcl_dir_list *out, char *err, size_t err_len)
{
    if (out == NULL || path == NULL || *path == '\0') {
        io_set_err(err, err_len, "列目录参数不合法");
        return -1;
    }
    out->items = NULL;
    out->count = 0;

    DIR *dir = opendir(path);
    if (dir == NULL) {
        io_set_err(err, err_len, "目录打不开(不存在或没有权限)");
        return -2;
    }
    size_t cap = 32;
    sxcl_dir_entry *items = (sxcl_dir_entry *)malloc(cap * sizeof(sxcl_dir_entry));
    if (items == NULL) {
        (void)closedir(dir);
        io_set_err(err, err_len, "内存不足");
        return -4;
    }
    size_t count = 0;
    struct dirent *de = NULL;
    while ((de = readdir(dir)) != NULL) {
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        const size_t nlen = strlen(name);
        if (nlen == 0 || nlen >= SXCL_DIR_NAME_MAX) {
            continue;
        }
        char child[4096];
        (void)sxcl_dir_join(child, sizeof(child), path, name);
        struct stat st;
        if (stat(child, &st) != 0) {
            continue;
        }
        if (count == cap) {
            size_t next = cap * 2;
            sxcl_dir_entry *grown = (sxcl_dir_entry *)realloc(items, next * sizeof(sxcl_dir_entry));
            if (grown == NULL) {
                free(items);
                (void)closedir(dir);
                io_set_err(err, err_len, "内存不足");
                return -4;
            }
            items = grown;
            cap = next;
        }
        sxcl_dir_entry *entry = &items[count++];
        (void)memcpy(entry->name, name, nlen + 1);
        entry->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
        entry->size = entry->is_dir ? 0 : (int64_t)st.st_size;
    }
    (void)closedir(dir);
    out->items = items;
    out->count = count;
    return 0;
}

int sxcl_dir_remove_tree(const char *path)
{
    if (path == NULL || *path == '\0') {
        return -1;
    }
    if (!sxcl_fs_is_dir(path)) {
        return sxcl_fs_remove(path);
    }
    sxcl_dir_list list;
    char err[SXCL_DIR_ERROR_MAX];
    if (sxcl_dir_list_open(path, &list, err, sizeof(err)) != 0) {
        return -2;
    }
    int rc = 0;
    for (size_t i = 0; i < list.count; ++i) {
        char child[8192];
        (void)sxcl_dir_join(child, sizeof(child), path, list.items[i].name);
        if (list.items[i].is_dir) {
            if (sxcl_dir_remove_tree(child) != 0) {
                rc = -2;
            }
        } else if (sxcl_fs_remove(child) != 0) {
            rc = -2;
        }
    }
    sxcl_dir_list_free(&list);
    if (rmdir(path) != 0) {
        rc = -2;
    }
    return rc;
}

#endif /* _WIN32 */

void sxcl_dir_list_free(sxcl_dir_list *list)
{
    if (list == NULL) {
        return;
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}
