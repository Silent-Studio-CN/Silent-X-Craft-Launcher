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

    store->env.java_home = store->java_home[0] ? store->java_home : NULL;
    store->env.program_files = store->program_files[0] ? store->program_files : NULL;
    store->env.program_files_x86 = store->program_files_x86[0] ? store->program_files_x86 : NULL;
    store->env.local_app_data = store->local_app_data[0] ? store->local_app_data : NULL;
    store->env.app_data = store->app_data[0] ? store->app_data : NULL;
    store->env.user_home = store->user_home[0] ? store->user_home : NULL;
    store->env.path = store->path[0] ? store->path : NULL;
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

static void push_candidate(sxcl_java_candidate *out, size_t cap, size_t *count,
                           const char *path, const char *home, const char *source)
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
    ++(*count);
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
        if (env->user_home && *env->user_home) {
            join_path(tmp, sizeof(tmp), env->user_home, "files/runtime", '/');
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
