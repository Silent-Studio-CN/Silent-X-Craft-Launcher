/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "diag_internal.h"

#include <stdio.h>
#include <string.h>

#include "sxcl/fs.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dirent.h>
#endif

#if defined(_WIN32)
#  define SXCL_DIAG_SEP '\\'
#else
#  define SXCL_DIAG_SEP '/'
#endif

int sxcl_diag_join(char *out, size_t cap, const char *a, const char *b)
{
    size_t n = 0;
    size_t take = 0;
    if (out == NULL || cap == 0u) {
        return -1;
    }
    out[0] = '\0';
    if (a != NULL && *a != '\0') {
        n = strlen(a);
        while (n > 0u && (a[n - 1u] == '/' || a[n - 1u] == '\\')) {
            --n;
        }
        if (n + 2u > cap) {
            return -1;
        }
        memcpy(out, a, n);
        out[n] = '\0';
    }
    if (b != NULL && *b != '\0') {
        const size_t m = strlen(b);
        if (n > 0u) {
            if (n + 1u >= cap) {
                return -1;
            }
            out[n++] = SXCL_DIAG_SEP;
            out[n] = '\0';
        }
        take = (n + m < cap) ? m : (cap - 1u - n);
        if (take < m) {
            return -1;
        }
        memcpy(out + n, b, take);
        out[n + take] = '\0';
    }
    return 0;
}

int sxcl_diag_ends_with_ci(const char *text, const char *suffix)
{
    size_t n = 0;
    size_t m = 0;
    size_t i = 0;
    if (text == NULL || suffix == NULL) {
        return 0;
    }
    n = strlen(text);
    m = strlen(suffix);
    if (m == 0u || m > n) {
        return 0;
    }
    for (i = 0; i < m; ++i) {
        char a = text[n - m + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

int sxcl_diag_has_visible_text(const char *path)
{
    unsigned char buf[512];
    FILE *f = NULL;
    size_t got = 0;
    size_t i = 0;
    int64_t size = 0;
    if (path == NULL || sxcl_fs_stat(path, &size, NULL) != 0 || size <= 0) {
        return 0;
    }
    f = sxcl_fs_fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    got = fread(buf, 1, sizeof(buf), f);
    (void)fclose(f);
    for (i = 0; i < got; ++i) {
        if (buf[i] != (unsigned char)' ' && buf[i] != (unsigned char)'\t' &&
            buf[i] != (unsigned char)'\r' && buf[i] != (unsigned char)'\n' && buf[i] != 0u) {
            return 1;
        }
    }
    /* 前 512 字节全是空白:再看一看总大小,大文件不可能是空的 */
    return size > (int64_t)sizeof(buf) ? 1 : 0;
}

#if defined(_WIN32)
static int diag_utf8_to_wide(const char *text, wchar_t *out, size_t out_count)
{
    int need = 0;
    if (text == NULL || out == NULL || out_count == 0u) {
        return 0;
    }
    need = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (need <= 0 || (size_t)need > out_count) {
        return 0;
    }
    return MultiByteToWideChar(CP_UTF8, 0, text, -1, out, need) == need ? 1 : 0;
}

static int diag_wide_to_utf8(const wchar_t *text, char *out, size_t out_count)
{
    int need = 0;
    if (text == NULL || out == NULL || out_count == 0u) {
        return 0;
    }
    need = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (need <= 0 || (size_t)need > out_count) {
        return 0;
    }
    return WideCharToMultiByte(CP_UTF8, 0, text, -1, out, need, NULL, NULL) == need ? 1 : 0;
}
#endif

int sxcl_diag_dir_list(const char *dir, sxcl_diag_dir *out)
{
    if (out == NULL) {
        return -1;
    }
    out->count = 0;
    out->truncated = 0;
    if (dir == NULL || *dir == '\0' || !sxcl_fs_is_dir(dir)) {
        return -1;
    }
#if defined(_WIN32)
    {
        wchar_t wdir[1024];
        wchar_t pattern[1100];
        WIN32_FIND_DATAW found;
        HANDLE h = INVALID_HANDLE_VALUE;
        if (!diag_utf8_to_wide(dir, wdir, sizeof(wdir) / sizeof(wdir[0]))) {
            return -1;
        }
        (void)swprintf(pattern, sizeof(pattern) / sizeof(pattern[0]), L"%ls\\*", wdir);
        h = FindFirstFileW(pattern, &found);
        if (h == INVALID_HANDLE_VALUE) {
            return -1;
        }
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                continue;
            }
            if (out->count >= SXCL_DIAG_MAX_FILES) {
                out->truncated = 1;
                break;
            }
            if (!diag_wide_to_utf8(found.cFileName, out->names[out->count], SXCL_DIAG_NAME_MAX)) {
                continue;
            }
            out->count += 1u;
        } while (FindNextFileW(h, &found));
        FindClose(h);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *entry = NULL;
        if (d == NULL) {
            return -1;
        }
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '\0' || strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (out->count >= SXCL_DIAG_MAX_FILES) {
                out->truncated = 1;
                break;
            }
            if (strlen(entry->d_name) >= SXCL_DIAG_NAME_MAX) {
                continue;
            }
            {
                /* readdir 连目录一起给:只收普通文件(POSIX 侧要拼出全路径才能判) */
                char full[1400];
                if (sxcl_diag_join(full, sizeof(full), dir, entry->d_name) != 0 ||
                    sxcl_fs_is_dir(full)) {
                    continue;
                }
            }
            (void)snprintf(out->names[out->count], SXCL_DIAG_NAME_MAX, "%s", entry->d_name);
            out->count += 1u;
        }
        closedir(d);
    }
#endif
    return 0;
}
