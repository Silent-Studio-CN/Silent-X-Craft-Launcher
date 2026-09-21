/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* _wfopen 在 MSVC 下被标记弃用;C4996 在 /WX 下会直接打挂构建 */
#endif

#include "sxcl/fs.h"

#include <stdio.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <stdlib.h>
#  include <string.h>

FILE *sxcl_fs_fopen(const char *path, const char *mode)
{
    if (!path || !mode) {
        return NULL;
    }
    const int path_need = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    const int mode_need = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (path_need <= 0 || mode_need <= 0) {
        return NULL;
    }
    wchar_t *wpath = (wchar_t *)malloc((size_t)path_need * sizeof(wchar_t));
    wchar_t *wmode = (wchar_t *)malloc((size_t)mode_need * sizeof(wchar_t));
    FILE *fh = NULL;
    if (wpath && wmode &&
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, path_need) == path_need &&
        MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, mode_need) == mode_need) {
        fh = _wfopen(wpath, wmode);
    }
    free(wpath);
    free(wmode);
    return fh;
}
#else
FILE *sxcl_fs_fopen(const char *path, const char *mode)
{
    if (!path || !mode) {
        return NULL;
    }
    return fopen(path, mode);
}
#endif
