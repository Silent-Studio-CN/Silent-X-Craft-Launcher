/* 递归删除目录树(独立文件,避免改动平台大文件)。
 *
 * 为什么需要:失败的安装要能清掉"本次新建的实例目录"(页面规格 §2.8 断言 10:
 * 失败后 versions/<版本名>/ 必须不存在),而 fs.h 原来只有删单个文件。
 * 用法约定(调用方负责判断安全性):只删自己刚建的目录,绝不删用户已有实例 ——
 * 这个函数本身不做这种判断,它只是"递归删"。
 */
#include "sxcl/fs.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <stdlib.h>
#  include <string.h>

static int remove_tree_wide(const wchar_t *wpath)
{
    WIN32_FIND_DATAW found;
    wchar_t pattern[MAX_PATH * 2];
    (void)swprintf(pattern, sizeof(pattern) / sizeof(pattern[0]), L"%ls\\*", wpath);
    HANDLE h = FindFirstFileW(pattern, &found);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) {
                continue;
            }
            wchar_t child[MAX_PATH * 2];
            (void)swprintf(child, sizeof(child) / sizeof(child[0]), L"%ls\\%ls", wpath, found.cFileName);
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                (void)remove_tree_wide(child);
            } else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                (void)DeleteFileW(child);
            }
        } while (FindNextFileW(h, &found));
        FindClose(h);
    }
    if (RemoveDirectoryW(wpath)) {
        return 0;
    }
    return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
}

int sxcl_fs_remove_tree(const char *path)
{
    if (!path || !*path) {
        return -1;
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (need <= 0) {
        return -1;
    }
    wchar_t *w = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!w) {
        return -1;
    }
    int rc = -1;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, need) == need) {
        const DWORD attr = GetFileAttributesW(w);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            rc = 0; /* 本来就不存在:算成功 */
        } else if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
            rc = DeleteFileW(w) ? 0 : -1;
        } else {
            rc = remove_tree_wide(w);
        }
    }
    free(w);
    return rc;
}
#else
#  include <dirent.h>
#  include <stdio.h>
#  include <string.h>
#  include <sys/stat.h>
#  include <unistd.h>

static int remove_tree_posix(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e = NULL;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            char child[4096];
            (void)snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(child, &st) != 0) {
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                (void)remove_tree_posix(child);
            } else {
                (void)unlink(child);
            }
        }
        closedir(d);
    }
    if (rmdir(path) == 0) {
        return 0;
    }
    return 0; /* 不存在或已删掉都算成功(与 Windows 侧一致) */
}

int sxcl_fs_remove_tree(const char *path)
{
    if (!path || !*path) {
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? 0 : -1;
    }
    return remove_tree_posix(path);
}
#endif
