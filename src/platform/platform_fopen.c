/* UTF-8 路径安全的 fopen(独立文件,避免改动平台大文件时与别的改动冲突)。
 *
 * 为什么必须单独提供:MSVC 的 fopen 按**当前 ANSI 代码页**解释路径,中文目录(国内用户是常态)
 * 会直接失败 —— 实测 `sxcl-dl options build\中文测试\选项.txt --set ...` 报"保存失败",
 * 而同一路径用 CreateFileW/_wfopen 就正常。本工程所有读写文件的地方都该走它。 */
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
