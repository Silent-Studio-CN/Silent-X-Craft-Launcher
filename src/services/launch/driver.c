/* 启动驱动 —— 把已经做好的几层串成"真的把游戏跑起来"这一条线。
 *
 * 依赖的都是已有模块,一层都不重造:
 *   sxcl/json.h      读版本 JSON            sxcl/launch.h(§1/§2/§3) 选 Java / 拼 argv / 归类日志
 *   sxcl/options.h   写 options.txt         sxcl/settings.h  每实例设置(渲染后端、上次实际后端)
 *   sxcl/process.h   起进程 + 逐行回调       sxcl/fs.h        建 natives 目录
 *
 * 顺序是有讲究的(每一步都对应一个真实的坑):
 *   1. 先读版本 JSON —— 没有它连该用哪个 Java 都不知道;
 *   2. 按 javaVersion.majorVersion 选 Java —— 选不出来就给"装哪个版本、去哪装"的人话,而不是抛个错误码;
 *   3. **启动前**把实例的渲染后端写进 options.txt —— options.h 的实测结论是:游戏在 Vulkan 起不来时
 *      会静默回退并把 options.txt 改成自己觉得合适的值,所以这个设置必须每次启动前由启动器重写;
 *   4. natives 目录先建好,并把 classifier jar 里的 .dll/.so 解进去 —— 目录空着等于
 *      LWJGL 加载原生库时抛 UnsatisfiedLinkError,启动参数拼得再对也没用;
 *   5. 拼 argv 起进程,on_line 同时喂给 logscan 与调用方(取消/提前收工都靠回调返回非 0);
 *   6. 结束后拿 logscan 的汇总当结论;如果设置的是 Vulkan 而日志出现回退,就明说"你选的 Vulkan 没生效,
 *      实际跑的是 OpenGL",并把实际后端写回 instance.<实例名>.lastGraphicsApi。
 *
 * 不做:资源补全。库/assets 缺了不该由启动驱动负责,但日志里抽到的"缺什么"会原样带在结果里。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/launch.h"

#include "sxcl/fs.h"
#include "sxcl/instance.h" /* sxcl_instance_read_json:PCL 式目录名与 id 不一致的版本也能启动 */
#include "sxcl/natives.h"
#include "sxcl/options.h"
#include "sxcl/process.h"
#include "sxcl/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ────────────────────────── 小工具 ────────────────────────── */

static char path_sep(void)
{
#if defined(_WIN32)
    return '\\';
#else
    return '/';
#endif
}

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

static const char *pick(const char *value, const char *fallback)
{
    return (value && *value) ? value : fallback;
}

static void join_path(char *out, size_t cap, const char *a, const char *b)
{
    const char sep = path_sep();
    size_t n = 0;
    size_t m = 0;
    size_t take = 0;
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
        m = strlen(b);
        take = (n + m < cap - 1) ? m : (cap - 1 - n);
        memcpy(out + n, b, take);
        out[n + take] = '\0';
    }
}

static void join3(char *out, size_t cap, const char *a, const char *b, const char *c)
{
    char tmp[SXCL_JAVA_PATH_MAX * 2];
    join_path(tmp, sizeof(tmp), a, b);
    join_path(out, cap, tmp, c);
}

static void set_error(sxcl_launch_result *out, char *err, size_t err_len, const char *text)
{
    /* 先落到本地缓冲:text 有可能就是 out->error 自己(调用点会把 error 再喂回来),
     * 直接 memcpy 到自己身上是自重叠,标准上不允许。 */
    char buf[256];
    copy_str(buf, sizeof(buf), text);
    if (out) {
        copy_str(out->error, sizeof(out->error), buf);
        copy_str(out->conclusion_text, sizeof(out->conclusion_text), buf);
    }
    if (err && err_len) {
        copy_str(err, err_len, buf);
    }
}

/* ────────────────────────── 逐行回调 ────────────────────────── */

typedef struct driver_line_ctx {
    sxcl_launch_result *res;
    const sxcl_launch_request *req;
} driver_line_ctx;

static int driver_on_line(void *userdata, int is_stderr, const char *line)
{
    driver_line_ctx *lc = (driver_line_ctx *)userdata;
    (void)sxcl_log_summary_add(&lc->res->log, line);
    if (lc->req->on_line) {
        return lc->req->on_line(lc->req->userdata, is_stderr, line);
    }
    return 0;
}

/* ────────────────────────── 选 Java ────────────────────────── */

/** 没找到 Java 时给一句能照着做的人话:要哪个版本、能装哪儿、怎么直接指定。 */
static void no_java_message(sxcl_launch_result *out, int required)
{
    char root0[SXCL_JAVA_PATH_MAX];
    char roots[8][SXCL_JAVA_PATH_MAX];
    const size_t n = sxcl_java_scan_roots(NULL, sxcl_java_current_os(), roots, 8);
    root0[0] = '\0';
    if (n > 0) {
        copy_str(root0, sizeof(root0), roots[0]);
    }
    (void)snprintf(out->error, sizeof(out->error),
                   "没找到可用的 Java 运行时:这个版本要求 Java %d。"
                   "请装一个 Java %d(或更新)的 JDK/JRE,或用 --java 直接指定 java 可执行文件,"
                   "或把 JAVA_HOME 指向它。常见的安装位置会去找:%s 等 %u 处。",
                   required, required, root0[0] ? root0 : "(本平台标准位置)", (unsigned)n);
    copy_str(out->conclusion_text, sizeof(out->conclusion_text), out->error);
    out->conclusion = SXCL_LOG_CONCLUSION_JAVA;
}

/* ────────────────────────── 主流程 ────────────────────────── */

int sxcl_launch_run(const sxcl_launch_request *req, sxcl_launch_result *out,
                    char *err, size_t err_len)
{
    char vjson[SXCL_JAVA_PATH_MAX * 2];
    char natives[SXCL_JAVA_PATH_MAX * 2];
    char options_path[SXCL_JAVA_PATH_MAX * 2];
    char versions_dir[SXCL_JAVA_PATH_MAX * 2];
    char errbuf[256];
    char leaf[SXCL_JAVA_PATH_MAX];
    sxcl_json *doc = NULL;
    sxcl_options *options = NULL;
    sxcl_settings *settings = NULL;
    sxcl_launch_args *args = NULL;
    sxcl_launch_ctx ctx;
    sxcl_java_info java;
    const char *instance = NULL;
    const char *requested = NULL;
    int required = 8;
    int rc = -1;

    if (err && err_len) {
        err[0] = '\0';
    }
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;
    out->java_is_64bit = -1;
    out->conclusion = SXCL_LOG_CONCLUSION_UNKNOWN;
    sxcl_log_summary_init(&out->log);

    if (!req) {
        set_error(out, err, err_len, "缺少启动请求");
        return -1;
    }
    if (!req->game_dir || !*req->game_dir) {
        set_error(out, err, err_len, "缺少游戏目录");
        return -1;
    }
    if (!req->version_name || !*req->version_name) {
        set_error(out, err, err_len, "缺少版本名");
        return -1;
    }
    copy_str(out->game_dir, sizeof(out->game_dir), req->game_dir);

    /* ── 1. 版本 JSON ──
     * 用实例扫描的读法而不是"只认 <版本名>/<版本名>.json":
     * PCL 式目录里常有"目录名与 JSON 里的 id 不一致"的版本(整合包/手改过的),只认同名文件会
     * 让这些实例**永远启动不了**。sxcl_instance_read_json 实现了 PCL 的兜底规则
     * (同名优先,否则取目录里任意含 mainClass+type+id 的 JSON),并把**实际读到的路径**回给我们,
     * 后面的 jar/参数都按这个路径来,保持自洽。 */
    join3(versions_dir, sizeof(versions_dir), req->game_dir, "versions", req->version_name);
    char read_err[192] = {0};
    doc = sxcl_instance_read_json(req->game_dir, req->version_name, vjson, sizeof(vjson), read_err,
                                  sizeof(read_err));
    if (!doc) {
        char msg[320];
        (void)snprintf(msg, sizeof(msg), "读不到版本 JSON:%s(%s)。这个版本可能没装好。", versions_dir,
                       read_err[0] ? read_err : errbuf);
        set_error(out, err, err_len, msg);
        goto done;
    }
    (void)leaf;

    /* ── 2. 选 Java:指定的优先,没指定才探测 ── */
    required = sxcl_java_required_major(doc);
    memset(&java, 0, sizeof(java));
    java.is_64bit = -1;
    java.is_jre = -1;
    if (req->java_path && *req->java_path) {
        if (sxcl_java_inspect(req->java_path, &java) != 0) {
            /* 用户明确指定了就用指定的:读不出 release 只说明"版本未知",不是错误。
             * 少了这条,包装脚本/测试用的假 java 就没法用 —— 而"用户说了算"本来就该是这个语义。 */
            char why[SXCL_JAVA_PATH_MAX];
            copy_str(why, sizeof(why), java.error);
            memset(&java, 0, sizeof(java));
            java.major = 0;
            java.is_64bit = -1;
            java.is_jre = -1;
            copy_str(java.path, sizeof(java.path), req->java_path);
            copy_str(java.error, sizeof(java.error), why[0] ? why : "读不出 release,按未知版本处理");
        }
    } else {
        sxcl_java_info found[16];
        size_t order[16];
        const size_t n = sxcl_java_discover(NULL, sxcl_java_current_os(), found, 16);
        const size_t ranked = sxcl_java_rank(found, n, required, order, 16);
        if (ranked == 0) {
            char msg[256];
            no_java_message(out, required);
            copy_str(msg, sizeof(msg), out->error);
            set_error(out, err, err_len, msg);
            goto done;
        }
        java = found[order[0]];
    }
    out->java_major = java.major;
    out->java_is_64bit = java.is_64bit;
    copy_str(out->java_path, sizeof(out->java_path), java.path);
    copy_str(out->java_version, sizeof(out->java_version), java.version);

    /* ── 3. 渲染后端:实例设置(或显式覆盖)-> 启动前写进 options.txt ── */
    instance = (req->instance && *req->instance) ? req->instance : req->version_name;
    if (req->settings_path && *req->settings_path) {
        settings = sxcl_settings_open(req->settings_path);
    }
    if (req->backend && *req->backend) {
        requested = req->backend;
    } else if (settings) {
        requested = sxcl_settings_instance_graphics_api(settings, instance);
    }
    if (!requested || !*requested) {
        requested = SXCL_LAUNCH_BACKEND_DEFAULT;
    }
    copy_str(out->requested_backend, sizeof(out->requested_backend), requested);
    copy_str(out->actual_backend, sizeof(out->actual_backend), requested);

    join_path(options_path, sizeof(options_path), req->game_dir, "options.txt");
    copy_str(out->options_path, sizeof(out->options_path), options_path);
    options = sxcl_options_load(options_path);
    if (!options) {
        char msg[256];
        (void)snprintf(msg, sizeof(msg),
                       "打不开 options.txt:%s。渲染后端必须由启动器在启动前写进去,"
                       "写不了就不启动 —— 否则用户会以为设置生效了。",
                       options_path);
        set_error(out, err, err_len, msg);
        goto done;
    }
    if (sxcl_options_set(options, SXCL_LAUNCH_GRAPHICS_KEY, requested) != 0 ||
        sxcl_options_save(options, options_path) != 0) {
        char msg[256];
        (void)snprintf(msg, sizeof(msg), "写不了 options.txt:%s(权限?)。", options_path);
        set_error(out, err, err_len, msg);
        goto done;
    }
    sxcl_options_free(options);
    options = NULL;

    /* ── 4. natives 目录 + 把原生库解出来 ──
     * 目录本身在拼参数前必须存在;里面有没有 .dll/.so 决定了 LWJGL 能不能加载 ——
     * 所以这一步失败就直接不启动(与 options.txt 写不进去同理:启动了也必然挂)。 */
    (void)snprintf(leaf, sizeof(leaf), "%s-natives", req->version_name);
    join_path(natives, sizeof(natives), versions_dir, leaf);
    if (sxcl_fs_mkdirs(natives) != 0) {
        char msg[256];
        (void)snprintf(msg, sizeof(msg), "建不了 natives 目录:%s(游戏会因为缺原生库直接崩)。", natives);
        set_error(out, err, err_len, msg);
        goto done;
    }
    copy_str(out->natives_dir, sizeof(out->natives_dir), natives);
    if (sxcl_natives_prepare_json(doc, req->game_dir, natives, errbuf, sizeof(errbuf)) != 0) {
        char msg[256];
        (void)snprintf(msg, sizeof(msg), "准备原生库失败:%s", errbuf);
        set_error(out, err, err_len, msg);
        goto done;
    }
    out->natives_count = sxcl_natives_last_count();

    /* ── 5. 拼 argv ── */
    memset(&ctx, 0, sizeof(ctx));
    /* 身份:有正版令牌就用正版那套,没有就退回离线那套(与从前完全一致)。
     * 判据是 access_token —— 没有它时 uuid/userType 说什么都没意义(游戏会去验档案)。 */
    const int online = (req->access_token != NULL && req->access_token[0] != '\0');
    ctx.player_name = pick(req->player_name, pick(req->offline_name, "Player"));
    ctx.uuid = pick(req->uuid, "00000000-0000-0000-0000-000000000000");
    ctx.access_token = pick(req->access_token, "0");
    ctx.user_type = pick(req->user_type, online ? "msa" : "legacy");
    ctx.xuid = req->xuid;
    ctx.client_id = req->client_id;
    ctx.version_name = req->version_name;
    ctx.game_directory = req->game_dir;
    ctx.natives_directory = natives;
    ctx.launcher_name = req->launcher_name;       /* NULL = 用默认 */
    ctx.launcher_version = req->launcher_version; /* NULL = 用默认 */
    ctx.memory_mb = req->memory_mb;
    ctx.java_major = java.major;
    ctx.is_64bit = java.is_64bit;
    ctx.os = SXCL_LAUNCH_OS_AUTO;
    args = sxcl_launch_build_args(doc, &ctx, errbuf, sizeof(errbuf));
    if (!args) {
        char msg[256];
        (void)snprintf(msg, sizeof(msg), "拼启动参数失败:%s", errbuf);
        set_error(out, err, err_len, msg);
        goto done;
    }

    if (req->dry_run) {
        (void)snprintf(out->conclusion_text, sizeof(out->conclusion_text),
                       "只准备不启动:Java 与参数都已就绪,options.txt 已写入 %s。", requested);
        rc = 0;
        goto done;
    }

    /* ── 6. 起进程:每一行同时喂给 logscan 与调用方回调 ── */
    {
        sxcl_process_opts popts;
        sxcl_process_result pr;
        driver_line_ctx lc;
        memset(&popts, 0, sizeof(popts));
        memset(&pr, 0, sizeof(pr));
        lc.res = out;
        lc.req = req;
        popts.program = out->java_path;
        popts.args = sxcl_launch_argv(args);
        popts.work_dir = req->game_dir;
        popts.timeout_ms = req->timeout_ms;
        popts.on_line = driver_on_line;
        popts.userdata = &lc;
        if (sxcl_process_run(&popts, &pr) != 0) {
            char msg[256];
            (void)snprintf(msg, sizeof(msg), "启动进程失败:%s", pr.error);
            set_error(out, err, err_len, msg);
            goto done;
        }
        out->started = 1;
        out->exit_code = pr.exit_code;
        out->timed_out = pr.timed_out;
        out->killed_by_client = pr.killed_by_client;
        out->elapsed_ms = pr.elapsed_ms;
    }

    /* ── 7. 结论 ── */
    sxcl_log_summary_finish(&out->log);
    out->conclusion = out->log.conclusion;
    copy_str(out->conclusion_text, sizeof(out->conclusion_text), out->log.advice);
    /* 缺什么原样带出去(不做补全,只报告) */
    copy_str(out->missing, sizeof(out->missing), out->log.missing);

    if (out->log.voted_vulkan_fallback) {
        copy_str(out->actual_backend, sizeof(out->actual_backend), SXCL_LAUNCH_BACKEND_OPENGL);
        if (strcmp(requested, SXCL_LAUNCH_BACKEND_VULKAN) == 0) {
            /* 这次是"用户选的 Vulkan"被设备拒绝了 —— 结论必须点名,而不是笼统说图形栈有问题 */
            out->vulkan_fell_back = 1;
            out->conclusion = SXCL_LOG_CONCLUSION_VULKAN_FALLBACK;
            (void)snprintf(out->conclusion_text, sizeof(out->conclusion_text),
                           "你选的 Vulkan 没生效:设备/驱动没有接受 Vulkan,游戏实际跑的是 OpenGL"
                           "(日志里出现了回退提示)。想稳定就用 OpenGL;想再试 Vulkan 先更新显卡驱动。");
        }
    }
    if (out->timed_out &&
        (out->conclusion == SXCL_LOG_CONCLUSION_OK || out->conclusion == SXCL_LOG_CONCLUSION_UNKNOWN)) {
        out->conclusion = SXCL_LOG_CONCLUSION_UNKNOWN;
        (void)snprintf(out->conclusion_text, sizeof(out->conclusion_text),
                       "启动超时:%d 秒内进程没有退出,已被启动器终止(可能卡在资源下载或驱动初始化)。",
                       req->timeout_ms / 1000 > 0 ? req->timeout_ms / 1000 : 1);
    } else if (out->exit_code != 0 &&
               (out->conclusion == SXCL_LOG_CONCLUSION_OK ||
                out->conclusion == SXCL_LOG_CONCLUSION_UNKNOWN)) {
        /* 崩溃了但日志没给原因:不能因为"日志里没写"就说一切正常 */
        out->conclusion = SXCL_LOG_CONCLUSION_CRASH;
        (void)snprintf(out->conclusion_text, sizeof(out->conclusion_text),
                       "游戏以退出码 %d 结束,但日志里没有写出明确原因;可以看 crash-reports/ 与 "
                       "hs_err_pid*.log 里的细节。",
                       out->exit_code);
    }

    /* 把"实际生效的后端"记回实例设置:下次进设置页能直接告诉用户上次跑的是什么 */
    if (settings && out->started) {
        if (sxcl_settings_instance_set(settings, instance, SXCL_LAUNCH_LAST_BACKEND_KEY,
                                       out->actual_backend) == 0) {
            (void)sxcl_settings_save(settings, req->settings_path);
        }
    }
    rc = 0;

done:
    sxcl_options_free(options);
    sxcl_settings_free(settings);
    sxcl_launch_args_free(args);
    sxcl_json_free(doc);
    if (rc != 0 && !(err != NULL && err_len > 0 && err[0] != '\0')) {
        set_error(out, err, err_len, out->error[0] ? out->error : "启动失败");
    }
    return rc;
}
