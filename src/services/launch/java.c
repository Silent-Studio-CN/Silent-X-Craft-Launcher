/* Java 运行时探测 —— 扫路径 + 解析 release / java -version 文本 + 按需求排序候选。
 *
 * 为什么以 release 文件为主,而不是跑 "java -version":
 *   - 快:一次 fopen 就能读完,不用为每个候选起一个进程(JVM 冷启动动辄 200ms+);
 *   - 稳:不依赖可执行权限、不依赖目标平台能不能跑(判断安卓/32 位运行时也成立);
 *   - 全:release 里有版本、厂商、OS_ARCH,正好是启动器要展示的三样东西。
 * 只有明确要看"它到底能不能跑起来"的时候才该去执行 java —— 那不属于这一层。
 *
 * 纯文本入口(sxcl_java_parse_release / sxcl_java_parse_version_output)不碰文件系统,
 * 所以单测不需要机器上真的装了 JDK,也不需要往仓库里塞夹具。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/launch.h"

#include "sxcl/android.h" /* 安卓:路径"能不能读/能不能执行"的分类 */
#include "sxcl/fs.h"       /* sxcl_fs_fopen(读可执行文件头判架构;Windows 侧走宽字符) */
#include "sxcl/process.h"  /* sxcl_process_run:真的起一次 <java> -version(见 1c 节) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

/* ────────────────────────── 平台小工具 ────────────────────────── */

sxcl_java_os sxcl_java_current_os(void)
{
#if defined(_WIN32)
    return SXCL_JAVA_OS_WINDOWS;
#elif defined(__ANDROID__)
    return SXCL_JAVA_OS_ANDROID;
#elif defined(__APPLE__)
    return SXCL_JAVA_OS_MACOS;
#else
    return SXCL_JAVA_OS_LINUX;
#endif
}

const char *sxcl_java_os_name(sxcl_java_os os)
{
    switch (os) {
    case SXCL_JAVA_OS_WINDOWS: return "windows";
    case SXCL_JAVA_OS_MACOS:   return "osx";
    case SXCL_JAVA_OS_LINUX:
    case SXCL_JAVA_OS_ANDROID: /* Mojang 的 rules 里没有 android 这个名字,安卓按 linux 匹配 */
    default:                   return "linux";
    }
}

const char *sxcl_java_exe_name(sxcl_java_os os)
{
    return os == SXCL_JAVA_OS_WINDOWS ? "java.exe" : "java";
}

static char os_sep(sxcl_java_os os)
{
    return os == SXCL_JAVA_OS_WINDOWS ? '\\' : '/';
}

#if defined(_WIN32)
static int utf8_to_wide(const char *src, wchar_t *dst, size_t cap)
{
    if (!src || cap == 0) {
        return -1;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap) <= 0) {
        dst[0] = L'\0';
        return -1;
    }
    return 0;
}

static int wide_to_utf8(const wchar_t *src, char *dst, size_t cap)
{
    if (!src || cap == 0) {
        return -1;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)cap, NULL, NULL) <= 0) {
        dst[0] = '\0';
        return -1;
    }
    return 0;
}
#endif

/* ────────────────────────── 字符串/路径小工具 ────────────────────────── */

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    const size_t n = strlen(src);
    const size_t take = (n < cap - 1) ? n : cap - 1;
    memcpy(dst, src, take);
    dst[take] = '\0';
}

static void copy_n(char *dst, size_t cap, const char *src, size_t len)
{
    if (cap == 0) {
        return;
    }
    const size_t take = (len < cap - 1) ? len : cap - 1;
    if (src && take) {
        memcpy(dst, src, take);
    }
    dst[take] = '\0';
}

/** a + sep + b;a 末尾已有的分隔符先去掉,免得出现双斜杠。 */
static void join_path(char *out, size_t cap, const char *a, const char *b, char sep)
{
    size_t n = 0;
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    if (a && *a) {
        n = strlen(a);
        while (n > 0 && (a[n - 1] == '/' || a[n - 1] == '\\')) {
            --n;
        }
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(out, a, n);
        out[n] = '\0';
    }
    if (b && *b) {
        if (n > 0 && n + 1 < cap) {
            out[n++] = sep;
            out[n] = '\0';
        }
        const size_t m = strlen(b);
        const size_t take = (n + m < cap - 1) ? m : (cap - 1 - n);
        memcpy(out + n, b, take);
        out[n + take] = '\0';
    }
}

/** 去掉最后一段(末尾分隔符先剥掉);没有分隔符时输出空串。 */
static void path_parent(const char *path, char *out, size_t cap)
{
    size_t n = path ? strlen(path) : 0;
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\')) {
        --n;
    }
    while (n > 0 && path[n - 1] != '/' && path[n - 1] != '\\') {
        --n;
    }
    while (n > 1 && (path[n - 1] == '/' || path[n - 1] == '\\')) {
        --n;
    }
    copy_n(out, cap, path ? path : "", n);
}

static const char *path_base(const char *path)
{
    const char *base = path ? path : "";
    for (const char *p = base; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

static int ieq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int icontains(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle) {
        return 0;
    }
    const size_t n = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i]) {
            char ca = p[i];
            char cb = needle[i];
            if (ca >= 'A' && ca <= 'Z') {
                ca = (char)(ca - 'A' + 'a');
            }
            if (cb >= 'A' && cb <= 'Z') {
                cb = (char)(cb - 'A' + 'a');
            }
            if (ca != cb) {
                break;
            }
            ++i;
        }
        if (i == n) {
            return 1;
        }
    }
    return 0;
}

/* ────────────────────────── 文件系统 ────────────────────────── */

static int path_is_dir(const char *path)
{
    if (!path || !*path) {
        return 0;
    }
#if defined(_WIN32)
    wchar_t w[1024];
    if (utf8_to_wide(path, w, 1024) != 0) {
        return 0;
    }
    const DWORD attr = GetFileAttributesW(w);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode) ? 1 : 0;
#endif
}

static int path_is_file(const char *path)
{
    if (!path || !*path) {
        return 0;
    }
#if defined(_WIN32)
    wchar_t w[1024];
    if (utf8_to_wide(path, w, 1024) != 0) {
        return 0;
    }
    const DWORD attr = GetFileAttributesW(w);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISREG(st.st_mode) ? 1 : 0;
#endif
}

/** 读整个文本文件到 buf(NUL 结尾);返回长度,失败 -1。 */
static long read_text_file(const char *path, char *buf, size_t cap)
{
    if (cap == 0) {
        return -1;
    }
    buf[0] = '\0';
    FILE *fh = fopen(path, "rb");
    if (!fh) {
        return -1;
    }
    const size_t got = fread(buf, 1, cap - 1, fh);
    buf[got] = '\0';
    fclose(fh);
    return (long)got;
}

/** 目录项回调;返回非 0 = 提前结束枚举。 */
typedef int (*dir_visit_fn)(void *user, const char *name, int is_dir);

static int dir_visit(const char *path, dir_visit_fn fn, void *user)
{
    if (!path || !*path) {
        return -1;
    }
#if defined(_WIN32)
    wchar_t w[1024];
    wchar_t pattern[1200];
    if (utf8_to_wide(path, w, 1024) != 0) {
        return -1;
    }
    if (swprintf(pattern, 1200, L"%ls\\*", w) < 0) {
        return -1;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        char name[SXCL_JAVA_PATH_MAX];
        if (wide_to_utf8(fd.cFileName, name, sizeof(name)) != 0) {
            continue;
        }
        const int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        if (fn(user, name, is_dir)) {
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
#else
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char full[SXCL_JAVA_PATH_MAX * 2];
        join_path(full, sizeof(full), path, ent->d_name, '/');
        const int is_dir = path_is_dir(full) ? 1 : 0;
        if (fn(user, ent->d_name, is_dir)) {
            break;
        }
    }
    closedir(dir);
    return 0;
#endif
}

/* ────────────────────────── 环境变量 ────────────────────────── */

static void get_env_utf8(const char *name, char *out, size_t cap)
{
    out[0] = '\0';
#if defined(_WIN32)
    wchar_t wname[64];
    size_t i = 0;
    for (; name[i] && i + 1 < 64; ++i) {
        wname[i] = (wchar_t)(unsigned char)name[i];
    }
    wname[i] = L'\0';
    const wchar_t *value = _wgetenv(wname); /* 走宽字符:中文用户名/路径不会被代码页毁掉 */
    if (value) {
        (void)wide_to_utf8(value, out, cap);
    }
#else
    const char *value = getenv(name);
    if (value) {
        copy_str(out, cap, value);
    }
#endif
}

void sxcl_java_env_capture(sxcl_java_env_store *store)
{
    if (!store) {
        return;
    }
    memset(store, 0, sizeof(*store));
    get_env_utf8("JAVA_HOME", store->java_home, sizeof(store->java_home));
    get_env_utf8("ProgramFiles", store->program_files, sizeof(store->program_files));
    get_env_utf8("ProgramFiles(x86)", store->program_files_x86, sizeof(store->program_files_x86));
    get_env_utf8("LOCALAPPDATA", store->local_app_data, sizeof(store->local_app_data));
    get_env_utf8("APPDATA", store->app_data, sizeof(store->app_data));
    get_env_utf8("USERPROFILE", store->user_home, sizeof(store->user_home));
    if (store->user_home[0] == '\0') {
        get_env_utf8("HOME", store->user_home, sizeof(store->user_home));
    }
    get_env_utf8("PATH", store->path, sizeof(store->path));
    /* 安卓打包层设置的两个口子:私有 files 目录(唯一能放可执行文件的地方)与共享存储根 */
    get_env_utf8("SXCL_ANDROID_FILES", store->android_files, sizeof(store->android_files));
    get_env_utf8("SXCL_ANDROID_SHARED", store->android_shared, sizeof(store->android_shared));

    store->env.java_home = store->java_home[0] ? store->java_home : NULL;
    store->env.program_files = store->program_files[0] ? store->program_files : NULL;
    store->env.program_files_x86 = store->program_files_x86[0] ? store->program_files_x86 : NULL;
    store->env.local_app_data = store->local_app_data[0] ? store->local_app_data : NULL;
    store->env.app_data = store->app_data[0] ? store->app_data : NULL;
    store->env.user_home = store->user_home[0] ? store->user_home : NULL;
    store->env.path = store->path[0] ? store->path : NULL;
    store->env.android_files = store->android_files[0] ? store->android_files : NULL;
    store->env.android_shared = store->android_shared[0] ? store->android_shared : NULL;
}

/* ────────────────────────── 版本串 → 主版本 ────────────────────────── */

int sxcl_java_major_of(const char *version_text)
{
    int first = 0;
    int second = 0;
    int have_first = 0;
    int have_second = 0;
    const char *p = version_text;
    if (!p) {
        return 0;
    }
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    while (*p >= '0' && *p <= '9') {
        have_first = 1;
        if (first < 100000) {
            first = first * 10 + (*p - '0');
        }
        ++p;
    }
    if (!have_first) {
        return 0;
    }
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            have_second = 1;
            if (second < 100000) {
                second = second * 10 + (*p - '0');
            }
            ++p;
        }
    }
    /* "1.8.0_402" 这种老版式:首段是 1,真正的主版本在第二段 */
    if (first == 1 && have_second) {
        return second;
    }
    return first;
}

/* ────────────────────────── release 文件 ────────────────────────── */

static void strip_trailing(char *value)
{
    size_t n = strlen(value);
    while (n > 0 && (value[n - 1] == ' ' || value[n - 1] == '\t' || value[n - 1] == '\r')) {
        value[--n] = '\0';
    }
    if (n >= 2 && value[0] == '"' && value[n - 1] == '"') {
        memmove(value, value + 1, n - 2);
        value[n - 2] = '\0';
    }
}

/** 在键值清单里找 key(键名大小写敏感:release 的键是固定的全大写)。找到返回 out,否则 NULL。 */
static const char *release_lookup(const char *text, const char *key, char *out, size_t cap)
{
    const size_t key_len = strlen(key);
    const char *line = text;
    if (cap == 0) {
        return NULL;
    }
    out[0] = '\0';
    while (line && *line) {
        const char *eol = line;
        while (*eol && *eol != '\n' && *eol != '\r') {
            ++eol;
        }
        if ((size_t)(eol - line) > key_len && strncmp(line, key, key_len) == 0 &&
            line[key_len] == '=') {
            copy_n(out, cap, line + key_len + 1, (size_t)(eol - line) - key_len - 1);
            strip_trailing(out);
            return out;
        }
        while (*eol == '\r' || *eol == '\n') {
            ++eol;
        }
        line = eol;
    }
    return NULL;
}

static void arch_from_os_arch(const char *os_arch, sxcl_java_info *out)
{
    out->is_64bit = -1;
    out->arch[0] = '\0';
    if (!os_arch || !*os_arch) {
        return;
    }
    copy_str(out->arch, sizeof(out->arch), os_arch);
    if (ieq(os_arch, "amd64") || ieq(os_arch, "x86_64") || ieq(os_arch, "x64")) {
        out->is_64bit = 1;
        copy_str(out->arch, sizeof(out->arch), "x64");
    } else if (ieq(os_arch, "x86") || ieq(os_arch, "i386") || ieq(os_arch, "i586") ||
               ieq(os_arch, "i686")) {
        out->is_64bit = 0;
        copy_str(out->arch, sizeof(out->arch), "x86");
    } else if (ieq(os_arch, "aarch64") || ieq(os_arch, "arm64")) {
        out->is_64bit = 1;
        copy_str(out->arch, sizeof(out->arch), "arm64");
    } else if (ieq(os_arch, "arm") || ieq(os_arch, "armv7l") || ieq(os_arch, "armv6l")) {
        out->is_64bit = 0;
        copy_str(out->arch, sizeof(out->arch), "arm32");
    } else if (ieq(os_arch, "ppc64le") || ieq(os_arch, "ppc64")) {
        out->is_64bit = 1;
        copy_str(out->arch, sizeof(out->arch), "ppc64");
    }
}

int sxcl_java_parse_release(const char *text, size_t len, sxcl_java_info *out)
{
    char saved_path[SXCL_JAVA_PATH_MAX];
    char saved_home[SXCL_JAVA_PATH_MAX];
    char saved_source[24];
    char buf[512];
    if (!out) {
        return -1;
    }
    copy_str(saved_path, sizeof(saved_path), out->path);
    copy_str(saved_home, sizeof(saved_home), out->home);
    copy_str(saved_source, sizeof(saved_source), out->source);

    memset(out, 0, sizeof(*out));
    copy_str(out->path, sizeof(out->path), saved_path);
    copy_str(out->home, sizeof(out->home), saved_home);
    copy_str(out->source, sizeof(out->source), saved_source);
    out->is_64bit = -1;
    out->is_jre = 0;
    copy_str(out->vendor, sizeof(out->vendor), "Unknown");
    copy_str(out->error, sizeof(out->error), "release 里没有 JAVA_VERSION");

    if (!text || len == 0) {
        copy_str(out->error, sizeof(out->error), "release 内容为空");
        return -1;
    }
    if (!release_lookup(text, "JAVA_VERSION", buf, sizeof(buf)) || !buf[0]) {
        return -1;
    }
    copy_str(out->version, sizeof(out->version), buf);
    out->major = sxcl_java_major_of(out->version);
    if (out->major <= 0) {
        copy_str(out->error, sizeof(out->error), "release 的 JAVA_VERSION 认不出主版本");
        return -1;
    }

    /* 厂商:JDK 9+ 用 IMPLEMENTOR;JDK 8 的 release 通常两个都没有(留给 inspect 按路径猜) */
    if (release_lookup(text, "IMPLEMENTOR", buf, sizeof(buf)) && buf[0]) {
        copy_str(out->vendor, sizeof(out->vendor), buf);
    } else if (release_lookup(text, "JAVA_VENDOR", buf, sizeof(buf)) && buf[0]) {
        copy_str(out->vendor, sizeof(out->vendor), buf);
    }

    if (release_lookup(text, "OS_ARCH", buf, sizeof(buf))) {
        arch_from_os_arch(buf, out);
    }

    /* JRE 判定(文本级):jlink 出来的运行时镜像会写 JAVA_RUNTIME_VERSION / JAVA_RUNTIME_NAME,
     * 完整 JDK 的 release 不带这两个键。硬证据在 sxcl_java_inspect:那里能看 bin/javac 在不在。 */
    if ((release_lookup(text, "JAVA_RUNTIME_VERSION", buf, sizeof(buf)) && buf[0]) ||
        (release_lookup(text, "JAVA_RUNTIME_NAME", buf, sizeof(buf)) && buf[0])) {
        out->is_jre = 1;
    }
    out->error[0] = '\0';
    return 0;
}

int sxcl_java_parse_version_output(const char *text, size_t len, sxcl_java_info *out)
{
    char saved_path[SXCL_JAVA_PATH_MAX];
    char saved_home[SXCL_JAVA_PATH_MAX];
    char saved_source[24];
    int found = 0;
    const char *scan = NULL;
    if (!out) {
        return -1;
    }
    copy_str(saved_path, sizeof(saved_path), out->path);
    copy_str(saved_home, sizeof(saved_home), out->home);
    copy_str(saved_source, sizeof(saved_source), out->source);

    memset(out, 0, sizeof(*out));
    copy_str(out->path, sizeof(out->path), saved_path);
    copy_str(out->home, sizeof(out->home), saved_home);
    copy_str(out->source, sizeof(out->source), saved_source);
    out->is_64bit = -1;
    out->is_jre = -1; /* -version 输出分不出 JRE/JDK:JRE 也会打 "Server VM" 那一行 */
    copy_str(out->vendor, sizeof(out->vendor), "Unknown");
    copy_str(out->error, sizeof(out->error), "没找到 version \"...\"");

    if (!text || len == 0) {
        copy_str(out->error, sizeof(out->error), "版本输出为空");
        return -1;
    }
    /* openjdk version "21.0.3" / java version "1.8.0_402" / java version "17" */
    scan = text;
    while ((scan = strstr(scan, "version \"")) != NULL) {
        const char *value = scan + 9;
        const char *end = strchr(value, '"');
        if (end && end > value) {
            copy_n(out->version, sizeof(out->version), value, (size_t)(end - value));
            found = 1;
            break;
        }
        scan = value;
    }
    if (!found) {
        return -1;
    }
    out->major = sxcl_java_major_of(out->version);
    if (out->major <= 0) {
        copy_str(out->error, sizeof(out->error), "版本串认不出主版本");
        return -1;
    }

    if (icontains(text, "temurin") || icontains(text, "adoptium")) {
        copy_str(out->vendor, sizeof(out->vendor), "Eclipse Adoptium");
    } else if (icontains(text, "zulu")) {
        copy_str(out->vendor, sizeof(out->vendor), "Azul Zulu");
    } else if (icontains(text, "microsoft")) {
        copy_str(out->vendor, sizeof(out->vendor), "Microsoft");
    } else if (icontains(text, "corretto") || icontains(text, "amazon")) {
        copy_str(out->vendor, sizeof(out->vendor), "Amazon Corretto");
    } else if (icontains(text, "liberica") || icontains(text, "bellsoft")) {
        copy_str(out->vendor, sizeof(out->vendor), "BellSoft Liberica");
    } else if (icontains(text, "semeru") || icontains(text, "openj9") || icontains(text, "ibm")) {
        copy_str(out->vendor, sizeof(out->vendor), "IBM Semeru");
    } else if (icontains(text, "graalvm")) {
        copy_str(out->vendor, sizeof(out->vendor), "GraalVM");
    } else if (icontains(text, "hotspot") || icontains(text, "java(tm)")) {
        copy_str(out->vendor, sizeof(out->vendor), "Oracle");
    } else if (icontains(text, "openjdk")) {
        copy_str(out->vendor, sizeof(out->vendor), "OpenJDK");
    }

    if (icontains(text, "64-bit") || icontains(text, "64 bit")) {
        out->is_64bit = 1;
    } else if (icontains(text, "32-bit") || icontains(text, "32 bit")) {
        out->is_64bit = 0;
    }
    if (icontains(text, "aarch64") || icontains(text, "arm64")) {
        copy_str(out->arch, sizeof(out->arch), "arm64");
    } else if (out->is_64bit == 1) {
        copy_str(out->arch, sizeof(out->arch), "x64");
    } else if (out->is_64bit == 0) {
        copy_str(out->arch, sizeof(out->arch), "x86");
    }
    out->error[0] = '\0';
    return 0;
}

/* ────────────────────────── 候选路径(纯字符串) ────────────────────────── */

static void push_candidate_owner(sxcl_java_candidate *out, size_t cap, size_t *count,
                                 const char *path, const char *home, const char *source,
                                 const char *owner)
{
    sxcl_java_candidate *slot = NULL;
    if (*count >= cap || !path || !*path) {
        return;
    }
    slot = &out[*count];
    memset(slot, 0, sizeof(*slot));
    copy_str(slot->path, sizeof(slot->path), path);
    if (home && *home) {
        copy_str(slot->home, sizeof(slot->home), home);
    }
    copy_str(slot->source, sizeof(slot->source), source ? source : "");
    copy_str(slot->owner, sizeof(slot->owner), owner ? owner : "");
    ++(*count);
}

/* 不带归属的旧写法(桌面候选都是"这台机器的",不需要 owner) */
static void push_candidate(sxcl_java_candidate *out, size_t cap, size_t *count,
                           const char *path, const char *home, const char *source)
{
    push_candidate_owner(out, cap, count, path, home, source, NULL);
}

size_t sxcl_java_candidate_paths(const sxcl_java_env *env, sxcl_java_os os,
                                 sxcl_java_candidate *out, size_t cap)
{
    sxcl_java_env empty;
    char sep;
    const char *exe = NULL;
    size_t count = 0;
    char tmp[SXCL_JAVA_PATH_MAX];
    char home[SXCL_JAVA_PATH_MAX];
    char full[SXCL_JAVA_PATH_MAX * 2];
    if (!out || cap == 0) {
        return 0;
    }
    if (!env) {
        memset(&empty, 0, sizeof(empty));
        env = &empty;
    }
    sep = os_sep(os);
    exe = sxcl_java_exe_name(os);

    /* 1) JAVA_HOME:最权威,排第一 */
    if (env->java_home && *env->java_home) {
        copy_str(home, sizeof(home), env->java_home);
        join_path(tmp, sizeof(tmp), home, "bin", sep);
        join_path(full, sizeof(full), tmp, exe, sep);
        push_candidate(out, cap, &count, full, home, "JAVA_HOME");
    }

    /* 2) PATH:用户自己配的,排在 JAVA_HOME 之后(顺序即优先级) */
    if (env->path && *env->path) {
        const char *p = env->path;
        while (*p) {
            const char *end = p;
            while (*end && *end != ';' && *end != ':') {
                ++end;
            }
            if (end > p) {
                char up[SXCL_JAVA_PATH_MAX];
                copy_n(tmp, sizeof(tmp), p, (size_t)(end - p));
                join_path(full, sizeof(full), tmp, exe, sep);
                path_parent(tmp, up, sizeof(up));
                push_candidate(out, cap, &count, full, up, "PATH");
            }
            while (*end == ';' || *end == ':') {
                ++end;
            }
            p = end;
        }
    }

    /* 3) 平台固定位置(不用扫目录就能算出来的可执行文件) */
    switch (os) {
    case SXCL_JAVA_OS_MACOS:
        push_candidate(out, cap, &count, "/usr/bin/java", "/usr", "SystemJava");
        /* Homebrew:Apple Silicon 在 /opt/homebrew,Intel 在 /usr/local */
        push_candidate(out, cap, &count, "/opt/homebrew/opt/openjdk/bin/java",
                       "/opt/homebrew/opt/openjdk", "Homebrew");
        push_candidate(out, cap, &count, "/usr/local/opt/openjdk/bin/java",
                       "/usr/local/opt/openjdk", "Homebrew");
        break;
    case SXCL_JAVA_OS_ANDROID:
        /* 安卓没有"系统 JDK":运行时要么由 JAVA_HOME 指过来,要么是启动器下到私有目录的
         * (PojavLauncher 系是 files/runtime/<component>/bin/java,由扫描根覆盖)。 */
        push_candidate(out, cap, &count, "/system/bin/java", "", "SystemJava");
        break;
    case SXCL_JAVA_OS_LINUX:
        push_candidate(out, cap, &count, "/usr/bin/java", "/usr", "SystemJava");
        break;
    case SXCL_JAVA_OS_WINDOWS:
    default:
        /* Windows 上可信入口只有 JAVA_HOME / PATH;Program Files 那堆交给真实扫描 */
        break;
    }
    return count;
}

typedef struct scan_root {
    char path[SXCL_JAVA_PATH_MAX];
    char source[24];
} scan_root;

static void push_root(scan_root *out, size_t cap, size_t *count, const char *path,
                      const char *source)
{
    if (*count >= cap || !path || !*path) {
        return;
    }
    copy_str(out[*count].path, sizeof(out[*count].path), path);
    copy_str(out[*count].source, sizeof(out[*count].source), source);
    ++(*count);
}

/** 需要真实扫目录的根(带来源标记)。Windows 上列的是"厂商目录",不是单个 JDK 目录。 */
static size_t collect_scan_roots(const sxcl_java_env *env, sxcl_java_os os,
                                 scan_root *out, size_t cap)
{
    sxcl_java_env empty;
    size_t count = 0;
    char tmp[SXCL_JAVA_PATH_MAX];
    if (!out || cap == 0) {
        return 0;
    }
    if (!env) {
        memset(&empty, 0, sizeof(empty));
        env = &empty;
    }

    if (os == SXCL_JAVA_OS_WINDOWS) {
        const char *roots_win[2];
        const char *vendors[] = { "Java", "Eclipse Adoptium", "Temurin", "Microsoft", "Zulu",
                                  "BellSoft", "Amazon Corretto", "IBM", "Semeru", "JetBrains",
                                  "OpenJDK" };
        size_t r = 0;
        size_t v = 0;
        roots_win[0] = env->program_files;
        roots_win[1] = env->program_files_x86;
        for (r = 0; r < 2; ++r) {
            if (!roots_win[r] || !*roots_win[r]) {
                continue;
            }
            for (v = 0; v < sizeof(vendors) / sizeof(vendors[0]); ++v) {
                join_path(tmp, sizeof(tmp), roots_win[r], vendors[v], '\\');
                push_root(out, cap, &count, tmp, r == 0 ? "ProgramFiles" : "ProgramFiles(x86)");
            }
        }
        if (env->local_app_data && *env->local_app_data) {
            join_path(tmp, sizeof(tmp), env->local_app_data, "Programs\\Eclipse Adoptium", '\\');
            push_root(out, cap, &count, tmp, "LocalAppData");
            join_path(tmp, sizeof(tmp), env->local_app_data, "Programs\\Microsoft", '\\');
            push_root(out, cap, &count, tmp, "LocalAppData");
            join_path(tmp, sizeof(tmp), env->local_app_data, "Programs\\Zulu", '\\');
            push_root(out, cap, &count, tmp, "LocalAppData");
        }
        /* 官方运行时(Mojang java-runtime-*):启动器自己下到配置目录里 */
        if (env->app_data && *env->app_data) {
            join_path(tmp, sizeof(tmp), env->app_data, "SilentXCraftLauncher\\runtime", '\\');
            push_root(out, cap, &count, tmp, "MojangRuntime");
            join_path(tmp, sizeof(tmp), env->app_data, ".minecraft\\runtime", '\\');
            push_root(out, cap, &count, tmp, "MojangRuntime");
        }
    } else if (os == SXCL_JAVA_OS_LINUX) {
        push_root(out, cap, &count, "/usr/lib/jvm", "JvmDir");
        push_root(out, cap, &count, "/usr/lib64/jvm", "JvmDir");
        push_root(out, cap, &count, "/usr/local/lib/jvm", "JvmDir");
        if (env->user_home && *env->user_home) {
            join_path(tmp, sizeof(tmp), env->user_home, ".sdkman/candidates/java", '/');
            push_root(out, cap, &count, tmp, "Sdkman");
        }
    } else if (os == SXCL_JAVA_OS_MACOS) {
        push_root(out, cap, &count, "/Library/Java/JavaVirtualMachines", "JvmDir");
        push_root(out, cap, &count, "/System/Library/Java/JavaVirtualMachines", "JvmDir");
        if (env->user_home && *env->user_home) {
            join_path(tmp, sizeof(tmp), env->user_home, "Library/Java/JavaVirtualMachines", '/');
            push_root(out, cap, &count, tmp, "JvmDir");
            join_path(tmp, sizeof(tmp), env->user_home, ".sdkman/candidates/java", '/');
            push_root(out, cap, &count, tmp, "Sdkman");
        }
    } else { /* Android */
        /* 安卓上**唯一**能放可执行 Java 的地方是本应用私有目录(见 sxcl/android.h):
         *   <files>/runtime/<任意>/bin/java  —— 我们自己装的(见 sxcl/java_runtime.h);
         *   <files>/jre、<files>/java        —— 用户手工拷进来时的常见命名;
         *   <data>/app_runtime/java/<任意>   —— FCL 的风格,照抄一份,用户从别处拷过来就能用。
         * 共享存储(/storage/emulated)是 noexec,**不当扫描根**:扫到了也起不来。
         * 但它会被 sxcl_java_probe_android 列出来并如实标注 NOEXEC —— 用户有权知道。
         *
         * 修过的 bug:老代码拼的是 <user_home>/files/runtime。安卓打包层把 HOME 直接设成
         * files 目录,于是实际拼成了 <files>/files/runtime —— 永远扫不到东西。 */
        const char *base = env->android_files;
        if (base && *base) {
            static const char *const own[] = { "runtime", "jre", "java" };
            size_t k = 0;
            for (k = 0; k < sizeof(own) / sizeof(own[0]); ++k) {
                join_path(tmp, sizeof(tmp), base, own[k], '/');
                push_root(out, cap, &count, tmp, "AndroidPrivate");
            }
            {
                char up[SXCL_JAVA_PATH_MAX];
                path_parent(base, up, sizeof(up));
                if (up[0] != '\0') {
                    join_path(tmp, sizeof(tmp), up, "app_runtime/java", '/');
                    push_root(out, cap, &count, tmp, "AndroidPrivate");
                }
            }
        }
        /* HOME 在安卓上就是 files 目录,这里等于再兜一遍;重复的根无害 ——
         * sxcl_java_discover 最后按 home 去重。 */
        if (env->user_home && *env->user_home) {
            join_path(tmp, sizeof(tmp), env->user_home, "runtime", '/');
            push_root(out, cap, &count, tmp, "Runtime");
            join_path(tmp, sizeof(tmp), env->user_home, "jre", '/');
            push_root(out, cap, &count, tmp, "Runtime");
        }
    }
    return count;
}

size_t sxcl_java_scan_roots(const sxcl_java_env *env, sxcl_java_os os,
                            char (*out)[SXCL_JAVA_PATH_MAX], size_t cap)
{
    scan_root tmp[48];
    size_t n = 0;
    size_t take = 0;
    size_t i = 0;
    if (!out || cap == 0) {
        return 0;
    }
    n = collect_scan_roots(env, os, tmp, 48);
    take = (n < cap) ? n : cap;
    for (i = 0; i < take; ++i) {
        copy_str(out[i], SXCL_JAVA_PATH_MAX, tmp[i].path);
    }
    return take;
}

/* ────────────────────────── 厂商兜底(按路径猜) ────────────────────────── */

static void vendor_from_path(const char *path, char *out, size_t cap)
{
    if (icontains(path, "adoptium") || icontains(path, "temurin")) {
        copy_str(out, cap, "Eclipse Adoptium");
    } else if (icontains(path, "zulu")) {
        copy_str(out, cap, "Azul Zulu");
    } else if (icontains(path, "microsoft")) {
        copy_str(out, cap, "Microsoft");
    } else if (icontains(path, "corretto") || icontains(path, "amazon")) {
        copy_str(out, cap, "Amazon Corretto");
    } else if (icontains(path, "liberica") || icontains(path, "bellsoft")) {
        copy_str(out, cap, "BellSoft Liberica");
    } else if (icontains(path, "semeru") || icontains(path, "openj9") || icontains(path, "ibm")) {
        copy_str(out, cap, "IBM Semeru");
    } else if (icontains(path, "jetbrains")) {
        copy_str(out, cap, "JetBrains");
    } else if (icontains(path, "graalvm")) {
        copy_str(out, cap, "GraalVM");
    } else if (icontains(path, "java-runtime") || icontains(path, "jre-legacy")) {
        copy_str(out, cap, "Mojang");
    } else if (icontains(path, "jdk") || icontains(path, "jre") || icontains(path, "jvm")) {
        copy_str(out, cap, "OpenJDK");
    }
}

/* ────────────────────────── 单个安装点:读 release(不执行 java) ────────────────────────── */

int sxcl_java_inspect(const char *java_exe_or_home, sxcl_java_info *out)
{
    sxcl_java_os host;
    char sep;
    char exe[SXCL_JAVA_PATH_MAX];
    char home[SXCL_JAVA_PATH_MAX];
    char parent[SXCL_JAVA_PATH_MAX];
    char bin[SXCL_JAVA_PATH_MAX];
    char release_path[SXCL_JAVA_PATH_MAX * 2];
    char text[4096];

    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->is_64bit = -1;
    out->is_jre = -1;
    copy_str(out->vendor, sizeof(out->vendor), "Unknown");
    if (!java_exe_or_home || !*java_exe_or_home) {
        copy_str(out->error, sizeof(out->error), "路径为空");
        return -1;
    }

    host = sxcl_java_current_os();
    sep = os_sep(host);

    if (path_is_dir(java_exe_or_home)) {
        copy_str(home, sizeof(home), java_exe_or_home);
        join_path(bin, sizeof(bin), home, "bin", sep);
        join_path(exe, sizeof(exe), bin, sxcl_java_exe_name(host), sep);
    } else {
        copy_str(exe, sizeof(exe), java_exe_or_home);
        path_parent(exe, parent, sizeof(parent));
        if (ieq(path_base(parent), "bin")) {
            path_parent(parent, home, sizeof(home));
        } else {
            copy_str(home, sizeof(home), parent);
        }
    }

    join_path(release_path, sizeof(release_path), home, "release", sep);
    if (read_text_file(release_path, text, sizeof(text)) < 0) {
        char up[SXCL_JAVA_PATH_MAX];
        path_parent(home, up, sizeof(up));
        join_path(release_path, sizeof(release_path), up, "release", sep);
        if (read_text_file(release_path, text, sizeof(text)) < 0) {
            copy_str(out->path, sizeof(out->path), exe);
            copy_str(out->home, sizeof(out->home), home);
            copy_str(out->error, sizeof(out->error), "找不到 release 文件");
            return -1;
        }
        copy_str(home, sizeof(home), up);
    }

    copy_str(out->path, sizeof(out->path), exe);
    copy_str(out->home, sizeof(out->home), home);
    if (sxcl_java_parse_release(text, strlen(text), out) != 0) {
        return -1;
    }
    copy_str(out->path, sizeof(out->path), exe);
    copy_str(out->home, sizeof(out->home), home);

    /* JRE 裁决:bin/javac 在不在才是硬证据(release 文本只是提示) */
    join_path(bin, sizeof(bin), home, "bin", sep);
    join_path(exe, sizeof(exe), bin, host == SXCL_JAVA_OS_WINDOWS ? "javac.exe" : "javac", sep);
    out->is_jre = path_is_file(exe) ? 0 : 1;

    if (ieq(out->vendor, "Unknown")) {
        char guessed[64];
        guessed[0] = '\0';
        vendor_from_path(home, guessed, sizeof(guessed));
        if (guessed[0]) {
            copy_str(out->vendor, sizeof(out->vendor), guessed);
        }
    }
    copy_str(out->source, sizeof(out->source), "Scan");
    return 0;
}

/* ────────────────────────── 真实扫目录 ────────────────────────── */

typedef struct scan_state {
    sxcl_java_info *out;
    size_t cap;
    size_t count;
    const char *source;
    sxcl_java_os os;
} scan_state;

static void add_info(scan_state *st, const sxcl_java_info *info)
{
    if (st->count >= st->cap) {
        return;
    }
    st->out[st->count] = *info;
    if (st->source && st->source[0]) {
        copy_str(st->out[st->count].source, sizeof(st->out[st->count].source), st->source);
    }
    ++st->count;
}

/** dir 本身是不是 java home(bin/java 在)?是就收下并返回 1。 */
static int try_java_home(scan_state *st, const char *dir)
{
    char bin[SXCL_JAVA_PATH_MAX];
    char exe[SXCL_JAVA_PATH_MAX];
    sxcl_java_info info;
    join_path(bin, sizeof(bin), dir, "bin", os_sep(st->os));
    join_path(exe, sizeof(exe), bin, sxcl_java_exe_name(st->os), os_sep(st->os));
    if (!path_is_file(exe)) {
        return 0;
    }
    memset(&info, 0, sizeof(info));
    if (sxcl_java_inspect(exe, &info) == 0) {
        add_info(st, &info);
    }
    return 1; /* 已经是 java home 了,不必再往子目录找(jre/ 只是冗余镜像) */
}

static void walk_root(scan_state *st, int depth, const char *root);

typedef struct walk_ctx {
    scan_state *st;
    const char *dir;
    int depth;
} walk_ctx;

static int walk_cb(void *user, const char *name, int is_dir)
{
    walk_ctx *wc = (walk_ctx *)user;
    char full[SXCL_JAVA_PATH_MAX];
    if (!is_dir) {
        return 0;
    }
    join_path(full, sizeof(full), wc->dir, name, os_sep(wc->st->os));
    walk_root(wc->st, wc->depth, full);
    return 0;
}

/** 从 root 往下最多 depth 层找 bin/java。root 很深时(macOS 的 .jdk/Contents/Home)靠 depth 覆盖。 */
static void walk_root(scan_state *st, int depth, const char *root)
{
    walk_ctx wc;
    if (!path_is_dir(root)) {
        return;
    }
    if (try_java_home(st, root)) {
        return;
    }
    if (depth <= 0) {
        return;
    }
    wc.st = st;
    wc.dir = root;
    wc.depth = depth - 1;
    (void)dir_visit(root, walk_cb, &wc);
}

/* ────────────────────────── 发现:候选 + 扫描根,去重 ────────────────────────── */

static int same_path(const char *a, const char *b)
{
#if defined(_WIN32)
    return ieq(a, b); /* Windows 路径大小写不敏感 */
#else
    return strcmp(a, b) == 0;
#endif
}

static size_t dedupe_infos(sxcl_java_info *list, size_t count)
{
    size_t kept = 0;
    size_t i = 0;
    size_t j = 0;
    for (i = 0; i < count; ++i) {
        int dup = 0;
        for (j = 0; j < kept; ++j) {
            if (same_path(list[j].path, list[i].path) || same_path(list[j].home, list[i].home)) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            if (kept != i) {
                list[kept] = list[i];
            }
            ++kept;
        }
    }
    return kept;
}

size_t sxcl_java_discover(const sxcl_java_env *env, sxcl_java_os os,
                          sxcl_java_info *out, size_t cap)
{
    sxcl_java_env_store store;
    sxcl_java_candidate cands[64];
    scan_root roots[48];
    scan_state st;
    size_t n = 0;
    size_t rn = 0;
    size_t i = 0;

    if (!out || cap == 0) {
        return 0;
    }
    if (!env) {
        sxcl_java_env_capture(&store);
        env = &store.env;
    }
    st.out = out;
    st.cap = cap;
    st.count = 0;
    st.source = NULL;
    st.os = os;

    /* 1) 固定候选(JAVA_HOME / PATH / 平台固定位置) */
    n = sxcl_java_candidate_paths(env, os, cands, 64);
    for (i = 0; i < n; ++i) {
        sxcl_java_info info;
        if (!path_is_file(cands[i].path)) {
            continue;
        }
        memset(&info, 0, sizeof(info));
        if (sxcl_java_inspect(cands[i].path, &info) != 0) {
            continue;
        }
        copy_str(info.source, sizeof(info.source), cands[i].source);
        add_info(&st, &info);
    }

    /* 2) 扫描根(每个根最多往下 3 层:覆盖 macOS 的 .jdk/Contents/Home 与 Windows 的 <厂商>\<JDK>\jre) */
    rn = collect_scan_roots(env, os, roots, 48);
    for (i = 0; i < rn; ++i) {
        st.source = roots[i].source;
        walk_root(&st, 3, roots[i].path);
    }
    st.source = NULL;

    return dedupe_infos(out, st.count);
}

/* ────────────────────────── 需求与排序 ────────────────────────── */

int sxcl_java_required_major(const sxcl_json *version_json)
{
    const sxcl_json_value *root = NULL;
    const sxcl_json_value *java_version = NULL;
    int64_t major = 0;
    if (!version_json) {
        return 8;
    }
    root = sxcl_json_root(version_json);
    java_version = sxcl_json_get(root, "javaVersion");
    if (!java_version) {
        return 8; /* 1.13 之前没有这个字段,官方启动器一律用 Java 8 */
    }
    major = sxcl_json_get_int64(java_version, "majorVersion", 0);
    if (major <= 0 || major > 1000) {
        return 8;
    }
    return (int)major;
}

typedef struct rank_item {
    size_t idx;
    int group;
    int sort_a;
    int is64;
    const char *path;
} rank_item;

static int rank_cmp(const void *lhs, const void *rhs)
{
    const rank_item *x = (const rank_item *)lhs;
    const rank_item *y = (const rank_item *)rhs;
    int c = 0;
    if (x->group != y->group) {
        return x->group < y->group ? -1 : 1;
    }
    if (x->sort_a != y->sort_a) {
        return x->sort_a < y->sort_a ? -1 : 1;
    }
    if (x->is64 != y->is64) {
        return x->is64 > y->is64 ? -1 : 1; /* 64 位优先 */
    }
    c = strcmp(x->path, y->path);
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    if (x->idx != y->idx) {
        return x->idx < y->idx ? -1 : 1;
    }
    return 0;
}

size_t sxcl_java_rank(const sxcl_java_info *list, size_t count, int required_major,
                      size_t *order, size_t cap)
{
    rank_item items[64];
    size_t n = 0;
    size_t i = 0;
    size_t take = 0;
    if (!list || !order || cap == 0) {
        return 0;
    }
    if (count > 64) {
        count = 64;
    }
    for (i = 0; i < count; ++i) {
        const int major = list[i].major;
        rank_item *it = &items[n];
        if (major <= 0) {
            it->group = 3; /* 版本未知的垫底:可能是个坏的安装 */
            it->sort_a = 0;
        } else if (required_major <= 0) {
            it->group = 0; /* 无要求:从新到旧 */
            it->sort_a = -major;
        } else if (major == required_major) {
            it->group = 0;
            it->sort_a = 0;
        } else if (major > required_major) {
            it->group = 1;
            it->sort_a = major; /* 升序:够用的里面挑最小的,越接近需求越保险 */
        } else {
            it->group = 2;
            it->sort_a = -major; /* 降序:不够用的里面挑最大的,离需求最近 */
        }
        it->idx = i;
        it->is64 = list[i].is_64bit;
        it->path = list[i].path;
        ++n;
    }
    qsort(items, n, sizeof(items[0]), rank_cmp);
    take = (n < cap) ? n : cap;
    for (i = 0; i < take; ++i) {
        order[i] = items[i].idx;
    }
    return take;
}

/* ══════════════════════ 1b. Android:为什么检不出 Java(可读的结论) ══════════════════════
 *
 * 用户反馈(192.168.220.33,Android 16,平板):"我平板有 HMCL 不可能没有 JAVA……
 * 成功检出游戏,但是没检出 JAVA"。
 * 硬证据(设备实测,docs/08 第 14 节有原文):FCL 的 Java 在
 *   /data/user/0/com.tungsten.fcl/app_runtime/java/jre25/bin/java
 * —— 那是**别的应用**的私有目录:
 *   * 我们 stat 它 -> EACCES(沙箱),所以老代码的 path_is_file() 直接返回 0,静默跳过;
 *   * 共享存储上的同理不可执行(/storage/emulated 挂载带 noexec)。
 * 结论:别的启动器的 Java **用不了**。本组函数把它变成产品行为 —— 不再是"没检出",
 * 而是"检出了、但安卓不允许用",并给出原因与下一步。
 */

static const char *const kJavaVerdictNames[] = {
    "可用", "不在", "沙箱拒绝", "共享存储不能执行", "没有执行位", "不是 JRE", "读不了",
};

static const char *const kJavaVerdictKeys[] = {
    "usable", "missing", "denied", "noexec", "not_executable", "not_a_jre", "unreadable",
};

static const char *java_verdict_pick(const char *const *table, size_t n, sxcl_java_verdict verdict,
                                     const char *fallback)
{
    const int i = (int)verdict;
    if (i < 0 || (size_t)i >= n) {
        return fallback;
    }
    return table[i];
}

const char *sxcl_java_verdict_name(sxcl_java_verdict verdict)
{
    return java_verdict_pick(kJavaVerdictNames,
                             sizeof(kJavaVerdictNames) / sizeof(kJavaVerdictNames[0]), verdict,
                             "未知");
}

const char *sxcl_java_verdict_key(sxcl_java_verdict verdict)
{
    return java_verdict_pick(kJavaVerdictKeys,
                             sizeof(kJavaVerdictKeys) / sizeof(kJavaVerdictKeys[0]), verdict,
                             "unknown");
}

const char *sxcl_java_verdict_hint(sxcl_java_verdict verdict)
{
    switch (verdict) {
    case SXCL_JAVA_VERDICT_USABLE:
        return "这份 Java 可以直接用来启动游戏。";
    case SXCL_JAVA_VERDICT_MISSING:
        return "这个位置没有 Java;如果刚装过,请确认装到了这里。";
    case SXCL_JAVA_VERDICT_DENIED:
        return "这是别的启动器(HMCL/FCL/PojavLauncher)装在它自己私有目录里的 Java。"
               "安卓不允许一个应用读另一个应用的私有目录,所以我们看不到也用不了。"
               "请在本应用里装一份自己的 Java(设置 - Java 运行路径 - 下载 Java)。";
    case SXCL_JAVA_VERDICT_NOEXEC:
        return "这份 Java 在共享存储(内部存储 / sdcard)上,而共享存储是 noexec 挂载,"
               "里面的程序起不来。Java 必须放在应用私有目录里。";
    case SXCL_JAVA_VERDICT_NOT_EXECUTABLE:
        return "文件在,但没有执行位(解压/拷贝时丢了 x 权限);重新解压一份到应用私有目录即可。";
    case SXCL_JAVA_VERDICT_NOT_A_JRE:
        return "这里没有可用的 Java 运行时(缺 bin/java 或读不出 release);"
               "请在本应用里装一份自己的 Java。";
    case SXCL_JAVA_VERDICT_UNREADABLE:
        return "读这个路径时出错;可能是权限问题或存储已卸载。";
    case SXCL_JAVA_VERDICT_COUNT:
    default:
        return "未知情况。";
    }
}

static sxcl_java_verdict verdict_of_access(sxcl_android_access access)
{
    switch (access) {
    case SXCL_ANDROID_OK:             return SXCL_JAVA_VERDICT_USABLE;
    case SXCL_ANDROID_MISSING:        return SXCL_JAVA_VERDICT_MISSING;
    case SXCL_ANDROID_DENIED:         return SXCL_JAVA_VERDICT_DENIED;
    case SXCL_ANDROID_NOEXEC:         return SXCL_JAVA_VERDICT_NOEXEC;
    case SXCL_ANDROID_NOT_EXECUTABLE: return SXCL_JAVA_VERDICT_NOT_EXECUTABLE;
    case SXCL_ANDROID_NOT_READABLE:   return SXCL_JAVA_VERDICT_UNREADABLE;
    case SXCL_ANDROID_ACCESS_COUNT:
    default:                          return SXCL_JAVA_VERDICT_MISSING;
    }
}

/** 体检一条候选并追加到报告。want_exec=1 时要求能执行(目录会跳过这步)。 */
static void java_probe_add(sxcl_java_report *out, const char *path, const char *home,
                           const char *source, const char *owner, sxcl_java_os os,
                           const char *mounts_text, int want_exec)
{
    sxcl_java_probe *probe = NULL;
    sxcl_java_info info;
    char detail[192];
    sxcl_android_access access;
    (void)os; /* 可执行文件名由候选路径本身决定,这里不需要再分平台 */
    if (out->count >= SXCL_JAVA_MAX_PROBES || path == NULL || path[0] == '\0') {
        return;
    }
    probe = &out->items[out->count];
    (void)memset(probe, 0, sizeof(*probe));
    copy_str(probe->path, sizeof(probe->path), path);
    if (home && *home) {
        copy_str(probe->home, sizeof(probe->home), home);
    }
    copy_str(probe->source, sizeof(probe->source), source ? source : "");
    copy_str(probe->owner, sizeof(probe->owner), owner ? owner : "");
    detail[0] = '\0';
    access = sxcl_android_probe_path_with_mounts(mounts_text, path, want_exec, detail,
                                                 sizeof(detail));
    probe->verdict = verdict_of_access(access);
    if (probe->verdict == SXCL_JAVA_VERDICT_USABLE) {
        (void)memset(&info, 0, sizeof(info));
        if (sxcl_java_inspect(path, &info) == 0 && info.major > 0) {
            probe->major = info.major;
            copy_str(probe->version, sizeof(probe->version), info.version);
            if (probe->home[0] == '\0' && info.home[0] != '\0') {
                copy_str(probe->home, sizeof(probe->home), info.home);
            }
            ++out->usable;
        } else {
            probe->verdict = SXCL_JAVA_VERDICT_NOT_A_JRE;
            if (path_is_dir(path)) {
                (void)snprintf(detail, sizeof(detail),
                               "目录在,但里面没有 bin/java(还没装 Java):%s", path);
            } else {
                (void)snprintf(detail, sizeof(detail),
                               "有文件但读不出 JRE 画像(缺 release 或不是 Java):%s", path);
            }
        }
    }
    copy_str(probe->reason, sizeof(probe->reason), detail);
    ++out->count;
}

size_t sxcl_java_probe_candidates(const sxcl_java_candidate *cands, size_t cand_count,
                                  sxcl_java_os os, const char *mounts_text, sxcl_java_report *out)
{
    size_t i = 0;
    if (out == NULL) {
        return 0;
    }
    (void)memset(out, 0, sizeof(*out));
    if (cands == NULL) {
        return 0;
    }
    for (i = 0; i < cand_count && out->count < SXCL_JAVA_MAX_PROBES; ++i) {
        java_probe_add(out, cands[i].path, cands[i].home, cands[i].source, cands[i].owner, os,
                       mounts_text, 1);
    }
    return out->count;
}

/* ── 安卓上"已知会放 Java"的地方 ──
 * 别的启动器那一组必然读不到(沙箱),但**要列出来** —— 用户的疑问是
 * "我明明装了 HMCL,怎么会没有 Java",答案恰恰是"有,但不能用"。
 * 其中 FCL 的两条是**实测**出来的路径(设备 /sdcard/FCL/log 里的原文),不是猜的。 */
typedef struct android_java_root {
    const char *path;  /* 绝对路径,或相对共享存储根的路径 */
    const char *owner;
} android_java_root;

static const android_java_root kForeignJavaRoots[] = {
    { "/data/data/com.tungsten.fcl/app_runtime/java", "FCL" },
    { "/data/data/com.tungsten.fcl/app_runtime/java/jre25/bin/java", "FCL" },
    { "/data/data/com.tungsten.fcl/app_runtime/java/jre8/bin/java", "FCL" },
    { "/data/data/com.tungsten.fcl/files/runtime", "FCL" },
    { "/data/data/org.jackhuang.hmcl/files/runtime", "HMCL" },
    { "/data/data/org.jackhuang.hmcl/app_runtime/java", "HMCL" },
    { "/data/data/net.kdt.pojavlaunch/files/runtime", "PojavLauncher" },
    { "/data/data/net.kdt.pojavlaunch/files/runtime/jre17/bin/java", "PojavLauncher" },
};

static const android_java_root kSharedJavaRoots[] = {
    { "Android/data/com.tungsten.fcl/files/runtime", "FCL" },
    { "FCL/runtime", "FCL" },
    { "Android/data/org.jackhuang.hmcl/files/runtime", "HMCL" },
};

/* 在 <root> 下最多 depth 层找 bin/java。找到填 exe/home 并返回 1。 */
typedef struct own_java_find {
    char exe[SXCL_JAVA_PATH_MAX];
    char home[SXCL_JAVA_PATH_MAX];
    int found;
} own_java_find;

static void own_java_walk(own_java_find *find, const char *dir, int depth);

typedef struct own_walk_ctx {
    own_java_find *find;
    const char *dir;
    int depth;
} own_walk_ctx;

static int own_walk_cb(void *user, const char *name, int is_dir)
{
    own_walk_ctx *ctx = (own_walk_ctx *)user;
    char full[SXCL_JAVA_PATH_MAX];
    if (!is_dir || ctx->find->found) {
        return ctx->find->found; /* 非 0 = 提前结束枚举 */
    }
    join_path(full, sizeof(full), ctx->dir, name, '/');
    own_java_walk(ctx->find, full, ctx->depth);
    return ctx->find->found;
}

static void own_java_walk(own_java_find *find, const char *dir, int depth)
{
    char bin[SXCL_JAVA_PATH_MAX];
    char exe[SXCL_JAVA_PATH_MAX];
    own_walk_ctx ctx;
    if (find->found || !path_is_dir(dir)) {
        return;
    }
    join_path(bin, sizeof(bin), dir, "bin", '/');
    join_path(exe, sizeof(exe), bin, "java", '/');
    if (path_is_file(exe)) {
        copy_str(find->exe, sizeof(find->exe), exe);
        copy_str(find->home, sizeof(find->home), dir);
        find->found = 1;
        return;
    }
    if (depth <= 0) {
        return;
    }
    ctx.find = find;
    ctx.dir = dir;
    ctx.depth = depth - 1;
    (void)dir_visit(dir, own_walk_cb, &ctx);
}

size_t sxcl_java_probe_android(const char *files_dir, const char *shared_root,
                               sxcl_java_report *out)
{
    char shared[SXCL_JAVA_PATH_MAX];
    size_t i = 0;
    if (out == NULL) {
        return 0;
    }
    (void)memset(out, 0, sizeof(*out));

    /* 1) 本应用私有目录:唯一可能真的能用的一类,真的往下扫 bin/java */
    if (files_dir != NULL && files_dir[0] != '\0') {
        static const char *const own[] = { "runtime", "jre", "java" };
        for (i = 0; i < sizeof(own) / sizeof(own[0]); ++i) {
            char root[SXCL_JAVA_PATH_MAX];
            own_java_find find;
            join_path(root, sizeof(root), files_dir, own[i], '/');
            (void)memset(&find, 0, sizeof(find));
            own_java_walk(&find, root, 3);
            if (find.found) {
                java_probe_add(out, find.exe, find.home, "AndroidPrivate", "本应用",
                               SXCL_JAVA_OS_ANDROID, NULL, 1);
            } else {
                java_probe_add(out, root, "", "AndroidPrivate", "本应用", SXCL_JAVA_OS_ANDROID,
                               NULL, 0);
            }
        }
        {
            /* FCL 风格的 app_runtime/java:本应用也照抄一份,用户从别处拷进来就能用 */
            char up[SXCL_JAVA_PATH_MAX];
            char root[SXCL_JAVA_PATH_MAX];
            own_java_find find;
            path_parent(files_dir, up, sizeof(up));
            if (up[0] != '\0') {
                join_path(root, sizeof(root), up, "app_runtime/java", '/');
                (void)memset(&find, 0, sizeof(find));
                own_java_walk(&find, root, 3);
                if (find.found) {
                    java_probe_add(out, find.exe, find.home, "AndroidPrivate", "本应用",
                                   SXCL_JAVA_OS_ANDROID, NULL, 1);
                } else {
                    java_probe_add(out, root, "", "AndroidPrivate", "本应用",
                                   SXCL_JAVA_OS_ANDROID, NULL, 0);
                }
            }
        }
    } else {
        /* 打包层没给 files 目录时也要有一条,不能整块静默消失 */
        java_probe_add(out, "/data/data/com.silentstudio.sxcl/files/runtime", "",
                       "AndroidPrivate", "本应用", SXCL_JAVA_OS_ANDROID, NULL, 0);
    }

    /* 2) 别的启动器:一定读不到,但要如实列出来并说清"是沙箱,不是没有" */
    for (i = 0; i < sizeof(kForeignJavaRoots) / sizeof(kForeignJavaRoots[0]); ++i) {
        java_probe_add(out, kForeignJavaRoots[i].path, "", "AndroidForeign",
                       kForeignJavaRoots[i].owner, SXCL_JAVA_OS_ANDROID, NULL, 1);
    }

    /* 3) 共享存储:一定 noexec —— 用户自己拷到 sdcard 上的那份也救不了 */
    if (shared_root != NULL && shared_root[0] != '\0') {
        copy_str(shared, sizeof(shared), shared_root);
    } else {
        copy_str(shared, sizeof(shared), "/storage/emulated/0");
    }
    for (i = 0; i < sizeof(kSharedJavaRoots) / sizeof(kSharedJavaRoots[0]); ++i) {
        char path[SXCL_JAVA_PATH_MAX];
        join_path(path, sizeof(path), shared, kSharedJavaRoots[i].path, '/');
        java_probe_add(out, path, "", "AndroidShared", kSharedJavaRoots[i].owner,
                       SXCL_JAVA_OS_ANDROID, NULL, 1);
    }
    return out->count;
}

/* ══════════════ 1c. 真实执行 `java -version`(②"不能只看目录名") ══════════════
 *
 * 上面那一层(sxcl_java_inspect / sxcl_java_discover)读的是 <home>/release 这份**文本**;
 * 文本可以是从别处拷来的、可以是 32 位换成 64 位忘了改的、也可能目录名写着 jdk-21 而里面
 * 是另一个版本。要回答"这个二进制在这台机器上到底能不能跑、跑起来是哪个版本",唯一的办法
 * 就是**真的起一次进程**。本节做这件事,并把"起不来"的原因分类带出来(不静默丢弃):
 *   沙箱拒绝 / 共享存储 noexec / 没有执行位 / 跑不起来 / 不是 Java / 架构不符。
 *
 * 架构不符**靠读文件头判**(ELF/PE/Mach-O),不等进程起不来才发现 —— 起不来的错误信息在
 * 安卓上往往只是一句 "can't execute",看不出是架构问题。
 */

const char *sxcl_java_host_arch(void)
{
#if defined(_WIN32)
#  if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#  elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#  else
    return "x64";
#  endif
#elif defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__arm__)
    return "arm32";
#elif defined(__i386__)
    return "x86";
#else
    return "x64";
#endif
}

/** 某个架构能不能在这台机器上跑(向后兼容:64 位机器能跑同族的 32 位)。
 *  返回 1 = 能,0 = 不能(架构不符),-1 = 信息不足(不判)。 */
static int host_arch_accepts(const char *arch)
{
    const char *host = sxcl_java_host_arch();
    if (arch == NULL || arch[0] == '\0') {
        return -1;
    }
    if (strcmp(arch, host) == 0) {
        return 1;
    }
    if (strcmp(host, "x64") == 0 && strcmp(arch, "x86") == 0) {
        return 1;
    }
    if (strcmp(host, "arm64") == 0 && strcmp(arch, "arm32") == 0) {
        return 1;
    }
    return 0;
}

int sxcl_java_binary_arch(const char *path, char *out, size_t out_len)
{
    /* 512 字节:PE 的 IMAGE_NT_HEADERS 在 e_lfanew 处,真实文件的 e_lfanew 常在 0x80~0x100,
     * 只读 64 字节会判不出 PE 的机器码(实测踩过)。 */
    unsigned char head[512];
    FILE *fh = NULL;
    size_t got = 0;
    if (out != NULL && out_len > 0) {
        out[0] = '\0';
    }
    if (path == NULL || path[0] == '\0' || out == NULL || out_len == 0) {
        return -1;
    }
    fh = sxcl_fs_fopen(path, "rb");
    if (fh == NULL) {
        return -1;
    }
    got = fread(head, 1, sizeof(head), fh);
    (void)fclose(fh);
    if (got < 20) {
        return -1;
    }
    if (head[0] == 0x7Fu && head[1] == 'E' && head[2] == 'L' && head[3] == 'F') {
        /* ELF:e_machine 在 0x12,字节序由 EI_DATA(head[5])决定 */
        const int le = (head[5] == 1);
        const unsigned int machine = le ? ((unsigned int)head[18] | ((unsigned int)head[19] << 8))
                                        : ((unsigned int)head[19] | ((unsigned int)head[18] << 8));
        switch (machine) {
        case 0x3E: copy_str(out, out_len, "x64"); return 0;
        case 0x03: copy_str(out, out_len, "x86"); return 0;
        case 0xB7: copy_str(out, out_len, "arm64"); return 0;
        case 0x28: copy_str(out, out_len, "arm32"); return 0;
        default: return -1; /* 别的机器码:认不出就不判 */
        }
    }
    if (head[0] == 'M' && head[1] == 'Z' && got >= 64) {
        /* PE:"PE\0\0" 之后 2 字节是 Machine;e_lfanew 在偏移 0x3C */
        const unsigned long off = (unsigned long)head[60] | ((unsigned long)head[61] << 8) |
                                  ((unsigned long)head[62] << 16) | ((unsigned long)head[63] << 24);
        if (off + 6 > got) {
            return -1; /* 头不在读到的这一小段里:交给执行结果判,别猜 */
        }
        const unsigned int machine =
            (unsigned int)head[off + 4] | ((unsigned int)head[off + 5] << 8);
        switch (machine) {
        case 0x8664: copy_str(out, out_len, "x64"); return 0;
        case 0x014C: copy_str(out, out_len, "x86"); return 0;
        case 0xAA64: copy_str(out, out_len, "arm64"); return 0;
        case 0x01C4: copy_str(out, out_len, "arm32"); return 0;
        default: return -1;
        }
    }
    /* Mach-O:64 位小端(0xFEEDFACF)/ 64 位大端(arm64);universal(fat)认不出 -> -1(不判不符) */
    if (head[0] == 0xCF && head[1] == 0xFA && head[2] == 0xED && head[3] == 0xFE) {
        copy_str(out, out_len, "x64");
        return 0;
    }
    if (head[0] == 0xFE && head[1] == 0xED && head[2] == 0xFA && head[3] == 0xCF) {
        copy_str(out, out_len, "arm64");
        return 0;
    }
    return -1;
}

/* ── 执行并收集输出(java -version 打在 stderr 上,两条都收) ── */

typedef struct java_version_capture {
    char text[8192];
    size_t len;
    int lines;
} java_version_capture;

static int java_version_on_line(void *userdata, int is_stderr, const char *line)
{
    java_version_capture *cap = (java_version_capture *)userdata;
    (void)is_stderr;
    if (cap == NULL || line == NULL) {
        return 0;
    }
    ++cap->lines;
    const size_t n = strlen(line);
    if (cap->len + n + 2 <= sizeof(cap->text)) {
        (void)memcpy(cap->text + cap->len, line, n);
        cap->len += n;
        cap->text[cap->len++] = '\n';
        cap->text[cap->len] = '\0';
    }
    return 0; /* 永远不中断:java -version 就那么几行 */
}

int sxcl_java_exec_version(const char *java_exe, int timeout_ms, sxcl_java_info *out)
{
    sxcl_java_os host;
    char exe[SXCL_JAVA_PATH_MAX];
    char parent[SXCL_JAVA_PATH_MAX];
    char home[SXCL_JAVA_PATH_MAX];
    java_version_capture cap;
    sxcl_process_opts opts;
    sxcl_process_result res;
    const char *args[2];
    int rc = 0;

    if (out == NULL) {
        return -1;
    }
    (void)memset(out, 0, sizeof(*out));
    out->is_64bit = -1;
    out->is_jre = -1;
    copy_str(out->vendor, sizeof(out->vendor), "Unknown");
    if (java_exe == NULL || java_exe[0] == '\0') {
        copy_str(out->error, sizeof(out->error), "路径为空");
        return -1;
    }
    host = sxcl_java_current_os();

    /* 允许直接给 JAVA_HOME:目录 -> <home>/bin/java[.exe] */
    if (path_is_dir(java_exe)) {
        char bin[SXCL_JAVA_PATH_MAX];
        join_path(bin, sizeof(bin), java_exe, "bin", os_sep(host));
        join_path(exe, sizeof(exe), bin, sxcl_java_exe_name(host), os_sep(host));
        copy_str(home, sizeof(home), java_exe);
    } else {
        copy_str(exe, sizeof(exe), java_exe);
        path_parent(exe, parent, sizeof(parent));
        if (ieq(path_base(parent), "bin")) {
            path_parent(parent, home, sizeof(home));
        } else {
            copy_str(home, sizeof(home), parent);
        }
    }
    if (!path_is_file(exe)) {
        copy_str(out->path, sizeof(out->path), exe);
        copy_str(out->home, sizeof(out->home), home);
        copy_str(out->error, sizeof(out->error), "找不到这个可执行文件");
        return -1;
    }

    (void)memset(&cap, 0, sizeof(cap));
    (void)memset(&opts, 0, sizeof(opts));
    (void)memset(&res, 0, sizeof(res));
    args[0] = "-version";
    args[1] = NULL;
    opts.program = exe;
    opts.args = args;
    opts.timeout_ms = (timeout_ms > 0) ? timeout_ms : 8000;
    opts.on_line = java_version_on_line;
    opts.userdata = &cap;
    rc = sxcl_process_run(&opts, &res);

    copy_str(out->path, sizeof(out->path), exe);
    copy_str(out->home, sizeof(out->home), home);
    if (rc != 0) {
        (void)snprintf(out->error, sizeof(out->error), "起不了进程: %s",
                       (res.error[0] != '\0') ? res.error : "未知原因");
        return -1;
    }
    if (res.timed_out) {
        (void)snprintf(out->error, sizeof(out->error), "执行超时(%.1f 秒)",
                       (double)opts.timeout_ms / 1000.0);
        return -1;
    }
    if (cap.len == 0) {
        if (res.exit_code == 127) {
            copy_str(out->error, sizeof(out->error),
                     "进程起不来(退出码 127:多半是没有执行位、noexec 挂载或架构不符)");
        } else {
            (void)snprintf(out->error, sizeof(out->error), "没有输出(退出码 %d)", res.exit_code);
        }
        return -1;
    }
    if (sxcl_java_parse_version_output(cap.text, cap.len, out) != 0) {
        return -1;
    }
    copy_str(out->path, sizeof(out->path), exe);
    copy_str(out->home, sizeof(out->home), home);
    if (out->arch[0] == '\0') {
        char arch[16];
        if (sxcl_java_binary_arch(exe, arch, sizeof(arch)) == 0 && arch[0] != '\0') {
            copy_str(out->arch, sizeof(out->arch), arch);
        }
    }
    /* 执行成功 = 真的能跑:JRE 判定用 bin/javac 在不在(release 文本只作提示) */
    {
        char bin[SXCL_JAVA_PATH_MAX];
        char javac[SXCL_JAVA_PATH_MAX];
        join_path(bin, sizeof(bin), home, "bin", os_sep(host));
        join_path(javac, sizeof(javac), bin, (host == SXCL_JAVA_OS_WINDOWS) ? "javac.exe" : "javac",
                  os_sep(host));
        out->is_jre = path_is_file(javac) ? 0 : 1;
    }
    return 0;
}

/* ── 候选收集(候选表 + 扫描根,带来源与归属)── */

#define SXCL_JAVA_PRIO_OWN       60 /* 本应用私有目录(安卓上唯一能用的一类)或 Runtime */
#define SXCL_JAVA_PRIO_RUNTIME   55 /* 官方运行时(我们自己下到配置目录里的 JRE) */
#define SXCL_JAVA_PRIO_JAVA_HOME 50
#define SXCL_JAVA_PRIO_PATH      40
#define SXCL_JAVA_PRIO_SCAN      20

typedef struct java_exe_list {
    sxcl_java_candidate items[80];
    size_t count;
} java_exe_list;

static void java_exe_push(java_exe_list *list, const char *path, const char *home,
                          const char *source, const char *owner)
{
    if (list == NULL || list->count >= sizeof(list->items) / sizeof(list->items[0])) {
        return;
    }
    if (path == NULL || path[0] == '\0') {
        return;
    }
    {
        sxcl_java_candidate *slot = &list->items[list->count];
        (void)memset(slot, 0, sizeof(*slot));
        copy_str(slot->path, sizeof(slot->path), path);
        copy_str(slot->home, sizeof(slot->home), home ? home : "");
        copy_str(slot->source, sizeof(slot->source), source ? source : "");
        copy_str(slot->owner, sizeof(slot->owner), owner ? owner : "");
        ++list->count;
    }
}

typedef struct java_exe_scan {
    java_exe_list *list;
    sxcl_java_os os;
    const char *source;
    const char *owner;
} java_exe_scan;

static void java_exe_walk(java_exe_scan *sc, int depth, const char *dir);

/* dir_visit 只给"名字",所以每次递归都新起一个上下文 —— 与上面的 walk_root/walk_cb
 * 是同一套写法。 */
typedef struct java_exe_wctx {
    java_exe_scan *sc;
    const char *dir;
    int depth;
} java_exe_wctx;

static int java_exe_wcb(void *user, const char *name, int is_dir)
{
    java_exe_wctx *ctx = (java_exe_wctx *)user;
    char full[SXCL_JAVA_PATH_MAX];
    if (!is_dir) {
        return 0;
    }
    join_path(full, sizeof(full), ctx->dir, name, os_sep(ctx->sc->os));
    java_exe_walk(ctx->sc, ctx->depth, full);
    return 0;
}

/** 在 <dir> 下最多 depth 层找 bin/java;找到就收下(不再往它里面找,与 try_java_home 同口径)。 */
static void java_exe_walk(java_exe_scan *sc, int depth, const char *dir)
{
    char bin[SXCL_JAVA_PATH_MAX];
    char exe[SXCL_JAVA_PATH_MAX];
    java_exe_wctx ctx;
    if (!path_is_dir(dir) || sc->list->count >= 80) {
        return;
    }
    join_path(bin, sizeof(bin), dir, "bin", os_sep(sc->os));
    join_path(exe, sizeof(exe), bin, sxcl_java_exe_name(sc->os), os_sep(sc->os));
    if (path_is_file(exe)) {
        java_exe_push(sc->list, exe, dir, sc->source, sc->owner);
        return;
    }
    if (depth <= 0) {
        return;
    }
    ctx.sc = sc;
    ctx.dir = dir;
    ctx.depth = depth - 1;
    (void)dir_visit(dir, java_exe_wcb, &ctx);
}

static void java_collect_candidates(const sxcl_java_env *env, sxcl_java_os os, java_exe_list *list)
{
    sxcl_java_candidate cands[64];
    scan_root roots[48];
    size_t n = 0;
    size_t rn = 0;
    size_t i = 0;
    (void)memset(list, 0, sizeof(*list));

    n = sxcl_java_candidate_paths(env, os, cands, 64);
    for (i = 0; i < n; ++i) {
        java_exe_push(list, cands[i].path, cands[i].home, cands[i].source, cands[i].owner);
    }

    rn = collect_scan_roots(env, os, roots, 48);
    for (i = 0; i < rn; ++i) {
        java_exe_scan sc;
        sc.list = list;
        sc.os = os;
        sc.source = roots[i].source;
        sc.owner = (strcmp(roots[i].source, "AndroidPrivate") == 0) ? "本应用" : "";
        java_exe_walk(&sc, 3, roots[i].path);
    }
}

static int java_source_priority(const char *source)
{
    if (source == NULL) {
        return SXCL_JAVA_PRIO_SCAN;
    }
    if (strcmp(source, "AndroidPrivate") == 0 || strcmp(source, "Runtime") == 0) {
        return SXCL_JAVA_PRIO_OWN;
    }
    if (strcmp(source, "MojangRuntime") == 0) {
        return SXCL_JAVA_PRIO_RUNTIME;
    }
    if (strcmp(source, "JAVA_HOME") == 0) {
        return SXCL_JAVA_PRIO_JAVA_HOME;
    }
    if (strcmp(source, "PATH") == 0) {
        return SXCL_JAVA_PRIO_PATH;
    }
    return SXCL_JAVA_PRIO_SCAN;
}

/* ── 结论文案 ── */

static const char *const kJavaRunNames[] = {
    "可用", "不在", "沙箱拒绝", "共享存储不能执行", "没有执行位", "跑不起来", "不是 Java", "架构不符",
};

static const char *const kJavaRunKeys[] = {
    "ok", "missing", "denied", "noexec", "not_executable", "exec_failed", "not_a_jre",
    "arch_mismatch",
};

const char *sxcl_java_run_verdict_name(sxcl_java_run_verdict verdict)
{
    const int i = (int)verdict;
    if (i < 0 || (size_t)i >= sizeof(kJavaRunNames) / sizeof(kJavaRunNames[0])) {
        return "未知";
    }
    return kJavaRunNames[i];
}

const char *sxcl_java_run_verdict_key(sxcl_java_run_verdict verdict)
{
    const int i = (int)verdict;
    if (i < 0 || (size_t)i >= sizeof(kJavaRunKeys) / sizeof(kJavaRunKeys[0])) {
        return "unknown";
    }
    return kJavaRunKeys[i];
}

const char *sxcl_java_run_verdict_hint(sxcl_java_run_verdict verdict)
{
    switch (verdict) {
    case SXCL_JAVA_RUN_OK:
        return "这份 Java 已经实测跑起来过,可以直接用来启动游戏。";
    case SXCL_JAVA_RUN_MISSING:
        return "这个位置没有 Java;如果刚装过,请确认装到了这里。";
    case SXCL_JAVA_RUN_DENIED:
        return "这是别的启动器(HMCL/FCL/PojavLauncher)装在它自己私有目录里的 Java。"
               "安卓不允许一个应用读另一个应用的私有目录,所以检测得到也用不了。"
               "请在本应用里装一份自己的 Java(设置 - Java - 下载 Java)。";
    case SXCL_JAVA_RUN_NOEXEC:
        return "这份 Java 在共享存储(内部存储 /sdcard)上,而共享存储是 noexec 挂载,"
               "里面的程序起不来。请用「下载 Java」装到应用私有目录。";
    case SXCL_JAVA_RUN_NOT_EXECUTABLE:
        return "文件在,但没有执行位(解压/拷贝时丢了 x 权限);用「下载 Java」重装一份即可。";
    case SXCL_JAVA_RUN_EXEC_FAILED:
        return "这份 Java 没能跑起来(进程起不来或超时)。请改用「下载 Java」装一份匹配本机"
               "架构的运行时。";
    case SXCL_JAVA_RUN_NOT_A_JRE:
        return "这个文件能执行,但输出里没有 Java 版本 —— 它不是一个 Java 运行时"
               "(可能是别的程序,或者文件损坏)。";
    case SXCL_JAVA_RUN_ARCH_MISMATCH:
        return "这份 Java 的机器码与本机架构不符(例如 arm64 设备上放了 x86 的 java),起不来。"
               "请下载与本机架构匹配的运行时。";
    case SXCL_JAVA_RUN_COUNT:
    default:
        return "未知情况。";
    }
}

static sxcl_java_run_verdict run_verdict_of_access(sxcl_android_access access)
{
    switch (access) {
    case SXCL_ANDROID_OK:             return SXCL_JAVA_RUN_OK;
    case SXCL_ANDROID_MISSING:        return SXCL_JAVA_RUN_MISSING;
    case SXCL_ANDROID_DENIED:         return SXCL_JAVA_RUN_DENIED;
    case SXCL_ANDROID_NOEXEC:         return SXCL_JAVA_RUN_NOEXEC;
    case SXCL_ANDROID_NOT_EXECUTABLE: return SXCL_JAVA_RUN_NOT_EXECUTABLE;
    case SXCL_ANDROID_NOT_READABLE:   return SXCL_JAVA_RUN_EXEC_FAILED;
    case SXCL_ANDROID_ACCESS_COUNT:
    default:                          return SXCL_JAVA_RUN_MISSING;
    }
}

static int java_install_dup(const sxcl_java_installations *out, const char *exe, const char *home)
{
    size_t i = 0;
    for (i = 0; i < out->count; ++i) {
        if (out->items[i].info.path[0] != '\0' && same_path(out->items[i].info.path, exe)) {
            return 1;
        }
        if (home != NULL && home[0] != '\0' && out->items[i].info.home[0] != '\0' &&
            same_path(out->items[i].info.home, home)) {
            return 1;
        }
    }
    return 0;
}

size_t sxcl_java_detect(const sxcl_java_env *env, sxcl_java_os os, int timeout_ms,
                        sxcl_java_installations *out)
{
    sxcl_java_env_store store;
    java_exe_list *list = NULL;
    size_t i = 0;
    if (out == NULL) {
        return 0;
    }
    (void)memset(out, 0, sizeof(*out));
    if (!env) {
        sxcl_java_env_capture(&store);
        env = &store.env;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 8000;
    }
    list = (java_exe_list *)calloc(1, sizeof(*list));
    if (!list) {
        return 0;
    }
    java_collect_candidates(env, os, list);

    for (i = 0; i < list->count && out->count < SXCL_JAVA_MAX_INSTALLS; ++i) {
        const sxcl_java_candidate *cand = &list->items[i];
        sxcl_java_installation *item = &out->items[out->count];
        char detail[192];
        char arch[16];
        sxcl_android_access access;
        (void)memset(item, 0, sizeof(*item));
        item->info.is_64bit = -1;
        item->info.is_jre = -1;
        copy_str(item->info.vendor, sizeof(item->info.vendor), "Unknown");
        item->priority = java_source_priority(cand->source);
        copy_str(item->source, sizeof(item->source), cand->source);
        copy_str(item->owner, sizeof(item->owner), cand->owner);

        if (java_install_dup(out, cand->path, cand->home)) {
            continue;
        }

        detail[0] = '\0';
        access = sxcl_android_probe_path(cand->path, 1, detail, sizeof(detail));
        item->verdict = run_verdict_of_access(access);
        if (item->verdict != SXCL_JAVA_RUN_OK) {
            /* 只读体检就判"用不了":别去起进程(安卓上那会是一个注定失败的 200ms) */
            copy_str(item->info.path, sizeof(item->info.path), cand->path);
            copy_str(item->info.home, sizeof(item->info.home), cand->home);
            (void)snprintf(item->reason, sizeof(item->reason), "%s", detail);
            ++out->count;
            continue;
        }

        /* 架构预判:读文件头,不等进程起不来才发现 */
        arch[0] = '\0';
        (void)sxcl_java_binary_arch(cand->path, arch, sizeof(arch));
        if (arch[0] != '\0' && host_arch_accepts(arch) == 0) {
            item->verdict = SXCL_JAVA_RUN_ARCH_MISMATCH;
            copy_str(item->info.path, sizeof(item->info.path), cand->path);
            copy_str(item->info.home, sizeof(item->info.home), cand->home);
            copy_str(item->info.arch, sizeof(item->info.arch), arch);
            (void)snprintf(item->reason, sizeof(item->reason),
                           "可执行文件的机器码是 %s,本机是 %s:%s", arch, sxcl_java_host_arch(),
                           cand->path);
            ++out->count;
            ++out->broken;
            continue;
        }

        /* 真的执行一次 */
        {
            sxcl_java_info info;
            (void)memset(&info, 0, sizeof(info));
            if (sxcl_java_exec_version(cand->path, timeout_ms, &info) == 0 && info.major > 0) {
                item->verdict = SXCL_JAVA_RUN_OK;
                item->executed = 1;
                item->info = info;
                copy_str(item->info.source, sizeof(item->info.source), cand->source);
                if (item->info.home[0] == '\0') {
                    copy_str(item->info.home, sizeof(item->info.home), cand->home);
                }
                (void)snprintf(item->reason, sizeof(item->reason),
                               "实测 java -version 成功:Java %s(%s,%s 位)%s", info.version,
                               info.vendor,
                               (info.is_64bit == 1) ? "64" : (info.is_64bit == 0 ? "32" : "?"),
                               (info.is_jre == 1) ? ",JRE" : (info.is_jre == 0 ? ",JDK" : ""));
                ++out->usable;
            } else {
                item->verdict = SXCL_JAVA_RUN_EXEC_FAILED;
                if (info.error[0] != '\0' && strstr(info.error, "没找到 version") != NULL) {
                    item->verdict = SXCL_JAVA_RUN_NOT_A_JRE;
                }
                copy_str(item->info.path, sizeof(item->info.path), cand->path);
                copy_str(item->info.home, sizeof(item->info.home),
                         (info.home[0] != '\0') ? info.home : cand->home);
                (void)snprintf(item->reason, sizeof(item->reason), "%s:%s",
                               (info.error[0] != '\0') ? info.error : "实测失败", cand->path);
                ++out->broken;
            }
        }
        ++out->count;
    }
    free(list);

    /* 排序:来源优先级 -> 主版本(降序)-> 路径(升序),结果稳定可复现 */
    for (i = 1; i < out->count; ++i) {
        sxcl_java_installation key = out->items[i];
        size_t j = i;
        while (j > 0) {
            const sxcl_java_installation *prev = &out->items[j - 1];
            int swap = 0;
            if (prev->priority < key.priority) {
                swap = 1;
            } else if (prev->priority == key.priority && prev->info.major < key.info.major) {
                swap = 1;
            } else if (prev->priority == key.priority && prev->info.major == key.info.major &&
                       strcmp(prev->info.path, key.info.path) > 0) {
                swap = 1;
            }
            if (!swap) {
                break;
            }
            out->items[j] = *prev;
            --j;
        }
        out->items[j] = key;
    }
    return out->count;
}



