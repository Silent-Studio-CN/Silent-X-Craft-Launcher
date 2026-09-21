/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* access() 在 MSVC 下被标记不安全;C4996 在 /WX 下会打挂构建 */
#endif

#include "sxcl/android.h"
#include "sxcl/fs.h" /* sxcl_fs_fopen(UTF-8 路径安全的 fopen)/ is_dir / exists / stat */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <errno.h>
#  include <stdlib.h> /* realpath */
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#define SXCL_ANDROID_MOUNTS_MAX 16384

/* ────────────────────────── 人话文案 ────────────────────────── */

const char *sxcl_android_access_name(sxcl_android_access access)
{
    switch (access) {
    case SXCL_ANDROID_OK:             return "可用";
    case SXCL_ANDROID_MISSING:        return "不存在";
    case SXCL_ANDROID_DENIED:         return "沙箱拒绝";
    case SXCL_ANDROID_NOEXEC:         return "共享存储不能执行";
    case SXCL_ANDROID_NOT_EXECUTABLE: return "没有执行位";
    case SXCL_ANDROID_NOT_READABLE:   return "读不了";
    case SXCL_ANDROID_ACCESS_COUNT:
    default:                          return "未知";
    }
}

const char *sxcl_android_access_key(sxcl_android_access access)
{
    switch (access) {
    case SXCL_ANDROID_OK:             return "ok";
    case SXCL_ANDROID_MISSING:        return "missing";
    case SXCL_ANDROID_DENIED:         return "denied";
    case SXCL_ANDROID_NOEXEC:         return "noexec";
    case SXCL_ANDROID_NOT_EXECUTABLE: return "not_executable";
    case SXCL_ANDROID_NOT_READABLE:   return "unreadable";
    case SXCL_ANDROID_ACCESS_COUNT:
    default:                          return "unknown";
    }
}

const char *sxcl_android_access_hint(sxcl_android_access access)
{
    switch (access) {
    case SXCL_ANDROID_OK:
        return "可以正常使用。";
    case SXCL_ANDROID_MISSING:
        return "这个位置没有东西;如果你刚装过,请确认装到了这里。";
    case SXCL_ANDROID_DENIED:
        return "安卓的沙箱挡住了这个位置:要么它是别的应用的私有目录,要么本应用还没有"
               "共享存储权限。两者都不靠猜 —— 具体解释见各自的提示文本。";
    case SXCL_ANDROID_NOEXEC:
        return "共享存储(内部存储 / sdcard)是 noexec 挂载,里面的程序起不来;"
               "Java 必须放在应用私有目录(/data/data/<包名>/files/...),不能放在 sdcard。";
    case SXCL_ANDROID_NOT_EXECUTABLE:
        return "文件在,但没有执行位(解压/拷贝时丢了 x 权限);重新解压到应用私有目录可以修好。";
    case SXCL_ANDROID_NOT_READABLE:
        return "读这个路径时出错;可能是权限问题或存储已卸载。";
    case SXCL_ANDROID_ACCESS_COUNT:
    default:
        return "未知情况。";
    }
}

int sxcl_android_is_android(void)
{
#if defined(__ANDROID__)
    return 1;
#else
    return 0;
#endif
}

/* ────────────────────────── 小工具 ────────────────────────── */

static void android_err(char *reason, size_t reason_len, const char *fmt, ...)
{
    va_list ap;
    if (reason == NULL || reason_len == 0) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(reason, reason_len, fmt, ap);
    va_end(ap);
}

/** mountpoint 是不是 path 的"按路径分量"的前缀。完全相等也算。
 *  /a 匹配 /a 与 /a/b,但**不**匹配 /ab。根挂载点 "/" 匹配一切。 */
static int mount_prefix_of(const char *mountpoint, size_t mlen, const char *path, size_t plen)
{
    if (mlen == 0 || plen == 0) {
        return 0;
    }
    if (mlen == 1 && mountpoint[0] == '/') {
        return path[0] == '/' ? 1 : 0;
    }
    if (plen < mlen) {
        return 0;
    }
    if (memcmp(mountpoint, path, mlen) != 0) {
        return 0;
    }
    return (plen == mlen) || (path[mlen] == '/') ? 1 : 0;
}

/** options("rw,nosuid,nodev,noexec,relatime") 里有没有恰好等于 name 的一项。 */
static int options_have(const char *options, size_t olen, const char *name)
{
    const size_t nlen = strlen(name);
    size_t i = 0;
    while (i < olen) {
        size_t j = i;
        while (j < olen && options[j] != ',') {
            ++j;
        }
        if (j - i == nlen && memcmp(options + i, name, nlen) == 0) {
            return 1;
        }
        i = (j < olen) ? j + 1 : olen;
    }
    return 0;
}

int sxcl_android_noexec_in_mounts(const char *mounts_text, const char *path)
{
    size_t plen = 0;
    size_t best_len = 0;
    int best = -1;
    const char *line = NULL;

    if (mounts_text == NULL || mounts_text[0] == '\0' || path == NULL || path[0] == '\0') {
        return -1;
    }
    plen = strlen(path);
    line = mounts_text;

    while (*line != '\0') {
        const char *eol = line;
        const char *field[4];   /* 0: dev  1: mountpoint  2: fstype  3: options */
        size_t flen[4];
        int idx = 0;
        while (*eol != '\0' && *eol != '\n') {
            ++eol;
        }
        {
            const char *p = line;
            while (idx < 4 && p < eol) {
                while (p < eol && (*p == ' ' || *p == '\t')) {
                    ++p;
                }
                if (p >= eol) {
                    break;
                }
                field[idx] = p;
                while (p < eol && *p != ' ' && *p != '\t') {
                    ++p;
                }
                flen[idx] = (size_t)(p - field[idx]);
                ++idx;
            }
        }
        if (idx >= 4 && mount_prefix_of(field[1], flen[1], path, plen) && flen[1] > best_len) {
            best_len = flen[1];
            best = options_have(field[3], flen[3], "noexec") ? 1 : 0;
        }
        line = (*eol == '\n') ? eol + 1 : eol;
    }
    return best;
}

#if !defined(_WIN32)
/* 定义在下面"路径体检"一节;这里先声明,因为挂载点判断要用它(见定义处注释)。 */
static int android_realpath(const char *path, char *out, size_t cap);
#endif

int sxcl_android_mount_noexec(const char *path)
{
#if defined(_WIN32)
    (void)path; /* Windows 没有 noexec 挂载的概念 */
    return -1;
#else
    FILE *fh = NULL;
    char buf[SXCL_ANDROID_MOUNTS_MAX];
    char resolved[4096];
    const char *use = path;
    size_t got = 0;
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    /* /sdcard 是 /storage/emulated/0 的符号链接。挂载点必须看**真实**路径,
     * 否则最长前缀会落到 "/" 上,把 noexec 的共享存储误判成可执行(设备实测踩过)。
     * 用 android_realpath:它对"还不存在"的路径也成立。 */
    if (android_realpath(path, resolved, sizeof(resolved)) == 0) {
        use = resolved;
    }
    fh = fopen("/proc/self/mounts", "rb");
    if (fh == NULL) {
        return -1;
    }
    got = fread(buf, 1, sizeof(buf) - 1, fh);
    (void)fclose(fh);
    buf[got] = '\0';
    return sxcl_android_noexec_in_mounts(buf, use);
#endif
}


#if !defined(_WIN32)

/* 把 path 规范化成"真实路径"。
 *
 * 为什么要这么麻烦:realpath() 对**不存在**的路径会失败,而我们要判的恰恰常常是
 * "还没装"的位置。设备实测踩过:/sdcard/x 上的 realpath 失败 -> 退回原串 -> 最长前缀落到
 * "/" 上 -> 把 noexec 的共享存储判成了可执行(noexec=0)。
 * 做法:realpath 整条路径;失败就逐级去掉末尾分量再试,成功后把去掉的部分接回去。
 * 这样 /sdcard/x(不存在)也能解析成 /storage/emulated/0/x,挂载点判断才是对的。
 * 返回 0 成功。 */
static int android_realpath(const char *path, char *out, size_t cap)
{
    char work[4096];
    char tail[2048];
    size_t n = 0;
    if (path == NULL || path[0] == '\0' || cap == 0) {
        return -1;
    }
    n = strlen(path);
    if (n >= sizeof(work)) {
        return -1;
    }
    (void)memcpy(work, path, n + 1);
    tail[0] = '\0';
    for (;;) {
        char resolved[4096];
        if (realpath(work, resolved) != NULL) {
            if (tail[0] == '\0') {
                const size_t rl = strlen(resolved);
                if (rl + 1 > cap) {
                    return -1;
                }
                (void)memcpy(out, resolved, rl + 1);
                return 0;
            }
            {
                const int written = snprintf(out, cap, "%s/%s", resolved, tail);
                return (written > 0 && (size_t)written < cap) ? 0 : -1;
            }
        }
        {
            char *slash = strrchr(work, '/');
            char new_tail[2048];
            int written = 0;
            if (slash == NULL || slash == work) {
                return -1;
            }
            *slash = '\0';
            if (tail[0] == '\0') {
                written = snprintf(new_tail, sizeof(new_tail), "%s", slash + 1);
            } else {
                written = snprintf(new_tail, sizeof(new_tail), "%s/%s", slash + 1, tail);
            }
            if (written <= 0 || (size_t)written >= sizeof(new_tail)) {
                return -1;
            }
            (void)memcpy(tail, new_tail, (size_t)written + 1);
        }
    }
}

#endif /* !_WIN32 */

/* ────────────────────────── 路径体检 ────────────────────────── */

#if defined(_WIN32)

/* Windows:GetFileAttributesW(UTF-8 路径先转宽字符,否则中文目录会失败)。
 * 返回 0 成功;否则返回一个 errno 风格的正数(2 = ENOENT,13 = EACCES)。 */
static int android_stat(const char *path, int *is_dir, int *has_exec)
{
    wchar_t w[1024];
    DWORD attr = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) <= 0) {
        return 13;
    }
    attr = GetFileAttributesW(w);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME)
                   ? 2
                   : 13;
    }
    *is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    *has_exec = *is_dir ? 0 : 1; /* Windows 没有执行位:普通文件都当能执行 */
    return 0;
}

static int android_readable(const char *path)
{
    wchar_t w[1024];
    HANDLE h = INVALID_HANDLE_VALUE;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) <= 0) {
        return 0;
    }
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    (void)CloseHandle(h);
    return 1;
}

#else /* POSIX / Android */

static int android_stat(const char *path, int *is_dir, int *has_exec)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return (errno != 0) ? errno : 5;
    }
    if (S_ISDIR(st.st_mode)) {
        *is_dir = 1;
        *has_exec = 0;
        return 0;
    }
    *is_dir = 0;
    *has_exec = (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) ? 1 : 0;
    return 0;
}

static int android_readable(const char *path)
{
    return (access(path, R_OK) == 0) ? 1 : 0;
}

#endif

sxcl_android_access sxcl_android_probe_path_with_mounts(const char *mounts_text, const char *path,
                                                        int want_exec,
                                                        char *reason, size_t reason_len)
{
    int is_dir = 0;
    int has_exec = 0;
    int rc = 0;

    if (path == NULL || path[0] == '\0') {
        android_err(reason, reason_len, "路径为空");
        return SXCL_ANDROID_MISSING;
    }

    rc = android_stat(path, &is_dir, &has_exec);
    if (rc != 0) {
        if (rc == 2 /* ENOENT */ || rc == 20 /* ENOTDIR */) {
            android_err(reason, reason_len, "不存在:%s", path);
            return SXCL_ANDROID_MISSING;
        }
        if (rc == 13 /* EACCES */ || rc == 1 /* EPERM */) {
            /* 只说事实:stat 被拒绝。**不说**是哪种原因 —— 可能是别人的私有目录
             * (/data/data/<别的包名>),也可能是本应用没有共享存储权限
             * (/storage/emulated/0/...),两者在设备的实测里都出现过。具体的解释
             * 交给调用方按场景给(Java 见 sxcl_java_verdict_hint,游戏目录见
             * sxcl_game_probe.hint)。 */
            android_err(reason, reason_len, "被沙箱挡住(stat 返回 Permission denied):%s", path);
            return SXCL_ANDROID_DENIED;
        }
        android_err(reason, reason_len, "读不了(stat 失败,errno=%d):%s", rc, path);
        return SXCL_ANDROID_NOT_READABLE;
    }

    if (!android_readable(path)) {
        android_err(reason, reason_len, "存在但打不开(open 返回 Permission denied):%s", path);
        return SXCL_ANDROID_DENIED;
    }

    if (!is_dir && want_exec) {
        const int noexec = (mounts_text != NULL) ? sxcl_android_noexec_in_mounts(mounts_text, path)
                                                  : sxcl_android_mount_noexec(path);
        if (noexec == 1) {
            android_err(reason, reason_len,
                        "在 noexec 挂载上(共享存储 /storage/emulated),有 x 权限也起不了进程:%s",
                        path);
            return SXCL_ANDROID_NOEXEC;
        }
        if (!has_exec) {
            android_err(reason, reason_len, "文件在,但没有执行位(rwx 里没有 x):%s", path);
            return SXCL_ANDROID_NOT_EXECUTABLE;
        }
    }

    android_err(reason, reason_len, "可以正常使用:%s", path);
    return SXCL_ANDROID_OK;
}

sxcl_android_access sxcl_android_probe_path(const char *path, int want_exec,
                                            char *reason, size_t reason_len)
{
    return sxcl_android_probe_path_with_mounts(NULL, path, want_exec, reason, reason_len);
}

/* ────────────────── JRE 侧共享库补齐(FCL RuntimeUtils.patchJava 语义) ────────────────── */

/* 顺序 = sxcl_android_jre_lib_name 的下标语义(共 SXCL_ANDROID_JRE_LIB_COUNT 条),别改。 */
static const char *const kJreLibs[SXCL_ANDROID_JRE_LIB_COUNT] = {
    "libawt_xawt.so",
    "libjsound.so",
};

const char *sxcl_android_jre_lib_name(int index)
{
    if (index < 0 || index >= SXCL_ANDROID_JRE_LIB_COUNT) {
        return NULL;
    }
    return kJreLibs[index];
}

const char *sxcl_android_jre_patch_code_name(int code)
{
    switch (code) {
    case SXCL_ANDROID_JRE_OK:             return "ok";
    case SXCL_ANDROID_JRE_ERR_ARG:        return "arg";
    case SXCL_ANDROID_JRE_ERR_NO_LIB_DIR: return "no_lib_dir";
    case SXCL_ANDROID_JRE_ERR_NO_SOURCE:  return "no_source";
    case SXCL_ANDROID_JRE_ERR_COPY:       return "copy";
    default:                              return "unknown";
    }
}

/* dir + "/" + leaf;装不下返回 -1(与 java_runtime.c 里那份同语义:各文件自带,不抽内部头)。 */
static int jre_join(char *out, size_t out_len, const char *dir, const char *leaf)
{
    const size_t dlen = strlen(dir);
    const size_t llen = strlen(leaf);
    const int need_sep = (dlen > 0 && dir[dlen - 1] == '/') ? 0 : 1;
    if (dlen + (size_t)need_sep + llen + 1 > out_len) {
        return -1;
    }
    (void)memcpy(out, dir, dlen);
    size_t n = dlen;
    if (need_sep) {
        out[n++] = '/';
    }
    (void)memcpy(out + n, leaf, llen + 1);
    return 0;
}

/* 整块拷贝(UTF-8 路径必须走 sxcl_fs_fopen:MSVC 的 fopen 按 ANSI 代码页解释路径)。
 * 返回 0 成功,-1 源打不开,-2 目标写不了 / 读写出错。 */
static int jre_copy_file(const char *src, const char *dst)
{
    FILE *in = sxcl_fs_fopen(src, "rb");
    if (in == NULL) {
        return -1;
    }
    FILE *out = sxcl_fs_fopen(dst, "wb");
    if (out == NULL) {
        (void)fclose(in);
        return -2;
    }
    char buf[64 * 1024];
    size_t n = 0;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -2;
            break;
        }
    }
    if (rc == 0 && ferror(in)) {
        rc = -1;
    }
    if (fclose(out) != 0 && rc == 0) {
        rc = -2; /* fclose 失败 = 数据可能没落盘,不能报成功 */
    }
    (void)fclose(in);
    return rc;
}

int sxcl_android_jre_patch_libs(const char *java_home, const char *native_lib_dir, char *lib_dir_out,
                                size_t lib_dir_len, char *err, size_t err_len)
{
    char jre8_dir[SXCL_ANDROID_JRE_PATH_MAX];
    char plain_dir[SXCL_ANDROID_JRE_PATH_MAX];
    char lib_dir[SXCL_ANDROID_JRE_PATH_MAX];
    char src[SXCL_ANDROID_JRE_PATH_MAX];
    char dst[SXCL_ANDROID_JRE_PATH_MAX];
    int i = 0;

    if (lib_dir_out != NULL && lib_dir_len > 0) {
        lib_dir_out[0] = '\0';
    }
    if (java_home == NULL || java_home[0] == '\0' || native_lib_dir == NULL ||
        native_lib_dir[0] == '\0') {
        android_err(err, err_len, "参数不合法:java_home / native_lib_dir 不能为空");
        return SXCL_ANDROID_JRE_ERR_ARG;
    }

    /* jre8 的库在 <java_home>/jre/lib,其余在 <java_home>/lib(FCL:isJDK8 时前缀 "/jre")。
     * 看哪个目录真的在,而不是去认版本号 —— 我们要的是"文件该放哪",不是"这是哪一代"。 */
    if (jre_join(jre8_dir, sizeof(jre8_dir), java_home, "jre/lib") != 0 ||
        jre_join(plain_dir, sizeof(plain_dir), java_home, "lib") != 0) {
        android_err(err, err_len, "JRE 路径太长:%s", java_home);
        return SXCL_ANDROID_JRE_ERR_ARG;
    }
    if (sxcl_fs_is_dir(jre8_dir)) {
        (void)memcpy(lib_dir, jre8_dir, strlen(jre8_dir) + 1);
    } else if (sxcl_fs_is_dir(plain_dir)) {
        (void)memcpy(lib_dir, plain_dir, strlen(plain_dir) + 1);
    } else {
        android_err(err, err_len, "JRE 里既没有 jre/lib 也没有 lib(java_home 给错了?):%s",
                    java_home);
        return SXCL_ANDROID_JRE_ERR_NO_LIB_DIR;
    }

    /* 先体检两个源:缺一个就是 APK 打包坏了(构建在缺的时候直接失败),必须硬失败。
     * 静默跳过只会把问题推到设备上"JVM 起不来"那一刻,而那时已经看不出是谁的错。 */
    for (i = 0; i < SXCL_ANDROID_JRE_LIB_COUNT; ++i) {
        if (jre_join(src, sizeof(src), native_lib_dir, kJreLibs[i]) != 0) {
            android_err(err, err_len, "路径太长:%s/%s", native_lib_dir, kJreLibs[i]);
            return SXCL_ANDROID_JRE_ERR_ARG;
        }
        if (!sxcl_fs_exists(src)) {
            android_err(err, err_len, "nativeLibraryDir 里没有 %s(APK 打包坏了,它必须随包发布):%s",
                        kJreLibs[i], src);
            return SXCL_ANDROID_JRE_ERR_NO_SOURCE;
        }
    }

    for (i = 0; i < SXCL_ANDROID_JRE_LIB_COUNT; ++i) {
        int64_t src_size = 0;
        int64_t dst_size = 0;
        int rc = 0;
        if (jre_join(src, sizeof(src), native_lib_dir, kJreLibs[i]) != 0 ||
            jre_join(dst, sizeof(dst), lib_dir, kJreLibs[i]) != 0) {
            android_err(err, err_len, "路径太长:%s", lib_dir);
            return SXCL_ANDROID_JRE_ERR_ARG;
        }
        rc = jre_copy_file(src, dst);
        if (rc != 0) {
            android_err(err, err_len, "拷贝 %s 失败(%s -> %s)", kJreLibs[i],
                        (rc == -1) ? "源打不开" : "写不进去", dst);
            return SXCL_ANDROID_JRE_ERR_COPY;
        }
        /* 拷完核对长度:截断的 .so 在设备上表现为 dlopen 失败或直接崩,不值得赌。 */
        if (sxcl_fs_stat(src, &src_size, NULL) != 0 || sxcl_fs_stat(dst, &dst_size, NULL) != 0 ||
            src_size != dst_size) {
            android_err(err, err_len, "拷贝后长度不一致:%s(%lld -> %lld)", kJreLibs[i],
                        (long long)src_size, (long long)dst_size);
            return SXCL_ANDROID_JRE_ERR_COPY;
        }
    }

    if (lib_dir_out != NULL && lib_dir_len > 0) {
        const size_t n = strlen(lib_dir);
        if (n + 1 <= lib_dir_len) {
            (void)memcpy(lib_dir_out, lib_dir, n + 1);
        }
    }
    (void)android_err(err, err_len, "两个 JRE 侧库都在位:%s", lib_dir);
    return SXCL_ANDROID_JRE_OK;
}

