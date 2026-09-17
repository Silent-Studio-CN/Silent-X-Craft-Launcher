/* 日志归类与故障分类 —— 一行文本进,类别 + 关键信息出;一批行进,一条人话结论出。
 *
 * 为什么要有这一层:
 *   启动失败时把整篇日志糊到用户脸上等于没说。用户需要的是三句话之一:
 *   "你的 Java 版本不对" / "你的显卡驱动不支持你选的图形后端" / "你没登录或网络不通"。
 *   所以这里只做两件事:把每一行归到一个**固定的**类别;把整篇日志收敛成**一条**结论。
 *
 * 判定顺序是固定的"越具体越优先",不是打分:
 *   崩溃 > Vulkan 回退 > Java 版本 > 模组/加载器 > 缺东西 > 账户网络 > 图形栈 > 正常退出 > 未知
 *   (单行)而汇总结论另有一套优先级 —— 见 sxcl_log_summary_finish 的注释。
 *
 * 不做:不解析日志时间戳、不还原堆栈、不读文件(调用方逐行喂进来,和 process.h 的 on_line 天然对接)。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/launch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ────────────────────────── 小工具 ────────────────────────── */

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void copy_n(char *dst, size_t cap, const char *src, size_t len)
{
    size_t take = 0;
    if (cap == 0) {
        return;
    }
    take = (len < cap - 1) ? len : cap - 1;
    if (src && take) {
        memcpy(dst, src, take);
    }
    dst[take] = '\0';
}

static char lower_ch(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

/** 大小写不敏感的子串查找;找不到返回 NULL。 */
static const char *istrstr(const char *hay, const char *needle)
{
    size_t n = 0;
    const char *p = NULL;
    if (!hay || !needle || !*needle) {
        return NULL;
    }
    n = strlen(needle);
    for (p = hay; *p; ++p) {
        size_t i = 0;
        while (i < n && p[i] && lower_ch(p[i]) == lower_ch(needle[i])) {
            ++i;
        }
        if (i == n) {
            return p;
        }
    }
    return NULL;
}

static int icontains(const char *hay, const char *needle)
{
    return istrstr(hay, needle) != NULL;
}

static int contains_any(const char *text, const char *const *needles, size_t n)
{
    size_t i = 0;
    for (i = 0; i < n; ++i) {
        if (icontains(text, needles[i])) {
            return 1;
        }
    }
    return 0;
}

/** 取[start, stop 里任一字符)之间的一段,去掉尾部空白。 */
static void take_until(char *out, size_t cap, const char *start, const char *stop_chars)
{
    size_t n = 0;
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!start) {
        return;
    }
    while (*start == ' ' || *start == '\t') { /* "ClassNotFoundException: xxx" 冒号后面还有空格 */
        ++start;
    }
    while (start[n] && n + 1 < cap && (!stop_chars || !strchr(stop_chars, start[n]))) {
        ++n;
    }
    while (n > 0 && (start[n - 1] == ' ' || start[n - 1] == '\t' || start[n - 1] == '\r')) {
        --n;
    }
    memcpy(out, start, n);
    out[n] = '\0';
}

/** 取整行(去掉首尾空白),用于 GL 版本/渲染器这种"标签:值"的抽取。 */
static void take_rest(char *out, size_t cap, const char *start)
{
    const char *p = start;
    size_t n = 0;
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!p) {
        return;
    }
    while (*p == ' ' || *p == ':' || *p == '=' || *p == '\t') {
        ++p;
    }
    n = strlen(p);
    while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r' || p[n - 1] == '\n')) {
        --n;
    }
    copy_n(out, cap, p, n);
}

/** 去掉日志前缀:"[时间] [线程/级别]: " 与 hs_err 的 "# "。 */
static const char *message_of(const char *line)
{
    const char *p = line;
    if (!line) {
        return "";
    }
    if (*p == '[') {
        const char *last = NULL;
        const char *q = NULL;
        for (q = p; *q; ++q) {
            if (q[0] == ']' && q[1] == ':') {
                last = q;
            }
        }
        if (last) {
            p = last + 2;
            while (*p == ' ') {
                ++p;
            }
        }
    } else if (*p == '#') {
        while (*p == '#' || *p == ' ') {
            ++p;
        }
    }
    return p;
}

/** Android logcat 的级别字母:"03-12 10:00:06.000  1234  1234 E Krypton : ..."。
 *  只在行首确实是 logcat 的日期时才认,免得把普通英文日志里的 " E " 当级别。 */
static int logcat_severity(const char *line)
{
    const char *p = line;
    int field = 0;
    if (!(line[0] >= '0' && line[0] <= '9') || !(line[1] >= '0' && line[1] <= '9') ||
        line[2] != '-') {
        return -1;
    }
    while (*p && field < 6) {
        const char *start = NULL;
        size_t len = 0;
        while (*p == ' ' || *p == '	') {
            ++p;
        }
        if (!*p) {
            break;
        }
        start = p;
        while (*p && *p != ' ' && *p != '	') {
            ++p;
        }
        len = (size_t)(p - start);
        if (field >= 3 && len == 1) {
            if (start[0] == 'E' || start[0] == 'F') {
                return 2;
            }
            if (start[0] == 'W') {
                return 1;
            }
        }
        ++field;
    }
    return -1;
}

static int severity_of(const char *line)
{
    int logcat = logcat_severity(line);
    if (icontains(line, "/error]") || icontains(line, "fatal") || icontains(line, "/fatal]")) {
        return 2;
    }
    if (icontains(line, "/warn]") || icontains(line, "warning:")) {
        return 1;
    }
    if (logcat >= 0) {
        return logcat;
    }
    if (istrstr(line, "error") || istrstr(line, "exception") || istrstr(line, "fatal")) {
        return 2;
    }
    if (istrstr(line, "warn")) {
        return 1;
    }
    return 0;
}

/* ────────────────────────── 抽取 ────────────────────────── */

static int preceded_by_requirement(const char *text, const char *at)
{
    size_t back = (size_t)(at - text);
    char buf[20];
    if (back > 16) {
        back = 16;
    }
    if (back == 0) {
        return 0;
    }
    memcpy(buf, at - back, back);
    buf[back] = '\0';
    return icontains(buf, "requir") || icontains(buf, "need") || icontains(buf, "least") ||
           icontains(buf, "minimum") || icontains(buf, "upgrade");
}

/** 运行中的 Java 版本:"openjdk version \"21.0.3\"" / "Java 17.0.9-internal" / "Java Version: 8"。 */
static void extract_java_version(const char *text, char *out, size_t cap)
{
    const char *p = NULL;
    const char *q = NULL;
    out[0] = '\0';
    p = istrstr(text, "version \"");
    if (p) {
        take_until(out, cap, p + 9, "\"");
        if (out[0]) {
            return;
        }
    }
    p = istrstr(text, "java version:");
    if (p) {
        take_until(out, cap, p + 13, " \t(");
        if (out[0]) {
            return;
        }
    }
    p = text;
    while ((p = istrstr(p, "java ")) != NULL) {
        const char *v = p + 5;
        if (*v >= '0' && *v <= '9' && !preceded_by_requirement(text, p)) {
            take_until(out, cap, v, " \t,;()");
            if (out[0]) {
                return;
            }
        }
        p = v;
    }
    /* JVM 自己的 "openjdk version" 之外的兜底:jvm_version=21.0.3 */
    q = istrstr(text, "jvm_version=");
    if (q) {
        take_until(out, cap, q + 12, " \t,;");
    }
}

static void extract_gl_version(const char *text, char *out, size_t cap)
{
    /* 不要收 "gl version" 这种松标记:"LWJGL version 3.3.3" 里就含 "GL version",会张冠李戴 */
    static const char *const markers[] = { "opengl version", "gl_version=", "gl_version:",
                                           "opengl es" };
    size_t i = 0;
    out[0] = '\0';
    for (i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
        const char *p = istrstr(text, markers[i]);
        if (p) {
            take_rest(out, cap, p + strlen(markers[i]));
            if (out[0]) {
                return;
            }
        }
    }
}

static void extract_gl_renderer(const char *text, char *out, size_t cap)
{
    static const char *const markers[] = { "opengl renderer", "gl_renderer=", "gl_renderer:", "renderer",
                                           "vulkan device", "gpu" };
    size_t i = 0;
    out[0] = '\0';
    for (i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
        const char *p = istrstr(text, markers[i]);
        if (p) {
            take_rest(out, cap, p + strlen(markers[i]));
            if (out[0]) {
                return;
            }
        }
    }
}

/** 缺的类/库/资源名:抽出来给用户看"到底缺什么"。 */
static void extract_missing(const char *text, char *out, size_t cap)
{
    static const char *const token_markers[] = { "classnotfoundexception:", "noclassdeffounderror:",
                                                 "could not find or load main class ",
                                                 "filenotfoundexception:" };
    size_t i = 0;
    const char *p = NULL;
    out[0] = '\0';
    for (i = 0; i < sizeof(token_markers) / sizeof(token_markers[0]); ++i) {
        p = istrstr(text, token_markers[i]);
        if (p) {
            take_until(out, cap, p + strlen(token_markers[i]), " \t,;(");
            if (out[0]) {
                return;
            }
        }
    }
    p = istrstr(text, "library \"");
    if (p) {
        take_until(out, cap, p + 9, "\"");
        if (out[0]) {
            return;
        }
    }
    p = istrstr(text, "unsatisfiedlinkerror:");
    if (p) {
        take_rest(out, cap, p + 19);
        if (out[0]) {
            return;
        }
    }
    p = istrstr(text, "mod id: '");
    if (p) {
        take_until(out, cap, p + 9, "'");
        if (out[0]) {
            return;
        }
    }
    p = istrstr(text, "no such file or directory");
    if (p) {
        take_rest(out, cap, message_of(text));
        if (out[0]) {
            return;
        }
    }
    p = istrstr(text, "missing asset");
    if (p) {
        take_rest(out, cap, p + 13);
    }
}

static int extract_exit_code(const char *text)
{
    const char *p = text;
    while ((p = istrstr(p, "code")) != NULL) {
        const char *v = p + 4;
        const char *back = p;
        size_t b = 0;
        size_t back_len = (size_t)(p - text);
        char ctx[24];
        while (*v == ' ' || *v == ':' || *v == '=') {
            ++v;
        }
        if (*v < '0' || *v > '9') {
            p = v;
            continue;
        }
        b = (back_len > 20) ? 20 : back_len;
        memcpy(ctx, back - b, b);
        ctx[b] = '\0';
        if (icontains(ctx, "exit") || icontains(ctx, "exited")) {
            int value = 0;
            while (*v >= '0' && *v <= '9') {
                value = value * 10 + (*v - '0');
                if (value > 100000) {
                    break;
                }
                ++v;
            }
            return value;
        }
        p = v;
    }
    return -1;
}

/* ────────────────────────── 类别判定 ────────────────────────── */

static int looks_like_crash(const char *text)
{
    static const char *const markers[] = { "a fatal error has been detected",
                                           "exception_access_violation",
                                           "outofmemoryerror",
                                           "java heap space",
                                           "gc overhead limit exceeded",
                                           "hs_err_pid",
                                           "crash-reports/crash-",
                                           "the game crashed whilst",
                                           "sigsegv",
                                           "sigbus",
                                           "sigill",
                                           "exception in thread \"main\"" };
    return contains_any(text, markers, sizeof(markers) / sizeof(markers[0]));
}

static int looks_like_vulkan_fallback(const char *text)
{
    static const char *const trouble[] = { "fall", "not supported", "unsupported", "unavailable",
                                           "failed", "error", "vk_error", "no vulkan",
                                           "不支持", "不可用", "回退", "降级" };
    if (!icontains(text, "vulkan") && !icontains(text, "zink")) {
        return 0;
    }
    return contains_any(text, trouble, sizeof(trouble) / sizeof(trouble[0]));
}

static int looks_like_java_version(const char *text)
{
    static const char *const markers[] = { "unsupportedclassversionerror",
                                           "unsupported major.minor version",
                                           "class file version",
                                           "requires java",
                                           "unsupported java",
                                           "minimum java",
                                           "java version mismatch",
                                           "not compatible with this java" };
    return contains_any(text, markers, sizeof(markers) / sizeof(markers[0]));
}

static int has_word_mod(const char *text)
{
    return icontains(text, "mod ") || icontains(text, "mods ") || icontains(text, "mod'") ||
           icontains(text, "mods/") || icontains(text, "mod\t");
}

static int looks_like_mod_loader(const char *text)
{
    static const char *const strong[] = { "incompatible mod",
                                          "modresolutionexception",
                                          "missing or unsupported mandatory dependencies",
                                          "loaderexception",
                                          "mixin apply failed",
                                          "failed to load mod",
                                          "failed to load mods",
                                          "mod loading has failed",
                                          "duplicate mod",
                                          "missing mod",
                                          "mods.toml",
                                          "a mod crashed",
                                          "mod resolution",
                                          "net.minecraftforge.fml",
                                          "fabric-loader" };
    if (contains_any(text, strong, sizeof(strong) / sizeof(strong[0]))) {
        return 1;
    }
    if (has_word_mod(text) &&
        (icontains(text, "requires") || icontains(text, "not compatible") ||
         icontains(text, "incompatible") || icontains(text, "failed") ||
         icontains(text, "error") || icontains(text, "missing"))) {
        return 1;
    }
    return 0;
}

static int looks_like_missing(const char *text)
{
    static const char *const markers[] = { "classnotfoundexception",
                                           "noclassdeffounderror",
                                           "unsatisfiedlinkerror",
                                           "could not find or load main class",
                                           "dlopen failed",
                                           "cannot open shared object file",
                                           "filenotfoundexception",
                                           "no such file or directory",
                                           "missing asset",
                                           "unable to load library",
                                           "failed to extract native" };
    if (contains_any(text, markers, sizeof(markers) / sizeof(markers[0]))) {
        return 1;
    }
    if (icontains(text, "library \"") && icontains(text, "not found")) {
        return 1;
    }
    return 0;
}

static int looks_like_account_net(const char *text)
{
    static const char *const markers[] = { "connectexception",
                                           "sockettimeoutexception",
                                           "unknownhostexception",
                                           "sslhandshakeexception",
                                           "connection refused",
                                           "connection reset",
                                           "read timed out",
                                           "unable to connect",
                                           "invalid session",
                                           "failed to verify username",
                                           "failed to log in",
                                           "failed to login",
                                           "authentication",
                                           "minecraftservices.com",
                                           "sessionserver.mojang.com",
                                           "authserver.mojang.com",
                                           "network is unreachable",
                                           "failed to fetch profile",
                                           "no internet" };
    return contains_any(text, markers, sizeof(markers) / sizeof(markers[0]));
}

static int looks_like_graphics(const char *text)
{
    static const char *const markers[] = { "lwjgl",
                                           "glfw",
                                           "opengl",
                                           "egl",
                                           "glew",
                                           "vulkan",
                                           "zink",
                                           "mesa",
                                           "swiftshader",
                                           "angle",
                                           "pixel format",
                                           "failed to create window",
                                           "renderer",
                                           "gpu",
                                           "graphics driver",
                                           "no gl context",
                                           "gl_invalid",
                                           "mali",
                                           "adreno",
                                           "powervr",
                                           "nvidia",
                                           "radeon",
                                           "intel graphics" };
    return contains_any(text, markers, sizeof(markers) / sizeof(markers[0]));
}

static int looks_like_exit_ok(const char *text)
{
    static const char *const markers[] = { "stopping!", "stopping server", "stopping worker threads",
                                           "shutting down", "game stopped", "goodbye",
                                           "exited with code 0" };
    if (contains_any(text, markers, sizeof(markers) / sizeof(markers[0]))) {
        return 1;
    }
    return 0;
}

/* ────────────────────────── 单行扫描 ────────────────────────── */

sxcl_log_kind sxcl_log_scan_line(const char *line, sxcl_log_line *out)
{
    sxcl_log_line local;
    const char *text = line ? line : "";
    sxcl_log_kind kind = SXCL_LOG_UNKNOWN;
    int exit_code = -1;

    memset(&local, 0, sizeof(local));
    local.exit_code = -1;
    local.severity = severity_of(text);
    exit_code = extract_exit_code(text);
    local.exit_code = exit_code;
    extract_java_version(text, local.java_version, sizeof(local.java_version));
    extract_gl_version(text, local.gl_version, sizeof(local.gl_version));
    extract_gl_renderer(text, local.gl_renderer, sizeof(local.gl_renderer));
    extract_missing(text, local.missing, sizeof(local.missing));
    if (looks_like_vulkan_fallback(text)) {
        local.vulkan_fallback = 1;
    }

    /* 判定顺序固定:越具体越靠前 */
    if (looks_like_crash(text)) {
        kind = SXCL_LOG_CRASH;
        copy_str(local.reason, sizeof(local.reason), message_of(text));
    } else if (exit_code > 0) {
        char buf[128];
        (void)snprintf(buf, sizeof(buf), "进程以退出码 %d 结束", exit_code);
        kind = SXCL_LOG_CRASH;
        copy_str(local.reason, sizeof(local.reason), buf);
    } else if (local.vulkan_fallback) {
        kind = SXCL_LOG_VULKAN_FALLBACK;
    } else if (looks_like_java_version(text)) {
        kind = SXCL_LOG_JAVA_VERSION;
    } else if (looks_like_mod_loader(text)) {
        kind = SXCL_LOG_MOD_LOADER;
    } else if (looks_like_missing(text)) {
        kind = SXCL_LOG_MISSING;
    } else if (looks_like_account_net(text)) {
        kind = SXCL_LOG_ACCOUNT_NET;
    } else if (looks_like_graphics(text)) {
        kind = SXCL_LOG_GRAPHICS;
    } else if (looks_like_exit_ok(text) || exit_code == 0) {
        kind = SXCL_LOG_EXIT_OK;
    } else {
        kind = SXCL_LOG_UNKNOWN;
    }

    local.kind = kind;
    if (kind != SXCL_LOG_UNKNOWN && kind != SXCL_LOG_EXIT_OK && !local.reason[0]) {
        copy_str(local.reason, sizeof(local.reason), message_of(text));
    }
    if (out) {
        *out = local;
    }
    return kind;
}

/* ────────────────────────── 汇总 ────────────────────────── */

const char *sxcl_log_kind_name(sxcl_log_kind kind)
{
    switch (kind) {
    case SXCL_LOG_GRAPHICS:        return "graphics";
    case SXCL_LOG_VULKAN_FALLBACK: return "vulkan_fallback";
    case SXCL_LOG_JAVA_VERSION:    return "java_version";
    case SXCL_LOG_MOD_LOADER:      return "mod_loader";
    case SXCL_LOG_MISSING:         return "missing";
    case SXCL_LOG_ACCOUNT_NET:     return "account_net";
    case SXCL_LOG_CRASH:           return "crash";
    case SXCL_LOG_EXIT_OK:         return "exit_ok";
    case SXCL_LOG_UNKNOWN:
    default:                       return "unknown";
    }
}

const char *sxcl_log_conclusion_name(sxcl_log_conclusion conclusion)
{
    switch (conclusion) {
    case SXCL_LOG_CONCLUSION_OK:              return "ok";
    case SXCL_LOG_CONCLUSION_GRAPHICS:        return "graphics";
    case SXCL_LOG_CONCLUSION_VULKAN_FALLBACK: return "vulkan_fallback";
    case SXCL_LOG_CONCLUSION_JAVA:            return "java";
    case SXCL_LOG_CONCLUSION_MOD:             return "mod";
    case SXCL_LOG_CONCLUSION_MISSING:         return "missing";
    case SXCL_LOG_CONCLUSION_ACCOUNT:         return "account";
    case SXCL_LOG_CONCLUSION_CRASH:           return "crash";
    case SXCL_LOG_CONCLUSION_UNKNOWN:
    default:                                  return "unknown";
    }
}

void sxcl_log_summary_init(sxcl_log_summary *summary)
{
    if (!summary) {
        return;
    }
    memset(summary, 0, sizeof(*summary));
    summary->exit_code = -1;
    summary->conclusion = SXCL_LOG_CONCLUSION_UNKNOWN;
    copy_str(summary->advice, sizeof(summary->advice), "还没看到可判断的日志行。");
}

sxcl_log_kind sxcl_log_summary_add(sxcl_log_summary *summary, const char *line)
{
    sxcl_log_line info;
    sxcl_log_kind kind = sxcl_log_scan_line(line, &info);
    if (!summary) {
        return kind;
    }
    ++summary->lines;
    if ((size_t)kind < (size_t)SXCL_LOG_KIND_COUNT) {
        ++summary->kind_counts[kind];
    }
    if (info.vulkan_fallback) {
        summary->voted_vulkan_fallback = 1;
    }
    if (kind == SXCL_LOG_CRASH) {
        summary->voted_crash = 1;
    }
    if (kind == SXCL_LOG_JAVA_VERSION) {
        summary->voted_java = 1;
    }
    if (kind == SXCL_LOG_MOD_LOADER) {
        summary->voted_mod = 1;
    }
    if (kind == SXCL_LOG_MISSING) {
        summary->voted_missing = 1;
    }
    if (kind == SXCL_LOG_ACCOUNT_NET) {
        summary->voted_account = 1;
    }
    if (kind == SXCL_LOG_GRAPHICS && info.severity >= 1) {
        /* 光提到 OpenGL 不算问题:必须是 WARN/ERROR 才算"图形栈出事了" */
        summary->voted_graphics = 1;
    }
    if (kind == SXCL_LOG_EXIT_OK) {
        summary->exited_ok = 1;
    }
    if (info.exit_code >= 0 && summary->exit_code < 0) {
        summary->exit_code = info.exit_code;
    }
    if (!summary->java_version[0] && info.java_version[0]) {
        copy_str(summary->java_version, sizeof(summary->java_version), info.java_version);
    }
    if (!summary->gl_version[0] && info.gl_version[0]) {
        copy_str(summary->gl_version, sizeof(summary->gl_version), info.gl_version);
    }
    if (!summary->gl_renderer[0] && info.gl_renderer[0]) {
        copy_str(summary->gl_renderer, sizeof(summary->gl_renderer), info.gl_renderer);
    }
    if (!summary->missing[0] && info.missing[0]) {
        copy_str(summary->missing, sizeof(summary->missing), info.missing);
    }
    if (!summary->crash_reason[0] && kind == SXCL_LOG_CRASH && info.reason[0]) {
        copy_str(summary->crash_reason, sizeof(summary->crash_reason), info.reason);
    }
    return kind;
}

void sxcl_log_summary_finish(sxcl_log_summary *summary)
{
    char buf[256];
    if (!summary) {
        return;
    }
    /* 结论优先级(与单行判定不同,这里考虑的是"用户该改什么"):
     *   Java 版本 > 缺东西 > 模组/加载器 > Vulkan 回退 > 图形栈 > 崩溃 > 账户网络 > 正常退出 > 未知
     * Java 排第一是因为版本不符会连带刷出一堆 NoClassDefFoundError 之类的次生错误,
     * 那些次生错误的类别看着更"具体",其实全是假线索。 */
    if (summary->voted_java) {
        summary->conclusion = SXCL_LOG_CONCLUSION_JAVA;
        if (summary->java_version[0]) {
            (void)snprintf(buf, sizeof(buf),
                           "Java 版本不匹配:当前用的是 Java %s,这个版本要求的更高(或更低)。"
                           "请在设置里换一个 Java 再启动。",
                           summary->java_version);
        } else {
            (void)snprintf(buf, sizeof(buf),
                           "Java 版本不匹配:游戏要求的 Java 比当前使用的高。请在设置里换一个 Java 再启动。");
        }
    } else if (summary->voted_missing) {
        summary->conclusion = SXCL_LOG_CONCLUSION_MISSING;
        if (summary->missing[0]) {
            (void)snprintf(buf, sizeof(buf),
                           "缺少运行需要的文件或库:%s。多半是下载没完成,重新校验并补全这个版本的文件即可。",
                           summary->missing);
        } else {
            (void)snprintf(buf, sizeof(buf),
                           "缺少运行需要的文件或库。多半是下载没完成,重新校验并补全这个版本的文件即可。");
        }
    } else if (summary->voted_mod) {
        summary->conclusion = SXCL_LOG_CONCLUSION_MOD;
        if (summary->crash_reason[0]) {
            (void)snprintf(buf, sizeof(buf),
                           "模组或加载器报错:%s。先确认模组与游戏版本匹配,或移除最近装的那个模组。",
                           summary->crash_reason);
        } else {
            (void)snprintf(buf, sizeof(buf),
                           "模组或加载器报错。先确认模组与游戏版本匹配,或移除最近装的那个模组。");
        }
    } else if (summary->voted_vulkan_fallback) {
        summary->conclusion = SXCL_LOG_CONCLUSION_VULKAN_FALLBACK;
        (void)snprintf(buf, sizeof(buf),
                       "图形后端回退了:设备没有接受 Vulkan,实际跑的是 OpenGL。"
                       "界面里选的 Vulkan 看起来没生效 —— 想稳就用 OpenGL。");
    } else if (summary->voted_graphics) {
        summary->conclusion = SXCL_LOG_CONCLUSION_GRAPHICS;
        if (summary->crash_reason[0]) {
            (void)snprintf(buf, sizeof(buf),
                           "图形栈出问题:%s。优先检查显卡驱动是否支持游戏要求的 OpenGL 版本。",
                           summary->crash_reason);
        } else {
            (void)snprintf(buf, sizeof(buf),
                           "图形栈出问题。优先检查显卡驱动是否支持游戏要求的 OpenGL 版本。");
        }
    } else if (summary->voted_crash) {
        summary->conclusion = SXCL_LOG_CONCLUSION_CRASH;
        if (summary->crash_reason[0]) {
            (void)snprintf(buf, sizeof(buf), "游戏崩溃了:%s。", summary->crash_reason);
        } else {
            (void)snprintf(buf, sizeof(buf), "游戏崩溃了,但日志里没写出明确原因。");
        }
    } else if (summary->voted_account) {
        summary->conclusion = SXCL_LOG_CONCLUSION_ACCOUNT;
        if (summary->crash_reason[0]) {
            (void)snprintf(buf, sizeof(buf),
                           "账户或网络问题:%s。检查登录状态与网络连接。", summary->crash_reason);
        } else {
            (void)snprintf(buf, sizeof(buf), "账户或网络问题。检查登录状态与网络连接。");
        }
    } else if (summary->exited_ok) {
        summary->conclusion = SXCL_LOG_CONCLUSION_OK;
        (void)snprintf(buf, sizeof(buf), "日志里看到正常退出,这次启动没有发现明显故障。");
    } else {
        summary->conclusion = SXCL_LOG_CONCLUSION_UNKNOWN;
        (void)snprintf(buf, sizeof(buf),
                       "没从日志里看出明确的失败原因(可能日志还没写到出错的那一步)。");
    }
    copy_str(summary->advice, sizeof(summary->advice), buf);
}

void sxcl_log_summarize(const char *const *lines, size_t count, sxcl_log_summary *out)
{
    size_t i = 0;
    if (!out) {
        return;
    }
    sxcl_log_summary_init(out);
    for (i = 0; i < count; ++i) {
        (void)sxcl_log_summary_add(out, lines[i]);
    }
    sxcl_log_summary_finish(out);
}
