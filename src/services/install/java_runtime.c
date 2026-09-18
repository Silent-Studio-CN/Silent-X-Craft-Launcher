/* SXCL-C 官方 Java 运行时(JRE/JDK)安装实现 —— 见 include/sxcl/java_runtime.h 的语义约定。
 *
 * 参考实现与出处(行为对齐):
 *   * Python 版 src/services/java/mojang_runtime.py(本文件每一步都能指回它的行号);
 *   * HMCL(GPLv3,HMCLCore/.../download/java/mojang/MojangJavaDownloadTask.java 与
 *     HMCL/.../java/JavaManager.getMojangJavaPlatform())。本文件按它们描述的**协议与行为**
 *     用 C 重写,没有拷贝 Java 代码;从 HMCL 取的只有两点:平台键的取值集合(含 windows-x86
 *     与 mac-os-arm64),以及"raw 与 lzma 同时存在时两者都可选"。我们只走 raw —— 官方清单里
 *     raw 一直在,省一个 LZMA 解码器(差异写进 docs/10-Java安装与国际化.md)。
 *
 * 这里**没有**自己的下载循环:清单走 sxcl_http.h(带期望 SHA-1),文件走 sxcl_engine
 * (断点续传/换源/逐文件 SHA-1/已存在快路径)。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/java_runtime.h"

#include "sxcl/fs.h"
#include "sxcl/hash.h"
#include "sxcl/http.h"
#include "sxcl/lang.h"       /* 进度文案随语言走(java.* 键) */
#include "sxcl/settings.h"   /* sxcl_settings_default_dir(runtime 根与配置目录同源) */
#include "sxcl/verify.h"

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
#  include <sys/stat.h>   /* chmod(可执行位) */
#endif

/* 路径统一用 '/' 当分隔符:Windows 侧 fs 层两种都认(platform_win32.c:110-115 把 '/' 归一成 '\\'),
 * 清单里的相对路径本来就是 '/'。这样 join 一处就够,不用按平台分叉。 */
#define JR_SEP "/"

/* ── 小工具 ── */

static void set_text(char *err, size_t err_len, const char *text)
{
    if (err && err_len) {
        snprintf(err, err_len, "%s", text ? text : "");
    }
}

static void copy_str(char *out, size_t out_len, const char *src)
{
    if (!out || out_len == 0) {
        return;
    }
    const size_t n = src ? strlen(src) : 0;
    const size_t take = n < out_len - 1 ? n : out_len - 1;
    if (take) {
        memcpy(out, src, take);
    }
    out[take] = '\0';
}

/* dir + "/" + leaf;装不下返回 -1。 */
static int join_path(char *out, size_t out_len, const char *dir, const char *leaf)
{
    if (!out || !dir || !leaf) {
        return -1;
    }
    const size_t dlen = strlen(dir);
    const size_t llen = strlen(leaf);
    const int need_sep = (dlen > 0 && dir[dlen - 1] == '/') ? 0 : 1;
    if (dlen + (size_t)need_sep + llen + 1 > out_len) {
        return -1;
    }
    memcpy(out, dir, dlen);
    size_t n = dlen;
    if (need_sep) {
        out[n++] = '/';
    }
    memcpy(out + n, leaf, llen + 1);
    return 0;
}

/* UTF-8 环境变量(与 settings.c / lang.c 里那份是同一个平台垫片)。
 * 各文件自带一份是**故意的**:共享就得抽内部头,而本次改动允许扩的头只有 java_runtime/lang/settings。 */
static const char *env_utf8(const char *name)
{
    if (!name || !*name) {
        return NULL;
    }
#if defined(_WIN32)
    static char buf[SXCL_JAVA_RUNTIME_PATH_MAX];
    wchar_t name_w[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, name_w, 64) <= 0) {
        return NULL;
    }
    const wchar_t *value = _wgetenv(name_w);
    if (!value || !*value) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, buf, (int)sizeof(buf), NULL, NULL) <= 0) {
        return NULL;
    }
    return buf;
#else
    const char *value = getenv(name);
    return (value && *value) ? value : NULL;
#endif
}

/* ── 返回码 / 阶段名 ── */

const char *sxcl_java_runtime_code_name(int code)
{
    switch (code) {
    case SXCL_JAVA_RUNTIME_OK:              return "ok";
    case SXCL_JAVA_RUNTIME_ERR_ARG:         return "arg";
    case SXCL_JAVA_RUNTIME_ERR_NET:         return "net";
    case SXCL_JAVA_RUNTIME_ERR_MANIFEST:    return "manifest";
    case SXCL_JAVA_RUNTIME_ERR_DOWNLOAD:    return "download";
    case SXCL_JAVA_RUNTIME_ERR_FINISH:      return "finish";
    case SXCL_JAVA_RUNTIME_ERR_CANCELLED:   return "cancelled";
    case SXCL_JAVA_RUNTIME_ERR_NOMEM:       return "nomem";
    case SXCL_JAVA_RUNTIME_ERR_IO:          return "io";
    default:                                return "unknown";
    }
}

const char *sxcl_java_runtime_stage_id(sxcl_java_runtime_stage stage)
{
    switch (stage) {
    case SXCL_JAVA_RUNTIME_STAGE_QUERY:    return "query";
    case SXCL_JAVA_RUNTIME_STAGE_MANIFEST: return "manifest";
    case SXCL_JAVA_RUNTIME_STAGE_DOWNLOAD: return "download";
    case SXCL_JAVA_RUNTIME_STAGE_FINISH:   return "finish";
    case SXCL_JAVA_RUNTIME_STAGE_END:      return "none";
    default:                               return "unknown";
    }
}

const char *sxcl_java_runtime_stage_name(sxcl_java_runtime_stage stage)
{
    switch (stage) {
    case SXCL_JAVA_RUNTIME_STAGE_QUERY:    return "获取组件清单";
    case SXCL_JAVA_RUNTIME_STAGE_MANIFEST: return "下载组件文件清单";
    case SXCL_JAVA_RUNTIME_STAGE_DOWNLOAD: return "下载并校验文件";
    case SXCL_JAVA_RUNTIME_STAGE_FINISH:   return "收尾";
    case SXCL_JAVA_RUNTIME_STAGE_END:      return "未失败";
    default:                               return "未知阶段";
    }
}

/* ── 平台键 ── */

/* 编译期目标架构。为什么用编译期而不是运行时探测:JRE 必须匹配**这个进程**能执行的架构 ——
 * Windows on ARM 上跑 x64 启动器就该拿 windows-x64(HMCL 也是按 SYSTEM_ARCH 判的,
 * 只是它在 ARM64 上还会额外考虑 x86_64 转译这条路)。 */
static const char *build_arch(void)
{
#if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__) || defined(__arm__)
    return "x86";
#else
    return "x64";
#endif
}

const char *sxcl_java_runtime_platform_key_for(sxcl_java_os os, const char *arch)
{
    const char *a = arch ? arch : build_arch();
    const int is_arm64 = (strcmp(a, "arm64") == 0);
    const int is_x86 = (strcmp(a, "x86") == 0 || strcmp(a, "i386") == 0 || strcmp(a, "arm32") == 0);
    switch (os) {
    case SXCL_JAVA_OS_WINDOWS:
        return is_arm64 ? "windows-arm64" : (is_x86 ? "windows-x86" : "windows-x64");
    case SXCL_JAVA_OS_MACOS:
        return is_arm64 ? "mac-os-arm64" : "mac-os";
    case SXCL_JAVA_OS_LINUX:
    case SXCL_JAVA_OS_ANDROID:
    default:
        /* Android 归 linux(launch.h 的 rules 口径;官方清单里也没有 android 这个平台键) */
        return is_x86 ? "linux-i386" : "linux";
    }
}

const char *sxcl_java_runtime_platform_key(void)
{
    return sxcl_java_runtime_platform_key_for(sxcl_java_current_os(), NULL);
}

/* ── 需要的 Java 主版本(Python required_java_major,mojang_runtime.py:88-109) ── */

int sxcl_java_runtime_required_major(const char *mc_version)
{
    if (!mc_version || !*mc_version) {
        return 21;
    }
    int parts[3] = { 0, 0, 0 };
    int filled = 0;
    const char *p = mc_version;
    for (int i = 0; i < 3; ++i) {
        if (*p < '0' || *p > '9') {
            return 21; /* 版本号里出现非数字:Python 同样是直接 return 21 */
        }
        int value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            if (value > 100000) {
                value = 100000; /* 防溢出;真实版本号远小于它 */
            }
            ++p;
        }
        parts[i] = value;
        ++filled;
        if (*p != '.') {
            break;
        }
        ++p;
    }
    if (filled == 0) {
        return 21;
    }
    const int major = parts[0];
    const int minor = parts[1];
    if (major >= 26) {
        return 25;
    }
    if (major >= 21) {
        return 21;
    }
    if (major == 1) {
        /* **与 Python 报表口径不同的一处(故意的,理由写在这)**:
         * Python required_java_major 只在 minor >= 21 时给 21,于是 1.20.5/1.20.6 会被判成 17 ——
         * 而官方从 1.20.5 起要求 Java 21(HMCL GameJavaVersion.getMinimumJavaVersion:
         * "1.20.5" -> JAVA_21)。按它下的 JRE 会装成 Java 17,游戏起不来。
         * 这里按 HMCL/官方口径把 1.20.5+ 判成 21;1.17 仍按 Python 给 17(16 也能跑,17 更省心;
         * HMCL 给的是 16)。差异见 docs/10-Java安装与国际化.md。 */
        if (minor > 20 || (minor == 20 && parts[2] >= 5)) {
            return 21;
        }
        if (minor >= 18) {
            return 17;
        }
        if (minor >= 17) {
            return 17;
        }
    }
    return 8;
}

/* ── 组件候选(Python COMPONENT_PREVIEW,mojang_runtime.py:65-69) ── */

static const sxcl_java_runtime_preset kPresets[] = {
    { "jre-legacy", "Java 8  (1.16.5 及更早)", 8 },
    { "java-runtime-gamma", "Java 17 (1.18 - 1.20.4)", 17 },
    { "java-runtime-delta", "Java 21 (1.20.5 - 1.21.x)", 21 },
};

size_t sxcl_java_runtime_preset_count(void) { return sizeof(kPresets) / sizeof(kPresets[0]); }

const sxcl_java_runtime_preset *sxcl_java_runtime_preset_at(size_t index)
{
    return index < sizeof(kPresets) / sizeof(kPresets[0]) ? &kPresets[index] : NULL;
}

/* ── 组件枚举与挑选 ── */

/* minecraft-java-exe 是启动器用的 exe,不是 JRE;gamma-snapshot 是快照版 */
static const char *const kExcluded[] = { "minecraft-java-exe", "java-runtime-gamma-snapshot" };

static int is_excluded(const char *component)
{
    for (size_t i = 0; i < sizeof(kExcluded) / sizeof(kExcluded[0]); ++i) {
        if (strcmp(component, kExcluded[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 从 "17.0.8" / "1.8.0_402" 取主版本(Python _java_major_of,mojang_runtime.py:112-120) */
static int java_major_of_name(const char *name)
{
    char buf[SXCL_JAVA_RUNTIME_VERSION_MAX];
    copy_str(buf, sizeof(buf), name ? name : "");
    const char *text = buf;
    if (strncmp(text, "1.", 2) == 0) {
        text += 2;
    }
    return sxcl_java_major_of(text);
}

static int fill_component(const sxcl_json_value *items, const char *component,
                          sxcl_java_runtime_component *out)
{
    const sxcl_json_value *entry = sxcl_json_at(items, 0);
    if (!entry) {
        return -1;
    }
    const sxcl_json_value *manifest = sxcl_json_get(entry, "manifest");
    const sxcl_json_value *version = sxcl_json_get(entry, "version");
    memset(out, 0, sizeof(*out));
    copy_str(out->component, sizeof(out->component), component);
    copy_str(out->manifest_url, sizeof(out->manifest_url), sxcl_json_get_string(manifest, "url", ""));
    copy_str(out->manifest_sha1, sizeof(out->manifest_sha1),
             sxcl_json_get_string(manifest, "sha1", ""));
    out->manifest_size = sxcl_json_get_int64(manifest, "size", 0);
    copy_str(out->version, sizeof(out->version), sxcl_json_get_string(version, "name", ""));
    copy_str(out->released, sizeof(out->released), sxcl_json_get_string(version, "released", ""));
    out->major = java_major_of_name(out->version);
    return out->manifest_url[0] ? 0 : -1;
}

size_t sxcl_java_runtime_components(const sxcl_json *all_json, const char *platform,
                                    sxcl_java_runtime_component *out, size_t cap)
{
    if (!all_json || !platform || !*platform || !out || cap == 0) {
        return 0;
    }
    const sxcl_json_value *plat = sxcl_json_get(sxcl_json_root(all_json), platform);
    if (!plat) {
        return 0;
    }
    size_t n = 0;
    const size_t members = sxcl_json_member_count(plat);
    for (size_t i = 0; i < members && n < cap; ++i) {
        const char *component = sxcl_json_member_key(plat, i);
        const sxcl_json_value *items = sxcl_json_member_value(plat, i);
        if (!component || !items || sxcl_json_size(items) == 0 || is_excluded(component)) {
            continue;
        }
        if (fill_component(items, component, &out[n]) == 0) {
            ++n;
        }
    }
    return n;
}

/* 官方推荐顺序(Python _PREFERRED_ORDER,mojang_runtime.py:127-130) */
static const char *const kPreferred[] = {
    "java-runtime-delta", "java-runtime-gamma", "java-runtime-epsilon",
    "jre-legacy", "java-runtime-beta", "java-runtime-alpha",
};

int sxcl_java_runtime_choose(const sxcl_json *all_json, const char *platform, int required_major,
                             sxcl_java_runtime_component *out)
{
    if (!all_json || !platform || !out) {
        return SXCL_JAVA_RUNTIME_ERR_ARG;
    }
    sxcl_java_runtime_component candidates[32];
    const size_t n = sxcl_java_runtime_components(all_json, platform, candidates, 32);
    if (n == 0) {
        return SXCL_JAVA_RUNTIME_ERR_MANIFEST;
    }
    const int required = required_major > 0 ? required_major : 21;

    /* 够用的里面取"主版本最小"的那一档(Python choose_component 的主路径)。
     * 一个都不够用时 Python 走 `or entries` 再取最小 —— 那会为"要 25"挑出 Java 8,明显是笔误;
     * 这里改成取**能拿到的最新**那档(最接近需求),差异写进 docs/10。 */
    int best_major = 0;
    int found = 0;
    for (size_t i = 0; i < n; ++i) {
        if (candidates[i].major >= required && candidates[i].major > 0 &&
            (!found || candidates[i].major < best_major)) {
            best_major = candidates[i].major;
            found = 1;
        }
    }
    if (!found) {
        for (size_t i = 0; i < n; ++i) {
            if (candidates[i].major > 0 && (!found || candidates[i].major > best_major)) {
                best_major = candidates[i].major;
                found = 1;
            }
        }
    }
    if (!found) {
        return SXCL_JAVA_RUNTIME_ERR_MANIFEST;
    }
    for (size_t p = 0; p < sizeof(kPreferred) / sizeof(kPreferred[0]); ++p) {
        for (size_t i = 0; i < n; ++i) {
            if (candidates[i].major == best_major &&
                strcmp(candidates[i].component, kPreferred[p]) == 0) {
                *out = candidates[i];
                return SXCL_JAVA_RUNTIME_OK;
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (candidates[i].major == best_major) {
            *out = candidates[i];
            return SXCL_JAVA_RUNTIME_OK;
        }
    }
    return SXCL_JAVA_RUNTIME_ERR_MANIFEST;
}

/* ── 清单抓取 ── */

/* 官方主机 -> BMCLAPI(同路径透传;mirror.py:65-81 的 _HOST_RULES 里与 JRE 有关的三条)。
 * 只换主机名,不动协议与路径 —— 写上前缀会拼出 https://https:// 这种坏地址。 */
static int mirror_url(const char *url, char *out, size_t out_len)
{
    static const char *const kHosts[] = {
        "launchermeta.mojang.com", "piston-meta.mojang.com", "piston-data.mojang.com",
    };
    if (!url || strncmp(url, "https://", 8) != 0) {
        return -1;
    }
    const char *host = url + 8;
    const char *slash = strchr(host, '/');
    const size_t host_len = slash ? (size_t)(slash - host) : strlen(host);
    for (size_t i = 0; i < sizeof(kHosts) / sizeof(kHosts[0]); ++i) {
        if (strlen(kHosts[i]) == host_len && strncmp(kHosts[i], host, host_len) == 0) {
            const int written = snprintf(out, out_len, "https://%s%s", SXCL_JAVA_RUNTIME_MIRROR_HOST,
                                         slash ? slash : "");
            return (written > 0 && (size_t)written < out_len) ? 0 : -1;
        }
    }
    return -1;
}

/* 取一段文本(两条候选:官方 + BMCLAPI)。expected_sha1 非空时强校验。 */
static int fetch_text(const sxcl_java_runtime_query *query, const char *url, const char *expected_sha1,
                      int use_mirror, char **out_text, size_t *out_len, char *err, size_t err_len)
{
    if (!query || !query->transport_factory) {
        set_text(err, err_len, "没有传输后端(transport_factory 为空)");
        return SXCL_JAVA_RUNTIME_ERR_ARG;
    }
    char mirror[SXCL_JAVA_RUNTIME_URL_MAX];
    const int has_mirror = use_mirror && mirror_url(url, mirror, sizeof(mirror)) == 0;
    const char *urls[2];
    urls[0] = url;
    urls[1] = has_mirror ? mirror : NULL;

    char detail[SXCL_JAVA_RUNTIME_ERROR_MAX];
    detail[0] = '\0';
    for (int i = 0; i < 2 && urls[i]; ++i) {
        sxcl_transport *tr = query->transport_factory(query->ud);
        if (!tr) {
            set_text(err, err_len, "传输后端创建失败");
            return SXCL_JAVA_RUNTIME_ERR_NET;
        }
        sxcl_http_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.expected_sha1 = (expected_sha1 && *expected_sha1) ? expected_sha1 : NULL;
        char http_err[SXCL_HTTP_ERROR_MAX];
        http_err[0] = '\0';
        char *text = NULL;
        size_t len = 0;
        const int rc = sxcl_http_get_text_ex(tr, urls[i], NULL, &opts, &text, &len, http_err,
                                             sizeof(http_err));
        if (tr->destroy) {
            tr->destroy(tr->ctx);
        }
        if (rc == SXCL_HTTP_OK) {
            *out_text = text;
            if (out_len) {
                *out_len = len;
            }
            return SXCL_JAVA_RUNTIME_OK;
        }
        free(text);
        if (detail[0] == '\0') {
            copy_str(detail, sizeof(detail), http_err);
        }
        /* SHA-1 不符 = 内容不对:镜像透传同一份字节,换条路没意义(报错更诚实) */
        if (rc == SXCL_HTTP_ERR_SHA1) {
            set_text(err, err_len, http_err[0] ? http_err : "清单 SHA-1 校验失败");
            return SXCL_JAVA_RUNTIME_ERR_MANIFEST;
        }
    }
    if (err && err_len) {
        snprintf(err, err_len, "下载失败: %s", detail[0] ? detail : url);
    }
    return SXCL_JAVA_RUNTIME_ERR_NET;
}

sxcl_json *sxcl_java_runtime_fetch_all(const sxcl_java_runtime_query *query, char *err, size_t err_len)
{
    if (!query) {
        set_text(err, err_len, "参数不合法");
        return NULL;
    }
    if (query->all_json_text) {
        sxcl_json *doc = sxcl_json_parse(query->all_json_text, strlen(query->all_json_text), err,
                                         err_len);
        if (!doc && err && err_len) {
            char detail[SXCL_JAVA_RUNTIME_ERROR_MAX];
            copy_str(detail, sizeof(detail), err);
            snprintf(err, err_len, "all.json 解析失败: %s", detail);
        }
        return doc;
    }
    const char *url = (query->all_json_url && *query->all_json_url) ? query->all_json_url
                                                                    : SXCL_JAVA_RUNTIME_MANIFEST_URL;
    char *text = NULL;
    size_t len = 0;
    if (fetch_text(query, url, NULL, 1, &text, &len, err, err_len) != SXCL_JAVA_RUNTIME_OK) {
        return NULL;
    }
    sxcl_json *doc = sxcl_json_parse(text, len, err, err_len);
    free(text);
    return doc;
}

sxcl_json *sxcl_java_runtime_fetch_manifest(const sxcl_java_runtime_query *query,
                                            const sxcl_java_runtime_component *entry,
                                            char *err, size_t err_len)
{
    if (!query || !entry) {
        set_text(err, err_len, "参数不合法");
        return NULL;
    }
    if (query->manifest_text) {
        return sxcl_json_parse(query->manifest_text, strlen(query->manifest_text), err, err_len);
    }
    if (!entry->manifest_url[0]) {
        set_text(err, err_len, "组件清单没有 url(all.json 结构不对?)");
        return NULL;
    }
    char *text = NULL;
    size_t len = 0;
    /* 清单本身必须过 SHA-1(Python fetch_component_manifest 也传了 manifest.sha1) */
    if (fetch_text(query, entry->manifest_url, entry->manifest_sha1, 1, &text, &len, err, err_len) !=
        SXCL_JAVA_RUNTIME_OK) {
        return NULL;
    }
    sxcl_json *doc = sxcl_json_parse(text, len, err, err_len);
    free(text);
    return doc;
}

int sxcl_java_runtime_list(const sxcl_java_runtime_query *query, sxcl_java_runtime_component *out,
                           size_t cap, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!query || !out || cap == 0) {
        set_text(err, err_len, "参数不合法");
        return SXCL_JAVA_RUNTIME_ERR_ARG;
    }
    const char *platform = (query->platform && *query->platform) ? query->platform
                                                                : sxcl_java_runtime_platform_key();
    sxcl_json *doc = sxcl_java_runtime_fetch_all(query, err, err_len);
    if (!doc) {
        return SXCL_JAVA_RUNTIME_ERR_NET;
    }
    const size_t n = sxcl_java_runtime_components(doc, platform, out, cap);
    sxcl_json_free(doc);
    if (n == 0) {
        if (err && err_len) {
            snprintf(err, err_len, "all.json 里没有平台 %s 的组件", platform);
        }
        return SXCL_JAVA_RUNTIME_ERR_MANIFEST;
    }
    return (int)n;
}

/* ── 默认安装根目录 ── */

int sxcl_java_runtime_default_root(char *out, size_t out_len, char *err, size_t err_len)
{
    if (!out || out_len == 0) {
        set_text(err, err_len, "参数不合法");
        return SXCL_JAVA_RUNTIME_ERR_ARG;
    }
    out[0] = '\0';
    const char *override_root = env_utf8("SXCL_RUNTIME_DIR");
    if (override_root && *override_root) {
        if (strlen(override_root) + 1 > out_len) {
            set_text(err, err_len, "SXCL_RUNTIME_DIR 太长");
            return SXCL_JAVA_RUNTIME_ERR_ARG;
        }
        copy_str(out, out_len, override_root);
        return SXCL_JAVA_RUNTIME_OK;
    }
    char config[768];
    char config_err[128];
    if (sxcl_settings_default_dir(config, sizeof(config), config_err, sizeof(config_err)) !=
        SXCL_SETTINGS_OK) {
        set_text(err, err_len, config_err);
        return SXCL_JAVA_RUNTIME_ERR_ARG;
    }
    if (join_path(out, out_len, config, "runtime") != 0) {
        set_text(err, err_len, "runtime 根目录路径太长");
        return SXCL_JAVA_RUNTIME_ERR_ARG;
    }
    return SXCL_JAVA_RUNTIME_OK;
}

/* ── 找已装的运行时 ── */

#if defined(_WIN32)
/* 列出 root 下的子目录(最多 cap 个)。返回条数;root 不存在/不是目录返回 0。 */
static size_t list_subdirs(const char *root, char (*out)[SXCL_JAVA_RUNTIME_PATH_MAX], size_t cap)
{
    const int need = MultiByteToWideChar(CP_UTF8, 0, root, -1, NULL, 0);
    if (need <= 0) {
        return 0;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wide) {
        return 0;
    }
    size_t found = 0;
    if (MultiByteToWideChar(CP_UTF8, 0, root, -1, wide, need) == need) {
        wchar_t pattern[MAX_PATH * 2];
        (void)swprintf(pattern, sizeof(pattern) / sizeof(pattern[0]), L"%ls\\*", wide);
        WIN32_FIND_DATAW data;
        HANDLE h = FindFirstFileW(pattern, &data);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                    wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
                    continue;
                }
                if (found >= cap) {
                    break;
                }
                char name[SXCL_JAVA_RUNTIME_PATH_MAX];
                if (WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, name, (int)sizeof(name), NULL,
                                        NULL) <= 0) {
                    continue;
                }
                if (join_path(out[found], SXCL_JAVA_RUNTIME_PATH_MAX, root, name) == 0) {
                    ++found;
                }
            } while (FindNextFileW(h, &data));
            FindClose(h);
        }
    }
    free(wide);
    return found;
}
#else
static size_t list_subdirs(const char *root, char (*out)[SXCL_JAVA_RUNTIME_PATH_MAX], size_t cap)
{
    DIR *dir = opendir(root);
    if (!dir) {
        return 0;
    }
    size_t found = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL && found < cap) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[SXCL_JAVA_RUNTIME_PATH_MAX];
        if (join_path(child, sizeof(child), root, entry->d_name) != 0) {
            continue;
        }
        struct stat st;
        if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        copy_str(out[found], SXCL_JAVA_RUNTIME_PATH_MAX, child);
        ++found;
    }
    closedir(d);
    return found;
}
#endif

int sxcl_java_runtime_java_path(const char *dir, char *out, size_t out_len)
{
    if (!dir || !*dir || !out || out_len == 0) {
        return SXCL_JAVA_RUNTIME_ERR_ARG;
    }
    char bin[768];
    if (join_path(bin, sizeof(bin), dir, "bin") != 0) {
        return SXCL_JAVA_RUNTIME_ERR_ARG;
    }
    if (join_path(out, out_len, bin, sxcl_java_exe_name(sxcl_java_current_os())) != 0) {
        return SXCL_JAVA_RUNTIME_ERR_ARG;
    }
    return SXCL_JAVA_RUNTIME_OK;
}

int sxcl_java_runtime_is_installed(const char *dir)
{
    char exe[SXCL_JAVA_RUNTIME_PATH_MAX];
    if (sxcl_java_runtime_java_path(dir, exe, sizeof(exe)) != SXCL_JAVA_RUNTIME_OK) {
        return 0;
    }
    return sxcl_fs_exists(exe) == 1 ? 1 : 0;
}

size_t sxcl_java_runtime_find(const char *target_root, sxcl_java_info *out, size_t cap)
{
    if (!target_root || !*target_root || !out || cap == 0) {
        return 0;
    }
    char root[768];
    if (strcmp(target_root, "@default") == 0) {
        char err[128];
        if (sxcl_java_runtime_default_root(root, sizeof(root), err, sizeof(err)) !=
            SXCL_JAVA_RUNTIME_OK) {
            return 0;
        }
    } else {
        copy_str(root, sizeof(root), target_root);
    }
    if (sxcl_fs_is_dir(root) != 1) {
        return 0;
    }
    char children[64][SXCL_JAVA_RUNTIME_PATH_MAX];
    const size_t n = list_subdirs(root, children, 64);
    size_t count = 0;
    for (size_t i = 0; i < n && count < cap; ++i) {
        if (!sxcl_java_runtime_is_installed(children[i])) {
            continue;
        }
        sxcl_java_info info;
        /* inspect 失败也收:路径与家目录有效、只是画像不全(major=0)——
         * 装了却读不出 release 的运行时照样让用户看得见,由调用方决定用不用。 */
        (void)sxcl_java_inspect(children[i], &info);
        copy_str(info.source, sizeof(info.source), "Runtime");
        out[count++] = info;
    }
    return count;
}

/* ── 安装 ── */

/* 清单里一个待下载文件(rel/url/sha1 都指向清单文档,文档活到安装结束)。 */
typedef struct jr_entry {
    const char *rel;
    const char *url;
    const char *sha1;
    int64_t size;
    int executable;
} jr_entry;

typedef struct jr_state {
    const sxcl_java_runtime_request *request;
    sxcl_java_runtime_result *out;
    sxcl_java_runtime_stage stage;
    int percent;
    const char *component;
    const char *version;
    const char *current;
    size_t files_total;
    size_t files_done;
    size_t files_skipped;
    size_t files_failed;
    int64_t bytes_total;
    int64_t bytes_done;
    int64_t bytes_skipped;   /**< 跳过分支确认掉的字节(引擎的 bytes_done 不含这部分) */

    /* 交给引擎的任务(取消与进度统计都要用) */
    sxcl_task *const *submitted;
    size_t submitted_count;
    unsigned char *settled;
    sxcl_engine *engine;
    int cancelled;
} jr_state;

static void emit(jr_state *st)
{
    if (!st->request->on_progress) {
        return;
    }
    sxcl_java_runtime_progress progress;
    memset(&progress, 0, sizeof(progress));
    progress.stage = st->stage;
    progress.stage_id = sxcl_java_runtime_stage_id(st->stage);
    progress.stage_name = sxcl_java_runtime_stage_name(st->stage);
    progress.percent = st->percent;
    progress.bytes_done = st->bytes_done;
    progress.bytes_total = st->bytes_total;
    progress.files_done = st->files_done;
    progress.files_total = st->files_total;
    progress.files_skipped = st->files_skipped;
    progress.files_failed = st->files_failed;
    progress.component = st->component ? st->component : "";
    progress.version = st->version ? st->version : "";
    progress.current = st->current ? st->current : "";
    /* 文案走语言表(java.* 键,与 Python 的 .lang 同一批键);没初始化 i18n 时就是中文兜底 */
    if (progress.current[0]) {
        snprintf(progress.message, sizeof(progress.message), "%s: %s",
                 sxcl_java_runtime_stage_name(st->stage), progress.current);
    } else if (st->version && st->version[0]) {
        char name[128];
        snprintf(name, sizeof(name), "Java %s", st->version);
        const char *const names[1] = { "name" };
        const char *const values[1] = { name };
        sxcl_lang_format(sxcl_lang_default(), "java.installing", "正在安装 {name}…", names, values, 1,
                         progress.message, sizeof(progress.message));
    } else {
        copy_str(progress.message, sizeof(progress.message),
                 sxcl_lang_tr("java.installing", "正在安装 Java 运行时…"));
    }
    st->request->on_progress(st->request->ud, &progress);
}

static int jr_cancelled(jr_state *st)
{
    if (st->cancelled) {
        return 1;
    }
    if (st->request->is_cancelled && st->request->is_cancelled(st->request->ud)) {
        st->cancelled = 1;
        return 1;
    }
    return 0;
}

/* 引擎进度回调(**工作线程**,下载中每 ~500ms 一次、每个任务落定必有一次)。
 * 这里顺手做两件事:统计落定文件、按用户取消叫停引擎。 */
static void engine_progress(void *ud, const sxcl_task *task)
{
    jr_state *st = (jr_state *)ud;
    if (task) {
        st->current = task->label;
        const int terminal = (task->state != SXCL_TASK_PENDING && task->state != SXCL_TASK_RUNNING);
        for (size_t i = 0; i < st->submitted_count; ++i) {
            if (st->submitted[i] != task) {
                continue;
            }
            if (terminal && !st->settled[i]) {
                st->settled[i] = 1;
                ++st->files_done;
                if (task->state == SXCL_TASK_FAILED) {
                    ++st->files_failed;
                }
                if (task->state == SXCL_TASK_CANCELLED) {
                    st->cancelled = 1;
                }
            }
            break;
        }
    }
    /* 已确认字节 = 跳过分支的 + 引擎里各任务当前的 bytes_done */
    int64_t done = st->bytes_skipped;
    for (size_t i = 0; i < st->submitted_count; ++i) {
        const sxcl_task *t = st->submitted[i];
        if (t->state != SXCL_TASK_PENDING && t->bytes_done > 0) {
            done += t->bytes_done;
        }
    }
    st->bytes_done = done;
    if (st->bytes_total > 0) {
        const int64_t share = st->bytes_done > st->bytes_total ? st->bytes_total : st->bytes_done;
        st->percent = 15 + (int)((share * 80) / st->bytes_total);
    }
    if (!st->cancelled && st->request->is_cancelled && st->request->is_cancelled(st->request->ud)) {
        st->cancelled = 1;
        if (st->engine) {
            sxcl_engine_cancel(st->engine); /* 引擎允许在自己的回调线程里被叫停(engine.c:838) */
        }
    }
    emit(st);
}

static int fail_stage(jr_state *st, int code, sxcl_java_runtime_stage stage)
{
    st->stage = stage;
    st->out->code = code;
    st->out->cancelled = (code == SXCL_JAVA_RUNTIME_ERR_CANCELLED) ? 1 : 0;
    st->out->fail_stage = stage;
    st->out->fail_stage_id = sxcl_java_runtime_stage_id(stage);
    st->out->files_total = st->files_total;
    st->out->files_done = st->files_done;
    st->out->files_skipped = st->files_skipped;
    st->out->files_failed = st->files_failed;
    st->out->bytes_done = st->bytes_done;
    st->out->bytes_total = st->bytes_total;
    return code;
}

/* JSON 字符串转义(标记文件里的 Windows 路径有反斜杠,必须转) */
static int json_escape(const char *text, char *out, size_t out_len)
{
    size_t n = 0;
    for (const char *p = text ? text : ""; *p; ++p) {
        char rep[8];
        const char *use = NULL;
        if (*p == '\\' || *p == '"') {
            rep[0] = '\\';
            rep[1] = *p;
            rep[2] = '\0';
            use = rep;
        } else if ((unsigned char)*p < 0x20) {
            (void)snprintf(rep, sizeof(rep), "\\u%04x", (unsigned char)*p);
            use = rep;
        }
        if (use) {
            const size_t len = strlen(use);
            if (n + len + 1 > out_len) {
                return -1;
            }
            memcpy(out + n, use, len);
            n += len;
        } else {
            if (n + 2 > out_len) {
                return -1;
            }
            out[n++] = *p;
        }
    }
    out[n] = '\0';
    return 0;
}

/* 写 .sxcl_runtime.json(Python 版认这个文件;字段与它逐字一致)。 */
static int write_marker(const char *target, const char *component, const char *platform,
                        const char *version, char *out_path, size_t out_path_len)
{
    char marker[800];
    if (join_path(marker, sizeof(marker), target, SXCL_JAVA_RUNTIME_MARKER) != 0) {
        return -1;
    }
    if (out_path && out_path_len) {
        copy_str(out_path, out_path_len, marker);
    }
    char esc_home[SXCL_JAVA_RUNTIME_PATH_MAX * 2];
    char esc_component[128];
    char esc_platform[64];
    char esc_version[128];
    if (json_escape(target, esc_home, sizeof(esc_home)) != 0 ||
        json_escape(component, esc_component, sizeof(esc_component)) != 0 ||
        json_escape(platform, esc_platform, sizeof(esc_platform)) != 0 ||
        json_escape(version, esc_version, sizeof(esc_version)) != 0) {
        return -1;
    }
    char body[4096];
    const int n = snprintf(body, sizeof(body),
                           "{\n"
                           "  \"component\": \"%s\",\n"
                           "  \"platform\": \"%s\",\n"
                           "  \"version\": \"%s\",\n"
                           "  \"java_home\": \"%s\"\n"
                           "}\n",
                           esc_component, esc_platform, esc_version, esc_home);
    if (n <= 0 || (size_t)n >= sizeof(body)) {
        return -1;
    }
    char tmp[820];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", marker) >= (int)sizeof(tmp)) {
        return -1;
    }
    FILE *fh = sxcl_fs_fopen(tmp, "wb");
    if (!fh) {
        return -1;
    }
    const int bad = fwrite(body, 1, (size_t)n, fh) == (size_t)n ? 0 : -1;
    if (fclose(fh) != 0 || bad != 0) {
        sxcl_fs_remove(tmp);
        return -1;
    }
    if (sxcl_fs_rename_replace(tmp, marker) != 0) {
        sxcl_fs_remove(tmp);
        return -1;
    }
    return 0;
}

#if !defined(_WIN32)
/* 给可执行条目补可执行位(Python 只对 executable=true 的条目 chmod;Windows 没这个概念)。 */
static void add_exec_bits(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return;
    }
    (void)chmod(path, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
}
#endif

int sxcl_java_runtime_install(const sxcl_java_runtime_request *request, sxcl_java_runtime_result *out)
{
    if (!request || !out) {
        return SXCL_JAVA_RUNTIME_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->fail_stage = SXCL_JAVA_RUNTIME_STAGE_END;
    out->fail_stage_id = sxcl_java_runtime_stage_id(SXCL_JAVA_RUNTIME_STAGE_END);

    char err[SXCL_JAVA_RUNTIME_ERROR_MAX];
    err[0] = '\0';

    /* 参数校验:清单来源与传输后端一个都没有 -> 压根不该开始(比"跑到一半报网络错"诚实) */
    if (!request->all_json_text && !request->transport_factory) {
        memset(out, 0, sizeof(*out));
        out->fail_stage = SXCL_JAVA_RUNTIME_STAGE_END;
        out->fail_stage_id = sxcl_java_runtime_stage_id(SXCL_JAVA_RUNTIME_STAGE_END);
        out->code = SXCL_JAVA_RUNTIME_ERR_ARG;
        copy_str(out->error, sizeof(out->error),
                 "既没有 all_json_text 也没有 transport_factory(不知道去哪儿取组件清单)");
        return SXCL_JAVA_RUNTIME_ERR_ARG;
    }

    jr_state st;
    memset(&st, 0, sizeof(st));
    st.request = request;
    st.out = out;
    st.stage = SXCL_JAVA_RUNTIME_STAGE_QUERY;

    const char *platform = (request->platform && *request->platform) ? request->platform
                                                                    : sxcl_java_runtime_platform_key();
    copy_str(out->platform, sizeof(out->platform), platform);

    sxcl_java_runtime_query query;
    memset(&query, 0, sizeof(query));
    query.all_json_text = request->all_json_text;
    query.all_json_url = request->all_json_url;
    query.manifest_text = request->manifest_text;
    query.platform = platform;
    query.transport_factory = request->transport_factory;
    query.ud = request->ud;

    jr_entry *entries = NULL;
    sxcl_task *tasks = NULL;
    char **dests = NULL;
    char **mirrors = NULL;
    sxcl_task **submitted = NULL;
    unsigned char *settled = NULL;
    sxcl_json *manifest = NULL;
    int rc = SXCL_JAVA_RUNTIME_OK;
    size_t file_count = 0;

    /* ── 1) all.json + 挑组件 ── */
    emit(&st);
    sxcl_java_runtime_component entry;
    memset(&entry, 0, sizeof(entry));
    {
        sxcl_json *all = sxcl_java_runtime_fetch_all(&query, err, sizeof(err));
        if (!all) {
            rc = SXCL_JAVA_RUNTIME_ERR_NET;
            snprintf(out->error, sizeof(out->error), "获取官方 JRE 清单失败: %s", err);
            return fail_stage(&st, rc, SXCL_JAVA_RUNTIME_STAGE_QUERY);
        }
        if (request->component && *request->component) {
            sxcl_java_runtime_component list[32];
            const size_t n = sxcl_java_runtime_components(all, platform, list, 32);
            int found = 0;
            for (size_t i = 0; i < n; ++i) {
                if (strcmp(list[i].component, request->component) == 0) {
                    entry = list[i];
                    found = 1;
                    break;
                }
            }
            if (!found) {
                sxcl_json_free(all);
                snprintf(out->error, sizeof(out->error), "平台 %s 上没有组件 %s", platform,
                         request->component);
                return fail_stage(&st, SXCL_JAVA_RUNTIME_ERR_MANIFEST, SXCL_JAVA_RUNTIME_STAGE_QUERY);
            }
        } else {
            const int required = request->required_major > 0
                                     ? request->required_major
                                     : sxcl_java_runtime_required_major(request->mc_version);
            rc = sxcl_java_runtime_choose(all, platform, required, &entry);
        }
        sxcl_json_free(all);
        if (rc != SXCL_JAVA_RUNTIME_OK) {
            snprintf(out->error, sizeof(out->error), "挑不出可用的 JRE 组件(平台 %s)", platform);
            return fail_stage(&st, SXCL_JAVA_RUNTIME_ERR_MANIFEST, SXCL_JAVA_RUNTIME_STAGE_QUERY);
        }
    }
    copy_str(out->component, sizeof(out->component), entry.component);
    copy_str(out->version, sizeof(out->version), entry.version);
    st.component = out->component;
    st.version = out->version;
    st.percent = 8;
    emit(&st);
    if (jr_cancelled(&st)) {
        copy_str(out->error, sizeof(out->error), sxcl_lang_tr("page.progress.cancel", "取消"));
        return fail_stage(&st, SXCL_JAVA_RUNTIME_ERR_CANCELLED, SXCL_JAVA_RUNTIME_STAGE_QUERY);
    }

    /* ── 2) 目标目录(组件定下来才能拼 <root>/<组件>-<平台>) ── */
    char target[SXCL_JAVA_RUNTIME_PATH_MAX];
    if (request->target_dir && *request->target_dir) {
        copy_str(target, sizeof(target), request->target_dir);
    } else {
        char root[SXCL_JAVA_RUNTIME_PATH_MAX];
        if (request->target_root && *request->target_root) {
            copy_str(root, sizeof(root), request->target_root);
        } else if (sxcl_java_runtime_default_root(root, sizeof(root), err, sizeof(err)) !=
                   SXCL_JAVA_RUNTIME_OK) {
            snprintf(out->error, sizeof(out->error), "拿不到 runtime 根目录: %s", err);
            return fail_stage(&st, SXCL_JAVA_RUNTIME_ERR_ARG, SXCL_JAVA_RUNTIME_STAGE_MANIFEST);
        }
        char leaf[SXCL_JAVA_RUNTIME_COMPONENT_MAX + SXCL_JAVA_RUNTIME_PLATFORM_MAX + 4];
        snprintf(leaf, sizeof(leaf), "%s-%s", entry.component, platform);
        if (join_path(target, sizeof(target), root, leaf) != 0) {
            snprintf(out->error, sizeof(out->error), "安装目录路径太长: %s", root);
            return fail_stage(&st, SXCL_JAVA_RUNTIME_ERR_ARG, SXCL_JAVA_RUNTIME_STAGE_MANIFEST);
        }
    }

    /* ── 3) 组件清单(manifest.sha1 强校验)+ 目标目录 + directory 条目 ── */
    st.stage = SXCL_JAVA_RUNTIME_STAGE_MANIFEST;
    st.percent = 10;
    emit(&st);
    manifest = sxcl_java_runtime_fetch_manifest(&query, &entry, err, sizeof(err));
    if (!manifest) {
        snprintf(out->error, sizeof(out->error), "组件清单下载或校验失败: %s", err);
        return fail_stage(&st, SXCL_JAVA_RUNTIME_ERR_MANIFEST, SXCL_JAVA_RUNTIME_STAGE_MANIFEST);
    }
    const sxcl_json_value *files = sxcl_json_get(sxcl_json_root(manifest), "files");
    const size_t members = files ? sxcl_json_member_count(files) : 0;
    if (members == 0) {
        rc = SXCL_JAVA_RUNTIME_ERR_MANIFEST;
        snprintf(out->error, sizeof(out->error), "组件清单里没有可下载的文件(%s)", entry.component);
        goto cleanup;
    }
    if (sxcl_fs_mkdirs(target) != 0) {
        rc = SXCL_JAVA_RUNTIME_ERR_IO;
        snprintf(out->error, sizeof(out->error), "无法创建安装目录: %s", target);
        goto cleanup;
    }
    for (size_t i = 0; i < members; ++i) {
        const sxcl_json_value *info = sxcl_json_member_value(files, i);
        const char *rel = sxcl_json_member_key(files, i);
        if (!rel || strcmp(sxcl_json_get_string(info, "type", ""), "directory") != 0) {
            continue;
        }
        char dir[SXCL_JAVA_RUNTIME_PATH_MAX];
        if (join_path(dir, sizeof(dir), target, rel) == 0) {
            (void)sxcl_fs_mkdirs(dir);
        }
    }

    /* ── 4) 收集待下载文件 ── */
    entries = (jr_entry *)calloc(members, sizeof(jr_entry));
    if (!entries) {
        rc = SXCL_JAVA_RUNTIME_ERR_NOMEM;
        snprintf(out->error, sizeof(out->error), "内存不足");
        goto cleanup;
    }
    for (size_t i = 0; i < members; ++i) {
        const sxcl_json_value *info = sxcl_json_member_value(files, i);
        const char *rel = sxcl_json_member_key(files, i);
        if (!rel || !info || strcmp(sxcl_json_get_string(info, "type", ""), "file") != 0) {
            continue;
        }
        const sxcl_json_value *raw = sxcl_json_get(sxcl_json_get(info, "downloads"), "raw");
        const char *url = sxcl_json_get_string(raw, "url", "");
        if (!url[0]) {
            continue; /* 没有 raw 的条目:跳过(官方清单里极少见);link 条目同理只走 raw */
        }
        entries[file_count].rel = rel;
        entries[file_count].url = url;
        entries[file_count].sha1 = sxcl_json_get_string(raw, "sha1", "");
        entries[file_count].size = sxcl_json_get_int64(raw, "size", 0);
        entries[file_count].executable = sxcl_json_get_bool(info, "executable", 0);
        ++file_count;
    }
    if (file_count == 0) {
        rc = SXCL_JAVA_RUNTIME_ERR_MANIFEST;
        snprintf(out->error, sizeof(out->error), "组件清单里没有可下载的文件(%s)", entry.component);
        goto cleanup;
    }

    /* ── 5) 建任务 + 跑引擎(已存在且校验通过的先跳过:一个字节都不下) ── */
    st.stage = SXCL_JAVA_RUNTIME_STAGE_DOWNLOAD;
    st.percent = 15;
    st.files_total = file_count;
    emit(&st);

    tasks = (sxcl_task *)calloc(file_count, sizeof(sxcl_task));
    dests = (char **)calloc(file_count, sizeof(char *));
    mirrors = (char **)calloc(file_count, sizeof(char *));
    submitted = (sxcl_task **)calloc(file_count, sizeof(sxcl_task *));
    settled = (unsigned char *)calloc(file_count, 1);
    if (!tasks || !dests || !mirrors || !submitted || !settled) {
        rc = SXCL_JAVA_RUNTIME_ERR_NOMEM;
        snprintf(out->error, sizeof(out->error), "内存不足");
        goto cleanup;
    }

    size_t submitted_count = 0;
    for (size_t i = 0; i < file_count; ++i) {
        char dest[SXCL_JAVA_RUNTIME_PATH_MAX];
        if (join_path(dest, sizeof(dest), target, entries[i].rel) != 0) {
            rc = SXCL_JAVA_RUNTIME_ERR_IO;
            snprintf(out->error, sizeof(out->error), "路径太长: %s/%s", target, entries[i].rel);
            goto cleanup;
        }
        /* 已存在且校验通过 -> 跳过(与引擎内部快路径同一个判定,统计才敢说"跳过 N 个") */
        const sxcl_verify_status vs =
            sxcl_verify_file(dest, entries[i].size, entries[i].sha1[0] ? entries[i].sha1 : NULL,
                             SXCL_HASH_SHA1, NULL);
        if (vs == SXCL_VERIFY_OK) {
            ++st.files_skipped;
            ++st.files_done;
            st.bytes_skipped += entries[i].size;
            continue;
        }
        dests[i] = (char *)malloc(strlen(dest) + 1);
        if (!dests[i]) {
            rc = SXCL_JAVA_RUNTIME_ERR_NOMEM;
            snprintf(out->error, sizeof(out->error), "内存不足");
            goto cleanup;
        }
        memcpy(dests[i], dest, strlen(dest) + 1);

        sxcl_task *task = &tasks[i];
        memset(task, 0, sizeof(*task));
        task->dest = dests[i];
        task->urls[0] = entries[i].url;
        if (request->use_mirror) {
            char mirror[SXCL_JAVA_RUNTIME_URL_MAX];
            if (mirror_url(entries[i].url, mirror, sizeof(mirror)) == 0) {
                mirrors[i] = (char *)malloc(strlen(mirror) + 1);
                if (mirrors[i]) {
                    memcpy(mirrors[i], mirror, strlen(mirror) + 1);
                    task->urls[1] = mirrors[i];
                }
            }
        }
        task->sha1 = entries[i].sha1[0] ? entries[i].sha1 : NULL;
        task->algo = SXCL_HASH_SHA1;
        task->size = entries[i].size;
        task->priority = 10;
        task->label = entries[i].rel;
        submitted[submitted_count++] = task;
        st.bytes_total += entries[i].size;
    }

    st.submitted = (sxcl_task *const *)submitted;
    st.submitted_count = submitted_count;
    st.settled = settled;
    st.bytes_done = st.bytes_skipped;
    st.percent = 15;
    emit(&st);

    if (submitted_count > 0) {
        sxcl_engine_opts opts;
        if (request->engine_opts) {
            opts = *request->engine_opts;
        } else {
            memset(&opts, 0, sizeof(opts));
        }
        opts.transport_factory = request->transport_factory;
        opts.userdata = &st;
        opts.on_progress = engine_progress;
        sxcl_engine *engine = sxcl_engine_create(&opts);
        if (!engine) {
            rc = SXCL_JAVA_RUNTIME_ERR_NOMEM;
            snprintf(out->error, sizeof(out->error), "创建下载引擎失败");
            goto cleanup;
        }
        st.engine = engine;
        for (size_t i = 0; i < submitted_count; ++i) {
            if (sxcl_engine_submit(engine, submitted[i]) != 0) {
                rc = SXCL_JAVA_RUNTIME_ERR_NOMEM;
                snprintf(out->error, sizeof(out->error), "任务入队失败(第 %d 个)", (int)(i + 1));
                break;
            }
        }
        if (rc == SXCL_JAVA_RUNTIME_OK) {
            (void)sxcl_engine_run(engine);
        }
        st.engine = NULL;
        sxcl_engine_destroy(engine); /* 任务由本函数持有,引擎必须在返回前放掉(engine.h 的生命周期契约) */
    }

    /* 引擎跑完:按任务状态收口(取消/失败要能分开数),失败要点名第一个出错的文件 */
    {
        int64_t done = st.bytes_skipped;
        for (size_t i = 0; i < submitted_count; ++i) {
            const sxcl_task *task = submitted[i];
            if (task->state != SXCL_TASK_PENDING && task->bytes_done > 0) {
                done += task->bytes_done;
            }
            if (task->state == SXCL_TASK_FAILED && out->error[0] == '\0') {
                snprintf(out->error, sizeof(out->error), "下载或校验失败: %s(%s)",
                         task->label ? task->label : "?", task->error);
            }
        }
        st.bytes_done = done;
    }
    if (st.cancelled || jr_cancelled(&st)) {
        snprintf(out->error, sizeof(out->error), "%s", sxcl_lang_tr("page.progress.cancel", "取消"));
        rc = SXCL_JAVA_RUNTIME_ERR_CANCELLED;
        goto cleanup;
    }
    if (rc != SXCL_JAVA_RUNTIME_OK) {
        goto cleanup;
    }
    if (st.files_failed > 0) {
        rc = SXCL_JAVA_RUNTIME_ERR_DOWNLOAD;
        goto cleanup;
    }

    /* ── 6) 收尾:可执行位 -> 核对 bin/java -> 标记文件 ── */
    st.stage = SXCL_JAVA_RUNTIME_STAGE_FINISH;
    st.percent = 96;
    st.current = NULL;
    emit(&st);

#if !defined(_WIN32)
    for (size_t i = 0; i < file_count; ++i) {
        if (!entries[i].executable) {
            continue;
        }
        char path[SXCL_JAVA_RUNTIME_PATH_MAX];
        if (join_path(path, sizeof(path), target, entries[i].rel) == 0) {
            add_exec_bits(path);
        }
    }
#endif

    copy_str(out->java_home, sizeof(out->java_home), target);
    if (sxcl_java_runtime_java_path(target, out->java_path, sizeof(out->java_path)) !=
        SXCL_JAVA_RUNTIME_OK) {
        rc = SXCL_JAVA_RUNTIME_ERR_IO;
        snprintf(out->error, sizeof(out->error), "拼不出 java 可执行文件路径: %s", target);
        goto cleanup;
    }
    if (!sxcl_fs_exists(out->java_path)) {
        /* Python: "❌ 安装完成但找不到 bin/java" —— 清单不完整或平台挑错了 */
        rc = SXCL_JAVA_RUNTIME_ERR_FINISH;
        snprintf(out->error, sizeof(out->error), "装完了却找不到 java 可执行文件: %s",
                 out->java_path);
        goto cleanup;
    }
    if (write_marker(target, entry.component, platform, entry.version, out->marker_path,
                     sizeof(out->marker_path)) != 0) {
        /* 标记文件写不了不算安装失败(不影响使用);Python 也只是 pass */
        copy_str(out->marker_path, sizeof(out->marker_path), "");
    }

    out->fail_stage = SXCL_JAVA_RUNTIME_STAGE_END;
    out->fail_stage_id = sxcl_java_runtime_stage_id(SXCL_JAVA_RUNTIME_STAGE_END);
    st.stage = SXCL_JAVA_RUNTIME_STAGE_FINISH;
    st.percent = 100;
    st.current = NULL;
    emit(&st);
    rc = SXCL_JAVA_RUNTIME_OK;

cleanup:
    for (size_t i = 0; i < file_count; ++i) {
        free(dests ? dests[i] : NULL);
        free(mirrors ? mirrors[i] : NULL);
    }
    free(tasks);
    free(dests);
    free(mirrors);
    free(submitted);
    free(settled);
    free(entries);
    sxcl_json_free(manifest);
    if (rc != SXCL_JAVA_RUNTIME_OK) {
        return fail_stage(&st, rc, st.stage);
    }
    out->code = SXCL_JAVA_RUNTIME_OK;
    out->cancelled = 0;
    out->files_total = st.files_total;
    out->files_done = st.files_done;
    out->files_skipped = st.files_skipped;
    out->files_failed = st.files_failed;
    out->bytes_done = st.bytes_done;
    out->bytes_total = st.bytes_total;
    return SXCL_JAVA_RUNTIME_OK;
}
