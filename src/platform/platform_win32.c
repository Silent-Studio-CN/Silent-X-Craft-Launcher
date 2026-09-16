/* Windows 侧文件系统实现。UTF-8 入口 -> UTF-16 调用 Win32,不依赖 CRT 代码页。 */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "sxcl/fs.h"

#include <stdlib.h>
#include <string.h>

static wchar_t *sxcl_win32_utf8_to_wide(const char *utf8)
{
    if (!utf8) {
        return NULL;
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (need <= 0) {
        return NULL;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wide) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, need) != need) {
        free(wide);
        return NULL;
    }
    return wide;
}

int sxcl_fs_stat(const char *path, int64_t *size, int64_t *mtime_ns)
{
    if (!path) {
        return -3;
    }
    wchar_t *wide = sxcl_win32_utf8_to_wide(path);
    if (!wide) {
        return -2;
    }
    WIN32_FILE_ATTRIBUTE_DATA info;
    const BOOL ok = GetFileAttributesExW(wide, GetFileExInfoStandard, &info);
    free(wide);
    if (!ok) {
        const DWORD err = GetLastError();
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? -1 : -2;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return -1;
    }
    if (size) {
        LARGE_INTEGER li;
        li.HighPart = (LONG)info.nFileSizeHigh;
        li.LowPart = info.nFileSizeLow;
        *size = (int64_t)li.QuadPart;
    }
    if (mtime_ns) {
        /* FILETIME 是 1601 起的 100ns 计数 */
        ULARGE_INTEGER ul;
        ul.HighPart = info.ftLastWriteTime.dwHighDateTime;
        ul.LowPart = info.ftLastWriteTime.dwLowDateTime;
        *mtime_ns = (int64_t)(ul.QuadPart * 100ULL);
    }
    return 0;
}

int sxcl_fs_is_dir(const char *path)
{
    if (!path) {
        return 0;
    }
    wchar_t *wide = sxcl_win32_utf8_to_wide(path);
    if (!wide) {
        return 0;
    }
    const DWORD attr = GetFileAttributesW(wide);
    free(wide);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0) ? 1 : 0;
}

int sxcl_fs_exists(const char *path)
{
    if (!path) {
        return 0;
    }
    wchar_t *wide = sxcl_win32_utf8_to_wide(path);
    if (!wide) {
        return 0;
    }
    const DWORD attr = GetFileAttributesW(wide);
    free(wide);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) ? 1 : 0;
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

    /* 逐级创建:把 '/' 与 '\\' 统一成 '\\',跳过盘符后的首个分隔符 */
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\\';
        }
    }
    int rc = 0;
    for (size_t i = 1; i <= len; ++i) {
        const int at_end = (i == len);
        if (!at_end && buf[i] != '\\') {
            continue;
        }
        const char saved = buf[i];
        buf[i] = '\0';
        if (buf[i - 1] != '\\' && buf[i - 1] != ':') {
            wchar_t *wide = sxcl_win32_utf8_to_wide(buf);
            if (wide) {
                if (CreateDirectoryW(wide, NULL) == 0) {
                    const DWORD err = GetLastError();
                    if (err != ERROR_ALREADY_EXISTS) {
                        rc = -2;
                    }
                }
                free(wide);
            } else {
                rc = -2;
            }
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
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
    if (!slash || slash == path) {
        return 0; /* 无目录部分 */
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
    wchar_t *wsrc = sxcl_win32_utf8_to_wide(src);
    wchar_t *wdst = sxcl_win32_utf8_to_wide(dst);
    int rc = -2;
    if (wsrc && wdst) {
        rc = MoveFileExW(wsrc, wdst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) ? 0 : -2;
    }
    free(wsrc);
    free(wdst);
    return rc;
}

int sxcl_fs_remove(const char *path)
{
    if (!path) {
        return -3;
    }
    wchar_t *wide = sxcl_win32_utf8_to_wide(path);
    int rc = -2;
    if (wide) {
        if (DeleteFileW(wide)) {
            rc = 0;
        } else if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            rc = 0;
        }
    }
    free(wide);
    return rc;
}
