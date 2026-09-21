/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/launch.h"

#include "sxcl/lang.h" /* 原因文案可以被用户语言包逐条覆盖(lang_table 同一套口径) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 原因键的文案表(name + advice,中英各一份)。放在独立文件里,与 lang_table.inc 同一套写法:
 * 改文案只改那个文件,判定逻辑一行都不用动。 */
#include "reason_lang.inc"

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


/* ══════════════════════ 原因键的规则表(第 3b 节) ══════════════════════
 *
 * 判定口径与"类别"一致:**大小写不敏感的子串**。为什么不写正则:
 *   1) 仓库里没有正则引擎,而 HCML 那套(约 60 条正则)是靠引擎撑起来的;
 *   2) 日志里的关键信息都是固定句型("java.lang.OutOfMemoryError: Java heap space"),
 *      子串足够,还能确保不引入回溯这种性能地雷。
 * 每条规则 = 原因键 + 分数(越大越具体)+ 任一命中(any)+ 全部命中(all,可空)。
 * 汇总时取**分数最高**的那条;同分取表里靠前的(顺序即优先级,别随意重排)。
 *
 * 为什么有 all:像 "Intel" 这种词在 CPU 信息行里也有("CPU: Intel(R) Core"),只认
 * 一个词会把 CPU 行误判成核显驱动问题;要求同现 "graphics" 才是真的图形栈行。
 *
 * 为什么要**门槛**(见 reason_gate):纯 INFO 的 "OpenGL Renderer: Intel(R) UHD Graphics"
 * 不是在报错 —— 只有"像出事了"的行才进规则表,免得把一条正常的环境信息当成故障。
 */

#define SXCL_RULE_N(a) (sizeof(a) / sizeof((a)[0]))

static const char *const kAnyOom[] = {"outofmemoryerror", "java heap space",
                                      "gc overhead limit exceeded", "out of memory"};
static const char *const kAnyNativeOom[] = {"unable to create new native thread",
                                            "insufficient memory for the java runtime",
                                            "native memory allocation", "out of native memory"};
static const char *const kAny32Bit[] = {"could not reserve enough space for",
                                        "maximum representable size", "32-bit jvm",
                                        "32 位 java", "32-bit java"};
static const char *const kAnyHeapBig[] = {"invalid maximum heap size", "invalid initial heap size",
                                          "heap size is invalid", "invalid xmx"};
static const char *const kAnyJvmFatal[] = {"a fatal error has been detected",
                                           "exception_access_violation", "sigsegv", "sigbus",
                                           "sigill", "hs_err_pid", "problematic frame:"};
static const char *const kAnyJvmInternal[] = {"internal error (", "assertion failed", "guarantee(",
                                              "fatal error in", "jvm internal error"};
static const char *const kAnyStackOverflow[] = {"stackoverflowerror", "stack overflow"};
static const char *const kAnyJavaVersion[] = {"unsupportedclassversionerror",
                                              "unsupported major.minor version",
                                              "compiled by a more recent version", "requires java",
                                              "class file version", "minimum java version",
                                              "java version mismatch", "not compatible with this java"};
static const char *const kAnyJavaBroken[] = {"could not create the java virtual machine",
                                             "failed to load jvm", "jvm.dll", "libjvm.so",
                                             "unable to access jarfile"};
static const char *const kAnyGfxIntel[] = {"intel", "iris", "hd graphics", "uhd graphics", "i915",
                                           "gma "};
static const char *const kAllGfxIntel[] = {"graphic"};
static const char *const kAnyGfxNvidia[] = {"nvidia", "geforce", "nvgpu", "nvlddmkm"};
static const char *const kAnyGfxAmd[] = {"radeon", "adrenalin", "amdxx", "amd graphics",
                                         "amd driver"};
static const char *const kAnyGfxMesa[] = {"mesa", "llvmpipe", "softpipe", "swrast", "gallium"};
static const char *const kAnyGfxAdreno[] = {"adreno"};
static const char *const kAnyGfxMali[] = {"arm mali", "mali-g", "mali g", "mali driver",
                                          "mali_tiler"};
static const char *const kAnyGfxPowervr[] = {"powervr", "imagination technologies"};
static const char *const kAnyGfxSoftware[] = {"swiftshader", "software rendering",
                                              "software rasterizer", "no hardware acceleration",
                                              "llvmpipe"};
static const char *const kAnyGfxOutdated[] = {"update your graphics driver", "outdated driver",
                                              "driver does not support", "out of date driver",
                                              "please update the driver",
                                              "the driver does not appear to support"};
static const char *const kAnyGfxGeneric[] = {"graphics driver", "gpu driver", "display driver",
                                             "video driver", "graphics device"};
static const char *const kAnyGlLow[] = {"requires opengl", "unsupported opengl",
                                        "opengl version not supported",
                                        "does not support opengl", "requires at least opengl"};
static const char *const kAnyGlfw[] = {"glfw error", "failed to create window", "glfw init",
                                       "window creation failed", "glfwwindow"};
static const char *const kAnyPixelFormat[] = {"pixel format", "pixelformat", "choose pixel format",
                                              "no accelerated"};
static const char *const kAnyNoContext[] = {"no gl context", "failed to create gl",
                                            "could not create gl context",
                                            "failed to make context current", "egl_",
                                            "opengl context"};
static const char *const kAnyRemote[] = {"remote desktop", "rdp session", "session 0", "no gpu",
                                         "gpu not found", "virtual machine"};
static const char *const kAnyGpuCrash[] = {"nvoglv", "atio6axx", "amdxx64", "igdumd", "amdxc",
                                           "opengl32.dll", "libgl.so", "libglx"};
static const char *const kAnyGpuCrashFrame[] = {"problematic frame:"};
static const char *const kAllGpuCrashFrame[] = {"gl"};
static const char *const kAnyVulkanNo[] = {"vk_error_incompatible_driver",
                                           "vulkan device not found", "vulkan is not supported",
                                           "no vulkan", "vulkan unavailable",
                                           "failed to create vulkan", "vkresult"};
static const char *const kAnyVulkanFallback[] = {"falling back to opengl", "fall back to opengl",
                                                 "回退到 opengl", "using opengl instead",
                                                 "vulkan_fallback"};
static const char *const kAnyModDuplicate[] = {"duplicate mod", "duplicatemodsfoundexception",
                                               "duplicate mods", "already registered",
                                               "has already been registered", "duplicate entry"};
static const char *const kAnyModConflict[] = {"modresolutionexception", "incompatible mod set",
                                              "mod resolution", "conflicting mods",
                                              "dependency resolution", "unresolved dependency",
                                              "incompatible dependencies"};
static const char *const kAnyModMissingDep[] = {"missing or unsupported mandatory dependencies",
                                                "mandatory dependency", "missing dependency",
                                                "requires mod", "missing mod", "which is missing"};
static const char *const kAnyModVersion[] = {"not compatible with", "wrong version",
                                             "requires minecraft", "mod version mismatch",
                                             "unsupported mod version"};
static const char *const kAnyModFileBad[] = {"invalid mod file", "corrupted mod",
                                             "failed to read mod", "malformed mod"};
static const char *const kAnyLoaderMissing[] = {"requires forge", "requires fabric",
                                                "please install forge", "no forge",
                                                "mod loader is not installed",
                                                "modloader not found"};
static const char *const kAnyLoaderVersion[] = {"loader version", "forge version",
                                                "fabric loader version", "incompatible loader"};
static const char *const kAnyMixin[] = {"mixin apply failed", "mixin transformation",
                                        "invalidmixin", "mixin error",
                                        "org.spongepowered.asm.mixin", "mixin config",
                                        "failed to apply mixin", "mixin injection"};
static const char *const kAnyOptifine[] = {"optifine"};
static const char *const kAllOptifine[] = {"conflict", "incompatible", "crash"};
static const char *const kAnyShader[] = {"shaderpack", "shader pack", "iris shader",
                                         "failed to compile shader", "shader compilation"};
static const char *const kAnyModCrash[] = {"a mod crashed", "crashed whilst loading",
                                           "mod loading has failed", "failed to load mod",
                                           "mod crashed"};
static const char *const kAnyMissLib[] = {"unsatisfiedlinkerror", "no lwjgl in java.library.path",
                                          "cannot open shared object file",
                                          "the specified module could not be found", "dlopen failed",
                                          "java.library.path", "failed to load native library",
                                          "nativelibrarynotfound"};
static const char *const kAnyMissClass[] = {"classnotfoundexception", "noclassdeffounderror",
                                            "class not found"};
static const char *const kAnyMissAsset[] = {"missing asset", "unable to load asset",
                                            "failed to download asset", "missing resource",
                                            "resource not found"};
static const char *const kAnyNatives[] = {"failed to extract native", "natives extract",
                                          "could not extract native", "natives not found"};
static const char *const kAnyCorruptJar[] = {"invalid or corrupt jarfile",
                                             "zip end header not found",
                                             "error in opening zip file", "unexpected end of zip",
                                             "corrupt jar", "invalid jar"};
static const char *const kAnyClasspath[] = {"could not find or load main class",
                                            "classpath is invalid", "unable to access jarfile"};
static const char *const kAnyPermission[] = {"access denied", "permission denied", "errno 13",
                                             "eacces", "read-only file system", "拒绝访问",
                                             "没有权限", "access is denied"};
static const char *const kAnyDiskFull[] = {"no space left on device", "not enough space on the disk",
                                           "disk full", "磁盘空间"};
static const char *const kAnyPathMissing[] = {"the system cannot find the path specified",
                                              "系统找不到指定的路径", "no such file or directory",
                                              "path not found", "cannot find the path"};
static const char *const kAnyArch[] = {"wrong elf class", "cannot execute binary file",
                                       "is not a valid win32 application", "bad cpu type",
                                       "exec format error", "incompatible architecture",
                                       "wrong architecture"};
static const char *const kAnySession[] = {"invalid session", "failed to verify username",
                                          "session server", "invalid token",
                                          "authentication servers are down", "session has expired"};
static const char *const kAnyAuth[] = {"failed to log in", "failed to login", "unauthorized",
                                       "you do not own", "does not own minecraft",
                                       "failed to fetch profile"};
static const char *const kAnyNetDown[] = {"connectexception", "connection refused",
                                          "network is unreachable", "no route to host",
                                          "connection reset", "unable to connect",
                                          "连接被拒绝"};
static const char *const kAnyDns[] = {"unknownhostexception", "name resolution",
                                      "no such host", "name or service not known"};
static const char *const kAnySsl[] = {"sslhandshakeexception", "pkix", "certificate",
                                      "ssl error", "tls error"};
static const char *const kAnyNetTimeout[] = {"sockettimeoutexception", "read timed out",
                                             "connection timed out", "request timed out"};
static const char *const kAnyServerDown[] = {"service unavailable", "servers are down",
                                             "502 bad gateway", "503"};
static const char *const kAnyNoexec[] = {"noexec"};
static const char *const kAnySharedDenied[] = {"permission denied", "denied"};
static const char *const kAllSharedDenied[] = {"/storage/emulated"};
static const char *const kAnySelinux[] = {"execute_no_trans", "avc: denied", "selinux"};
static const char *const kAnyCrash[] = {"the game crashed whilst", "minecraft crash report",
                                        "crash report", "crash-report"};
static const char *const kAnyExitOk[] = {"stopping!", "stopping server", "stopping worker threads",
                                         "goodbye"};

#define SXCL_RULE(reason_kind, score_value, any_list)                                            \
    {                                                                                            \
        reason_kind, score_value, any_list, SXCL_RULE_N(any_list), NULL, 0u                      \
    }
#define SXCL_RULE_ALL(reason_kind, score_value, any_list, all_list)                                \
    {                                                                                            \
        reason_kind, score_value, any_list, SXCL_RULE_N(any_list), all_list, SXCL_RULE_N(all_list) \
    }

typedef struct sxcl_reason_rule {
    sxcl_log_reason reason;
    int score;                        /* 越大越具体 */
    const char *const *any;           /* 任一命中 */
    size_t any_count;
    const char *const *all;           /* 全部命中(可空) */
    size_t all_count;
} sxcl_reason_rule;

/* 顺序 = 同分时的优先级(靠前优先),**别随意重排**。 */
static const sxcl_reason_rule kReasonRules[] = {
    SXCL_RULE(SXCL_REASON_JAVA_VERSION_MISMATCH, 96, kAnyJavaVersion),
    SXCL_RULE(SXCL_REASON_JVM_FATAL, 95, kAnyJvmFatal),
    SXCL_RULE(SXCL_REASON_JVM_INTERNAL, 94, kAnyJvmInternal),
    SXCL_RULE(SXCL_REASON_STACK_OVERFLOW, 93, kAnyStackOverflow),
    SXCL_RULE(SXCL_REASON_OUT_OF_MEMORY, 92, kAnyOom),
    SXCL_RULE(SXCL_REASON_MOD_DUPLICATE, 92, kAnyModDuplicate),
    SXCL_RULE(SXCL_REASON_OUT_OF_NATIVE_MEMORY, 91, kAnyNativeOom),
    SXCL_RULE(SXCL_REASON_JVM_32BIT, 90, kAny32Bit),
    SXCL_RULE(SXCL_REASON_MOD_RESOLUTION_CONFLICT, 90, kAnyModConflict),
    SXCL_RULE(SXCL_REASON_MIXIN_FAILURE, 90, kAnyMixin),
    SXCL_RULE(SXCL_REASON_MOD_MISSING_DEPENDENCY, 89, kAnyModMissingDep),
    SXCL_RULE(SXCL_REASON_HEAP_TOO_LARGE, 89, kAnyHeapBig),
    SXCL_RULE(SXCL_REASON_JAVA_BROKEN, 88, kAnyJavaBroken),
    SXCL_RULE(SXCL_REASON_MOD_VERSION_MISMATCH, 88, kAnyModVersion),
    SXCL_RULE(SXCL_REASON_ANDROID_NOEXEC, 88, kAnyNoexec),
    SXCL_RULE(SXCL_REASON_ANDROID_SELINUX, 87, kAnySelinux),
    SXCL_RULE(SXCL_REASON_MOD_FILE_CORRUPTED, 87, kAnyModFileBad),
    SXCL_RULE(SXCL_REASON_MOD_LOADER_MISSING, 86, kAnyLoaderMissing),
    SXCL_RULE(SXCL_REASON_MOD_LOADER_VERSION, 85, kAnyLoaderVersion),
    SXCL_RULE_ALL(SXCL_REASON_OPTIFINE_CONFLICT, 84, kAnyOptifine, kAllOptifine),
    SXCL_RULE(SXCL_REASON_SHADER_FAILURE, 83, kAnyShader),
    SXCL_RULE(SXCL_REASON_CORRUPT_JAR, 82, kAnyCorruptJar),
    SXCL_RULE(SXCL_REASON_ACCOUNT_INVALID_SESSION, 82, kAnySession),
    SXCL_RULE(SXCL_REASON_ACCOUNT_AUTH_FAILED, 81, kAnyAuth),
    SXCL_RULE(SXCL_REASON_MISSING_JAVA_LIBRARY, 80, kAnyMissLib),
    SXCL_RULE(SXCL_REASON_NATIVES_EXTRACT_FAILED, 79, kAnyNatives),
    /* 架构不符比"缺原生库"更具体(缺库常常是它的次生现象),所以分数更高 */
    SXCL_RULE(SXCL_REASON_ARCH_MISMATCH, 84, kAnyArch),
    SXCL_RULE(SXCL_REASON_GRAPHICS_DRIVER_OUTDATED, 78, kAnyGfxOutdated),
    SXCL_RULE(SXCL_REASON_DISK_FULL, 77, kAnyDiskFull),
    SXCL_RULE(SXCL_REASON_GRAPHICS_DRIVER_SOFTWARE, 76, kAnyGfxSoftware),
    SXCL_RULE(SXCL_REASON_FILE_PERMISSION, 76, kAnyPermission),
    SXCL_RULE_ALL(SXCL_REASON_GRAPHICS_DRIVER_INTEL, 75, kAnyGfxIntel, kAllGfxIntel),
    SXCL_RULE(SXCL_REASON_GRAPHICS_DRIVER_NVIDIA, 75, kAnyGfxNvidia),
    SXCL_RULE(SXCL_REASON_GRAPHICS_DRIVER_AMD, 75, kAnyGfxAmd),
    SXCL_RULE(SXCL_REASON_GRAPHICS_DRIVER_MESA, 75, kAnyGfxMesa),
    SXCL_RULE(SXCL_REASON_GRAPHICS_DRIVER_ADRENO, 75, kAnyGfxAdreno),
    SXCL_RULE(SXCL_REASON_GRAPHICS_DRIVER_MALI, 75, kAnyGfxMali),
    SXCL_RULE(SXCL_REASON_GRAPHICS_DRIVER_POWERVR, 75, kAnyGfxPowervr),
    SXCL_RULE(SXCL_REASON_OPENGL_TOO_LOW, 74, kAnyGlLow),
    SXCL_RULE(SXCL_REASON_GLFW_INIT_FAILED, 73, kAnyGlfw),
    SXCL_RULE(SXCL_REASON_PIXEL_FORMAT_FAILED, 72, kAnyPixelFormat),
    SXCL_RULE(SXCL_REASON_GPU_DRIVER_CRASH, 72, kAnyGpuCrash),
    SXCL_RULE_ALL(SXCL_REASON_GPU_DRIVER_CRASH, 71, kAnyGpuCrashFrame, kAllGpuCrashFrame),
    SXCL_RULE(SXCL_REASON_NO_GL_CONTEXT, 71, kAnyNoContext),
    SXCL_RULE(SXCL_REASON_MOD_CRASH, 70, kAnyModCrash),
    SXCL_RULE(SXCL_REASON_REMOTE_DESKTOP, 70, kAnyRemote),
    SXCL_RULE(SXCL_REASON_VULKAN_UNAVAILABLE, 68, kAnyVulkanNo),
    SXCL_RULE(SXCL_REASON_PATH_NOT_FOUND, 65, kAnyPathMissing),
    SXCL_RULE(SXCL_REASON_CLASSPATH_BROKEN, 63, kAnyClasspath),
    SXCL_RULE(SXCL_REASON_MISSING_ASSET, 62, kAnyMissAsset),
    SXCL_RULE(SXCL_REASON_MISSING_CLASS, 60, kAnyMissClass),
    SXCL_RULE(SXCL_REASON_GRAPHICS_DRIVER, 60, kAnyGfxGeneric),
    SXCL_RULE(SXCL_REASON_NET_TIMEOUT, 58, kAnyNetTimeout),
    SXCL_RULE(SXCL_REASON_SSL_FAILURE, 57, kAnySsl),
    SXCL_RULE(SXCL_REASON_DNS_FAILURE, 56, kAnyDns),
    SXCL_RULE(SXCL_REASON_NETWORK_UNREACHABLE, 55, kAnyNetDown),
    SXCL_RULE(SXCL_REASON_SERVER_OFFLINE, 54, kAnyServerDown),
    SXCL_RULE_ALL(SXCL_REASON_ANDROID_NOEXEC, 52, kAnySharedDenied, kAllSharedDenied),
    SXCL_RULE(SXCL_REASON_VULKAN_FALLBACK, 40, kAnyVulkanFallback),
    SXCL_RULE(SXCL_REASON_CRASH_UNKNOWN, 20, kAnyCrash),
    SXCL_RULE(SXCL_REASON_EXIT_OK, 10, kAnyExitOk),
};

#undef SXCL_RULE
#undef SXCL_RULE_ALL

/** 规则表的"门槛":不是所有行都值得进原因表。
 *  典型反例:"OpenGL Renderer: Intel(R) UHD Graphics 630" 是**环境信息**,
 *  把它当成"核显驱动问题"会让每次正常启动都报一个不存在的原因。
 *  所以:级别 >= WARN 的行直接进;级别是 INFO 的行必须自己带"出事了"的词。 */
static const char *const kTroubleWords[] = {
    "missing",  "not found",      "unsupported", "invalid",  "denied",     "refused",
    "cannot",   "could not",      "unable",      "fail",     "corrupt",    "no space",
    "not enough", "permission",   "conflict",    "incompatible", "without", "outdated",
    "expired",  "rejected",       "缺少",        "失败",     "错误",       "无法",
    "不支持",   "冲突",           "拒绝",        "超时",     "已满",       "不存在",
    NULL};

static int reason_gate(const char *text, int severity)
{
    if (severity >= 1) {
        return 1; /* WARN/ERROR:本来就是在说问题 */
    }
    return contains_any(text, kTroubleWords, SXCL_RULE_N(kTroubleWords));
}

static sxcl_log_reason classify_line_ex(const char *text, int severity, int *score_out)
{
    size_t i = 0;
    sxcl_log_reason best = SXCL_REASON_UNKNOWN;
    int best_score = -1;
    if (text == NULL || *text == '\0') {
        if (score_out != NULL) {
            *score_out = -1;
        }
        return SXCL_REASON_UNKNOWN;
    }
    /* 门槛之外还有一个例外:"正常退出"是**好消息**,不该被门槛挡掉(它本来就不带问题词);
     * 而它在表里的分数最低(10),任何真故障都会盖过它。 */
    if (!reason_gate(text, severity) && !contains_any(text, kAnyExitOk, SXCL_RULE_N(kAnyExitOk))) {
        if (score_out != NULL) {
            *score_out = -1;
        }
        return SXCL_REASON_UNKNOWN;
    }
    for (i = 0; i < SXCL_RULE_N(kReasonRules); ++i) {
        const sxcl_reason_rule *rule = &kReasonRules[i];
        size_t k = 0;
        int hit = 0;
        if (rule->score <= best_score) {
            continue; /* 已经有更具体的了:同分也保持表里靠前的 */
        }
        for (k = 0; k < rule->any_count && !hit; ++k) {
            if (icontains(text, rule->any[k])) {
                hit = 1;
            }
        }
        if (!hit) {
            continue;
        }
        for (k = 0; k < rule->all_count; ++k) {
            if (!icontains(text, rule->all[k])) {
                hit = 0;
                break;
            }
        }
        if (!hit) {
            continue;
        }
        best = rule->reason;
        best_score = rule->score;
    }
    if (score_out != NULL) {
        *score_out = best_score;
    }
    return best;
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
    /* 原因键(第 3b 节):与类别各判各的 —— 类别回答"这行属于哪一类",
     * 原因键回答"用户该改什么"。两者都记,汇总时各取所需。 */
    local.reason_kind = classify_line_ex(text, local.severity, NULL);
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

/* ────────────────────────── 原因键(第 3b 节)────────────────────────── */

size_t sxcl_log_reason_count(void)
{
    return (size_t)SXCL_REASON_TABLE_COUNT;
}

sxcl_log_reason sxcl_log_reason_at(size_t index)
{
    if (index >= (size_t)SXCL_REASON_TABLE_COUNT) {
        return SXCL_REASON_UNKNOWN;
    }
    return kReasonZhCn[index].reason;
}

const char *sxcl_log_reason_key(sxcl_log_reason reason)
{
    if ((size_t)reason >= (size_t)SXCL_REASON_TABLE_COUNT) {
        return kReasonZhCn[SXCL_REASON_UNKNOWN].key;
    }
    return kReasonZhCn[(size_t)reason].key;
}

/** 语言是不是英文(认 "en"/"en-us"/"en_US";其余一律中文)。 */
static int reason_is_english(const char *lang)
{
    if (lang == NULL) {
        const sxcl_lang *def = sxcl_lang_default();
        lang = (def != NULL) ? sxcl_lang_code(def) : NULL;
    }
    if (lang == NULL || lang[0] == '\0') {
        return 0;
    }
    return (lang[0] == 'e' || lang[0] == 'E') && (lang[1] == 'n' || lang[1] == 'N') ? 1 : 0;
}

static const char *reason_field(sxcl_log_reason reason, int want_advice, const char *lang)
{
    const sxcl_reason_kv *row = NULL;
    const sxcl_lang *def = sxcl_lang_default();
    if ((size_t)reason >= (size_t)SXCL_REASON_TABLE_COUNT) {
        reason = SXCL_REASON_UNKNOWN; /* 越界/COUNT:按"未知"给文案,绝不给空串 */
    }
    row = reason_is_english(lang) ? &kReasonEnUs[(size_t)reason] : &kReasonZhCn[(size_t)reason];
    if (def != NULL) {
        /* 用户语言包里可以逐条覆盖:crash.reason.<键> / crash.reason.<键>.advice */
        char key[96];
        if (want_advice) {
            (void)snprintf(key, sizeof(key), "crash.reason.%s.advice", row->key);
        } else {
            (void)snprintf(key, sizeof(key), "crash.reason.%s", row->key);
        }
        return sxcl_lang_get(def, key, want_advice ? row->advice : row->name);
    }
    return want_advice ? row->advice : row->name;
}

const char *sxcl_log_reason_name(sxcl_log_reason reason, const char *lang)
{
    const char *text = reason_field(reason, 0, lang);
    return (text != NULL && *text != '\0') ? text : kReasonZhCn[SXCL_REASON_UNKNOWN].name;
}

const char *sxcl_log_reason_advice(sxcl_log_reason reason, const char *lang)
{
    const char *text = reason_field(reason, 1, lang);
    return (text != NULL && *text != '\0') ? text : kReasonZhCn[SXCL_REASON_UNKNOWN].advice;
}

int sxcl_log_reason_text(sxcl_log_reason reason, const char *lang, char *out, size_t out_cap)
{
    int n = 0;
    if (out == NULL || out_cap == 0u) {
        return -1;
    }
    n = snprintf(out, out_cap, "%s:%s", sxcl_log_reason_name(reason, lang),
                 sxcl_log_reason_advice(reason, lang));
    if (n < 0) {
        out[0] = '\0';
        return -1;
    }
    return n;
}

sxcl_log_reason sxcl_log_classify_reason(const char *line)
{
    return classify_line_ex(line, severity_of(line != NULL ? line : ""), NULL);
}

sxcl_log_conclusion sxcl_log_reason_conclusion(sxcl_log_reason reason)
{
    switch (reason) {
    case SXCL_REASON_EXIT_OK:
        return SXCL_LOG_CONCLUSION_OK;
    case SXCL_REASON_VULKAN_FALLBACK:
        return SXCL_LOG_CONCLUSION_VULKAN_FALLBACK;
    case SXCL_REASON_JAVA_NOT_FOUND:
    case SXCL_REASON_JAVA_BROKEN:
    case SXCL_REASON_JAVA_VERSION_MISMATCH:
        return SXCL_LOG_CONCLUSION_JAVA;
    case SXCL_REASON_MOD_DUPLICATE:
    case SXCL_REASON_MOD_RESOLUTION_CONFLICT:
    case SXCL_REASON_MOD_MISSING_DEPENDENCY:
    case SXCL_REASON_MOD_VERSION_MISMATCH:
    case SXCL_REASON_MOD_FILE_CORRUPTED:
    case SXCL_REASON_MOD_LOADER_MISSING:
    case SXCL_REASON_MOD_LOADER_VERSION:
    case SXCL_REASON_MIXIN_FAILURE:
    case SXCL_REASON_OPTIFINE_CONFLICT:
    case SXCL_REASON_SHADER_FAILURE:
    case SXCL_REASON_MOD_CRASH:
        return SXCL_LOG_CONCLUSION_MOD;
    case SXCL_REASON_MISSING_JAVA_LIBRARY:
    case SXCL_REASON_MISSING_CLASS:
    case SXCL_REASON_MISSING_ASSET:
    case SXCL_REASON_NATIVES_EXTRACT_FAILED:
    case SXCL_REASON_CORRUPT_JAR:
    case SXCL_REASON_CLASSPATH_BROKEN:
        return SXCL_LOG_CONCLUSION_MISSING;
    case SXCL_REASON_ACCOUNT_INVALID_SESSION:
    case SXCL_REASON_ACCOUNT_AUTH_FAILED:
    case SXCL_REASON_NETWORK_UNREACHABLE:
    case SXCL_REASON_DNS_FAILURE:
    case SXCL_REASON_SSL_FAILURE:
    case SXCL_REASON_NET_TIMEOUT:
    case SXCL_REASON_SERVER_OFFLINE:
        return SXCL_LOG_CONCLUSION_ACCOUNT;
    case SXCL_REASON_GRAPHICS_DRIVER:
    case SXCL_REASON_GRAPHICS_DRIVER_INTEL:
    case SXCL_REASON_GRAPHICS_DRIVER_NVIDIA:
    case SXCL_REASON_GRAPHICS_DRIVER_AMD:
    case SXCL_REASON_GRAPHICS_DRIVER_MESA:
    case SXCL_REASON_GRAPHICS_DRIVER_ADRENO:
    case SXCL_REASON_GRAPHICS_DRIVER_MALI:
    case SXCL_REASON_GRAPHICS_DRIVER_POWERVR:
    case SXCL_REASON_GRAPHICS_DRIVER_SOFTWARE:
    case SXCL_REASON_GRAPHICS_DRIVER_OUTDATED:
    case SXCL_REASON_OPENGL_TOO_LOW:
    case SXCL_REASON_GLFW_INIT_FAILED:
    case SXCL_REASON_PIXEL_FORMAT_FAILED:
    case SXCL_REASON_NO_GL_CONTEXT:
    case SXCL_REASON_REMOTE_DESKTOP:
    case SXCL_REASON_GPU_DRIVER_CRASH:
    case SXCL_REASON_VULKAN_UNAVAILABLE:
        return SXCL_LOG_CONCLUSION_GRAPHICS;
    case SXCL_REASON_UNKNOWN:
        return SXCL_LOG_CONCLUSION_UNKNOWN;
    default:
        break; /* 其余(内存/JVM/权限/平台类)都归到"崩溃"这一大类 */
    }
    return SXCL_LOG_CONCLUSION_CRASH;
}

/** 结论 -> 最接近的原因键(规则一条都没命中时的兜底)。 */
static sxcl_log_reason reason_from_conclusion(sxcl_log_conclusion conclusion)
{
    switch (conclusion) {
    case SXCL_LOG_CONCLUSION_OK:
        return SXCL_REASON_EXIT_OK;
    case SXCL_LOG_CONCLUSION_GRAPHICS:
        return SXCL_REASON_GRAPHICS_DRIVER;
    case SXCL_LOG_CONCLUSION_VULKAN_FALLBACK:
        return SXCL_REASON_VULKAN_FALLBACK;
    case SXCL_LOG_CONCLUSION_JAVA:
        return SXCL_REASON_JAVA_VERSION_MISMATCH;
    case SXCL_LOG_CONCLUSION_MOD:
        return SXCL_REASON_MOD_CRASH;
    case SXCL_LOG_CONCLUSION_MISSING:
        return SXCL_REASON_MISSING_CLASS;
    case SXCL_LOG_CONCLUSION_ACCOUNT:
        return SXCL_REASON_NETWORK_UNREACHABLE;
    case SXCL_LOG_CONCLUSION_CRASH:
        return SXCL_REASON_CRASH_UNKNOWN;
    case SXCL_LOG_CONCLUSION_UNKNOWN:
    default:
        return SXCL_REASON_UNKNOWN;
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
    summary->reason = SXCL_REASON_UNKNOWN;
    summary->reason_score = -1;
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
    /* 原因键:取**最具体**的一条(分数最高);同分保留先命中的那条 */
    if (info.reason_kind != SXCL_REASON_UNKNOWN) {
        int score = -1;
        (void)classify_line_ex(line, info.severity, &score);
        if (score > summary->reason_score) {
            summary->reason_score = score;
            summary->reason = info.reason_kind;
        }
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

    /* ── 原因键:规则没命中时按结论给一个最接近的键,再套文案 ── */
    if (summary->reason == SXCL_REASON_UNKNOWN) {
        summary->reason = reason_from_conclusion(summary->conclusion);
    }
    copy_str(summary->reason_key, sizeof(summary->reason_key),
             sxcl_log_reason_key(summary->reason));
    copy_str(summary->reason_name, sizeof(summary->reason_name),
             sxcl_log_reason_name(summary->reason, NULL));
    copy_str(summary->reason_advice, sizeof(summary->reason_advice),
             sxcl_log_reason_advice(summary->reason, NULL));
    if (summary->reason != SXCL_REASON_UNKNOWN && summary->reason_name[0] != '\0') {
        /* 有原因键:用"短名:可执行建议"覆盖笼统的类别文案(旧文案作兜底保留在 buf 里) */
        size_t used = (size_t)snprintf(summary->advice, sizeof(summary->advice), "%s:%s",
                                       summary->reason_name, summary->reason_advice);
        if (used == 0u) {
            copy_str(summary->advice, sizeof(summary->advice), buf);
        }
    } else {
        copy_str(summary->advice, sizeof(summary->advice), buf);
    }
    if (summary->advice[0] == '\0') {
        copy_str(summary->advice, sizeof(summary->advice), buf);
    }
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
