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
    case SXCL_ANDROID_JRE_ERR_SHIM_MISMATCH: return "shim_mismatch";
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

/** 从 <java_home>/release 里读一行 KEY="VALUE"(只读;没有返回 -1)。 */
static int android_release_value(const char *java_home, const char *key, char *out, size_t out_len)
{
    char path[SXCL_ANDROID_JRE_PATH_MAX];
    char line[512];
    const size_t key_len = strlen(key);
    FILE *fp;
    if (jre_join(path, sizeof(path), java_home, "release") != 0) {
        return -1;
    }
    fp = sxcl_fs_fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t n = strlen(line);
        const char *v;
        size_t vn;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') {
            continue;
        }
        v = line + key_len + 1;
        if (*v == '"') {
            ++v;
        }
        vn = strlen(v);
        if (vn > 0 && v[vn - 1] == '"') {
            --vn;
        }
        if (vn >= out_len) {
            vn = out_len - 1;
        }
        memcpy(out, v, vn);
        out[vn] = '\0';
        (void)fclose(fp);
        return 0;
    }
    (void)fclose(fp);
    return -1;
}

/** <java_home>/<rel> 是不是目录(rel 是相对路径;纯读盘)。 */
static int android_rel_is_dir(const char *java_home, const char *rel)
{
    char full[SXCL_ANDROID_JRE_PATH_MAX];
    if (jre_join(full, sizeof(full), java_home, rel) != 0) {
        return 0;
    }
    return sxcl_fs_is_dir(full);
}

/** <java_home>/<rel> 是不是存在的文件。 */
static int android_rel_exists(const char *java_home, const char *rel)
{
    char full[SXCL_ANDROID_JRE_PATH_MAX];
    if (jre_join(full, sizeof(full), java_home, rel) != 0) {
        return 0;
    }
    return sxcl_fs_exists(full);
}

int sxcl_android_jre_shim_dir(const char *java_home, char *out, size_t out_len, char *err,
                              size_t err_len)
{
    char rel[SXCL_ANDROID_JRE_PATH_MAX];
    char arch[64];
    char full[SXCL_ANDROID_JRE_PATH_MAX];

    if (out != NULL && out_len > 0) {
        out[0] = '\0';
    }
    if (java_home == NULL || java_home[0] == '\0' || out == NULL || out_len == 0) {
        android_err(err, err_len, "参数不合法:java_home / out 不能为空");
        return SXCL_ANDROID_JRE_ERR_ARG;
    }

    /* 路径本身就装不下 -> 参数错(而不是"找不到库目录"):这两件事的处置完全不同,
     * 报错必须分得开(实测:超长 java_home 被报成 no_lib_dir,运维会去查目录结构,查错方向)。 */
    if (jre_join(full, sizeof(full), java_home, "lib") != 0) {
        android_err(err, err_len, "JRE 路径太长:%s", java_home);
        return SXCL_ANDROID_JRE_ERR_ARG;
    }

    arch[0] = '\0';
    (void)android_release_value(java_home, "OS_ARCH", arch, sizeof(arch));

    /* 规则见头文件:①<home>/lib/<OS_ARCH> ②真 JDK8 的 <home>/jre/lib[/<OS_ARCH>] ③<home>/lib
     * ④<home>/jre/lib。①优先是因为 Termux 那份 JRE 镜像布局就是 lib/aarch64(jre8 也是),
     * 而 JVM 的 sun.boot.library.path 指的就是它。
     * 注意:绝对路径一律用 full 缓冲**从相对路径拼**,绝不要拿一个缓冲同时当 leaf 与 out ——
     * jre_join 的 out 与 leaf 是同一个缓冲区时会把内容自己覆盖掉(实测过)。 */
    rel[0] = '\0';
    if (arch[0] != '\0') {
        snprintf(full, sizeof(full), "lib/%s", arch);
        if (android_rel_is_dir(java_home, full)) {
            snprintf(rel, sizeof(rel), "lib/%s", arch);
        }
    }
    if (rel[0] == '\0') {
        /* release 里没有 OS_ARCH 时的兜底:只认**众所周知的架构名**(不猜、不乱扫),
         * 顺序固定。Termux 那份有 OS_ARCH,走的是上面那条。 */
        static const char *const kArchs[] = {"aarch64", "arm64", "arm", "x86_64", "x64",
                                             "i686", "x86"};
        size_t a;
        for (a = 0; a < sizeof(kArchs) / sizeof(kArchs[0]) && rel[0] == '\0'; ++a) {
            snprintf(full, sizeof(full), "lib/%s", kArchs[a]);
            if (android_rel_is_dir(java_home, full)) {
                snprintf(rel, sizeof(rel), "lib/%s", kArchs[a]);
            }
        }
    }
    if (rel[0] == '\0') {
        const int has_jre = android_rel_is_dir(java_home, "jre");
        const int has_javac = android_rel_exists(java_home, "bin/javac");
        if (has_jre && has_javac) { /* FCL 的 isJDK8():jre/ 与 bin/javac 同时存在 */
            if (arch[0] != '\0') {
                snprintf(full, sizeof(full), "jre/lib/%s", arch);
                if (android_rel_is_dir(java_home, full)) {
                    snprintf(rel, sizeof(rel), "jre/lib/%s", arch);
                }
            }
            if (rel[0] == '\0' && android_rel_is_dir(java_home, "jre/lib")) {
                snprintf(rel, sizeof(rel), "jre/lib");
            }
        }
    }
    if (rel[0] == '\0' && android_rel_is_dir(java_home, "lib")) {
        snprintf(rel, sizeof(rel), "lib");
    }
    if (rel[0] == '\0' && android_rel_is_dir(java_home, "jre/lib")) {
        snprintf(rel, sizeof(rel), "jre/lib");
    }
    if (rel[0] == '\0') {
        android_err(err, err_len,
                    "JRE 里找不到库目录(找过 lib/<OS_ARCH>、jre/lib、lib;java_home 给错了?):%s",
                    java_home);
        return SXCL_ANDROID_JRE_ERR_NO_LIB_DIR;
    }
    if (strlen(rel) + 1 > out_len) {
        android_err(err, err_len, "库目录相对路径装不下: %s", rel);
        return SXCL_ANDROID_JRE_ERR_ARG;
    }
    memcpy(out, rel, strlen(rel) + 1);
    return SXCL_ANDROID_JRE_OK;
}

int sxcl_android_jre_patch_libs_ex(const char *java_home, const char *native_lib_dir,
                                   const char *expected_shim_dir, char *lib_dir_out,
                                   size_t lib_dir_len, char *err, size_t err_len)
{
    char rel[SXCL_ANDROID_JRE_PATH_MAX];
    char lib_dir[SXCL_ANDROID_JRE_PATH_MAX];
    char src[SXCL_ANDROID_JRE_PATH_MAX];
    char dst[SXCL_ANDROID_JRE_PATH_MAX];
    int i = 0;
    int rc;

    if (lib_dir_out != NULL && lib_dir_len > 0) {
        lib_dir_out[0] = '\0';
    }
    if (java_home == NULL || java_home[0] == '\0' || native_lib_dir == NULL ||
        native_lib_dir[0] == '\0') {
        android_err(err, err_len, "参数不合法:java_home / native_lib_dir 不能为空");
        return SXCL_ANDROID_JRE_ERR_ARG;
    }

    rc = sxcl_android_jre_shim_dir(java_home, rel, sizeof(rel), err, err_len);
    if (rc != SXCL_ANDROID_JRE_OK) {
        return rc;
    }
    /* 清单里说了 shim_dir 就必须与盘上算出来的一致:不一致 = 有一边是错的,
     * 这时候"挑一个用"就是赌(赌错 = 设备上一片 dlopen 失败)。 */
    if (expected_shim_dir != NULL && expected_shim_dir[0] != '\0' &&
        strcmp(expected_shim_dir, rel) != 0) {
        android_err(err, err_len,
                    "清单说库目录是 %s,盘上算出来是 %s(两处不一致,拒绝把 .so 放进一个 JVM 不看的目录)",
                    expected_shim_dir, rel);
        return SXCL_ANDROID_JRE_ERR_SHIM_MISMATCH;
    }
    if (jre_join(lib_dir, sizeof(lib_dir), java_home, rel) != 0) {
        android_err(err, err_len, "库目录路径太长:%s/%s", java_home, rel);
        return SXCL_ANDROID_JRE_ERR_ARG;
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
        int copy_rc = 0;
        if (jre_join(src, sizeof(src), native_lib_dir, kJreLibs[i]) != 0 ||
            jre_join(dst, sizeof(dst), lib_dir, kJreLibs[i]) != 0) {
            android_err(err, err_len, "路径太长:%s", lib_dir);
            return SXCL_ANDROID_JRE_ERR_ARG;
        }
        copy_rc = jre_copy_file(src, dst);
        if (copy_rc != 0) {
            android_err(err, err_len, "拷贝 %s 失败(%s -> %s)", kJreLibs[i],
                        (copy_rc == -1) ? "源打不开" : "写不进去", dst);
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

int sxcl_android_jre_patch_libs(const char *java_home, const char *native_lib_dir, char *lib_dir_out,
                                size_t lib_dir_len, char *err, size_t err_len)
{
    /* 不校验 shim_dir(官方清单那条线没有这个字段);语义与 _ex 完全一致。 */
    return sxcl_android_jre_patch_libs_ex(java_home, native_lib_dir, NULL, lib_dir_out, lib_dir_len,
                                          err, err_len);
}

