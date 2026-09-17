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
        /* FILETIME 是"1601-01-01 起的 100ns 计数":必须先减掉到 Unix 纪元的偏移(11644473600 秒),
         * 再乘 100 转纳秒。直接 *100 会溢出 int64(实测回绕成 -5012648075019669516),
         * 虽然"稳定"但当 key 之外的一切用途(显示/比较)都是错的 —— 由启动层的实测发现。 */
        *mtime_ns = (int64_t)((ul.QuadPart - 116444736000000000ULL) * 100ULL);
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

/* ── 下载落盘:sxcl_file(Windows) ── */

struct sxcl_file {
    HANDLE handle;
};

sxcl_file *sxcl_file_open_write(const char *path, int64_t final_size)
{
    if (!path) {
        return NULL;
    }
    wchar_t *wide = sxcl_win32_utf8_to_wide(path);
    if (!wide) {
        return NULL;
    }
    HANDLE h = CreateFileW(wide, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (final_size > 0) {
        /* 预分配:避免下载过程中反复扩文件,也给磁盘留出连续空间的机会 */
        LARGE_INTEGER li;
        li.QuadPart = final_size;
        if (SetFilePointerEx(h, li, NULL, FILE_BEGIN) && !SetEndOfFile(h)) {
            CloseHandle(h);
            return NULL;
        }
    }
    sxcl_file *f = (sxcl_file *)malloc(sizeof(sxcl_file));
    if (!f) {
        CloseHandle(h);
        return NULL;
    }
    f->handle = h;
    return f;
}

int64_t sxcl_file_write_at(sxcl_file *file, const void *data, size_t len, int64_t offset)
{
    if (!file || !data) {
        return -1;
    }
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset = (DWORD)(offset & 0xFFFFFFFFLL);
    ov.OffsetHigh = (DWORD)((offset >> 32) & 0xFFFFFFFFLL);
    DWORD written = 0;
    const DWORD want = (DWORD)(len > 0x7FFFFFFFu ? 0x7FFFFFFFu : len);
    if (!WriteFile(file->handle, data, want, &written, &ov)) {
        return -1;
    }
    return (int64_t)written;
}

int sxcl_file_flush(sxcl_file *file)
{
    if (!file) {
        return -1;
    }
    return FlushFileBuffers(file->handle) ? 0 : -1;
}

int sxcl_file_close(sxcl_file *file)
{
    if (!file) {
        return -1;
    }
    const BOOL ok = CloseHandle(file->handle);
    free(file);
    return ok ? 0 : -1;
}
