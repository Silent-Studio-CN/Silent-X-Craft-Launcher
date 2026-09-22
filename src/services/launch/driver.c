/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/launch.h"

#include "sxcl/crash.h"   /* 崩溃取证:退出后读 crash-reports 与 logs/latest.log */
#include "sxcl/fs.h"
#include "sxcl/instance.h" /* sxcl_instance_read_json:PCL 式目录名与 id 不一致的版本也能启动 */
#include "sxcl/loader.h"   /* sxcl_loader_flatten_json:继承式版本 JSON 在启动前合并一次(见 docs/23) */
#include "sxcl/log.h"      /* 游戏输出落盘(模块 game;攒批写,见 log.h 第 5 节) */
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

/** 把原因键的文案填进结果(所有出口都走它,免得某条路径忘了填)。 */
static void fill_reason(sxcl_launch_result *out)
{
    sxcl_log_reason reason = SXCL_REASON_UNKNOWN;
    if (out == NULL) {
        return;
    }
    reason = (out->log.reason != SXCL_REASON_UNKNOWN) ? out->log.reason : out->reason;
    out->reason = reason;
    copy_str(out->reason_key, sizeof(out->reason_key), sxcl_log_reason_key(reason));
    copy_str(out->reason_name, sizeof(out->reason_name), sxcl_log_reason_name(reason, NULL));
    copy_str(out->reason_advice, sizeof(out->reason_advice),
             sxcl_log_reason_advice(reason, NULL));
}

/** 记一个失败原因,并**点名原因键**(界面据此显示可执行建议,而不是只有一句错误)。 */
static void set_reason(sxcl_launch_result *out, sxcl_log_reason reason)
{
    if (out == NULL) {
        return;
    }
    out->reason = reason;
    fill_reason(out);
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

/** 带原因键的失败出口(set_error + set_reason)。 */
static void set_error_reason(sxcl_launch_result *out, char *err, size_t err_len, const char *text,
                             sxcl_log_reason reason)
{
    set_error(out, err, err_len, text);
    set_reason(out, reason);
}

/* ────────────────────────── 逐行回调 ────────────────────────── */

typedef struct driver_line_ctx {
    sxcl_launch_result *res;
    const sxcl_launch_request *req;
} driver_line_ctx;

static int driver_on_line(void *userdata, int is_stderr, const char *line)
{
    driver_line_ctx *lc = (driver_line_ctx *)userdata;
    /* ① 落盘:每一行游戏输出都进我们自己的日志(模块 game;攒批写,不逐行 flush)。
     *    这是"用户报障时我们手上得有一份"的唯一保证 —— stdout 在崩溃那一刻就断了。 */
    sxcl_log_game_line(line);
    /* ② 归类:类别与原因键都从这一路来(与从前完全一致) */
    (void)sxcl_log_summary_add(&lc->res->log, line);
    /* ③ 原始行交给调用方(界面/CLI),既有的行为一个字都不改 */
    if (lc->req->on_line) {
        return lc->req->on_line(lc->req->userdata, is_stderr, line);
    }
    return 0;
}

/* 进程起来 -> 记下 PID 并转发给调用方(界面要用它显示 / 单独结束游戏进程) */
static int driver_on_started(void *userdata, int64_t pid)
{
    driver_line_ctx *lc = (driver_line_ctx *)userdata;
    lc->res->pid = pid;
    if (lc->req->on_started) {
        return lc->req->on_started(lc->req->userdata, pid);
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


/** 第 i 个参数**打码后**的文本(与 log_argv 同一份规则:
 *  敏感开关的下一个参数一律替换成说明文字,不泄漏凭据/身份)。 */
static const char *argv_masked_text(const char *const *av, size_t index)
{
    static const char kMasked[] = "***（已打码：凭据/身份不进日志）";
    static const char *const kFlags[] = {"--accessToken", "--session", "--uuid", "--username",
                                         "--xuid",        "--clientId", NULL};
    size_t k = 0;
    if (av == NULL || index == 0u) {
        return (av != NULL && av[index] != NULL) ? av[index] : "";
    }
    for (k = 0; kFlags[k] != NULL; ++k) {
        if (strcmp(av[index - 1u], kFlags[k]) == 0) {
            return kMasked;
        }
    }
    return av[index];
}

/** 把将要执行的命令行写进运行日志(**已打码**:凭据与身份都不进日志)。
 *  为什么两个地方都要:dry-run 是"将要跑什么",真起进程是"实际跑了什么" ——
 *  用户报障时我们手上必须有后者(界面/CLI 都不需要自己再拼一遍)。 */
static void log_argv(const sxcl_launch_result *out, const sxcl_launch_args *args)
{
    static const char *const kMasked[] = {"--accessToken", "--session", "--uuid", "--username",
                                          "--xuid",        "--clientId", NULL};
    const char *const *av = sxcl_launch_argv(args);
    int mask_next = 0;
    size_t i = 0;
    SXCL_LOG_I("launch", "argv: %s", (out != NULL && out->java_path[0] != '\0') ? out->java_path
                                                                                : "(java 路径未知)");
    for (i = 0; av != NULL && av[i] != NULL; ++i) {
        const char *text = av[i];
        if (mask_next) {
            text = "***（已打码：凭据/身份不进日志）";
            mask_next = 0;
        } else {
            size_t k = 0;
            for (k = 0; kMasked[k] != NULL; ++k) {
                if (strcmp(av[i], kMasked[k]) == 0) {
                    mask_next = 1;
                    break;
                }
            }
        }
        SXCL_LOG_I("launch", "  arg[%llu] %s", (unsigned long long)i, text);
    }
}

/* ────────────────────────── 主流程 ────────────────────────── */

/* ── 资源对象那一遍(P0b):索引 -> 展开 objects -> 再跑一次引擎 ──
 *
 * 为什么要两遍:清单里只有"资源索引"这一件(sxcl_version_plan_build),而 5000+ 个对象
 * 得读**索引内容**才知道有哪些 —— 索引刚被第一遍下下来(或者本来就在),所以第二遍才有得展开。
 * 第二遍跑的是**整张计划**,命中的那些会走引擎的"已存在"快路径,只下缺的。
 *
 * level 见 launch.h 的 complete_assets(0 不补 / 1 只比大小 / 2 强校验)。
 * 镜像顺序**必须在这里自己排**:计划上那批已经 prefer_mirror 过一次了,再调一次会把它们换回来
 * (manifest.h 明写"只应调用一次")—— 所以官方/镜像谁在前由这里的两个 base 决定。
 *
 * 返回:>0 = 第二遍真的跑了(值 = 新展开的对象数,统计写在 out);
 *       0 = 不用补 / 没得补(没开档位、没有 assetIndex、索引还没下下来)—— 都不是错误;
 *      <0 = 第二遍失败(err 有人话;调用方按"补不齐不拦启动"处理)。 */
static int complete_assets_pass(const sxcl_json_value *root, const sxcl_launch_request *req,
                                sxcl_version_plan *plan, sxcl_fetch_stats *out, char *err,
                                size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (req->complete_assets <= 0) {
        return 0;
    }
    const char *index_id = sxcl_json_get_string(sxcl_json_get(root, "assetIndex"), "id", NULL);
    if (!index_id || !index_id[0]) {
        return 0;   /* 这个版本没有资源索引(很老的版本就是没有):没得展开 */
    }

    char rel[160];
    char path[SXCL_JAVA_PATH_MAX * 2];
    (void)snprintf(rel, sizeof(rel), "assets/indexes/%s.json", index_id);
    join_path(path, sizeof(path), req->game_dir, rel); /* 本文件是 void 版:拼不出来是空串,parse 会失败 */
    char perr[192];
    perr[0] = '\0';
    sxcl_json *index_doc = sxcl_json_parse_file(path, perr, sizeof(perr));
    if (!index_doc) {
        /* 索引还没下下来(第一遍可能失败了):如实说一句,但不当错误 —— 启动照走 */
        if (err && err_len) {
            (void)snprintf(err, err_len, "资源索引还没下下来（%s），这次先不补资源文件",
                           perr[0] ? perr : "读不出来");
        }
        return 0;
    }

    const char *first = SXCL_ASSET_OBJECTS_BASE;
    const char *second = NULL;
    if (req->prefer_mirror) {
        first = (req->mirror_base && req->mirror_base[0]) ? req->mirror_base
                                                          : SXCL_MIRROR_BMCLAPI_BASE;
        second = SXCL_ASSET_OBJECTS_BASE;
    }
    const int added = sxcl_version_plan_add_asset_objects_ex(
        plan, index_doc, req->game_dir, first, second, req->complete_assets >= 2 ? 1 : 0, err,
        err_len);
    sxcl_json_free(index_doc);
    if (added <= 0) {
        return 0;
    }
    SXCL_LOG_I("launch", "启动前补全:资源索引 %s 展开出 %d 个资源对象(%s)",
               index_id, added, req->complete_assets >= 2 ? "强校验" : "只比大小");
    const int rc = sxcl_version_plan_fetch(plan, req->engine_opts, req->complete_is_cancelled,
                                           req->complete_cancel_ud, out, err, err_len);
    return rc == SXCL_FETCH_OK ? added : (rc == SXCL_FETCH_PARTIAL ? added : -1);
}

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
    out->pid = -1; /* 没起来就是 -1(与 sxcl_process_result.pid 同口径) */
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
        set_error_reason(out, err, err_len, msg, SXCL_REASON_PATH_NOT_FOUND);
        goto done;
    }
    /* ── 1b. 继承式版本 JSON:启动层自己合并一次(拍平的兜底) ──
     * 我们自己的安装器现在会把版本 JSON **拍平**(PCL/HMCL 装的都是这个形态,见 docs/23),
     * 但**存量实例**、别家启动器(HMCL/FCL)装的版本、手改过的版本都还是"inheritsFrom 指向原版"。
     * 启动层以前完全不解析继承:这种版本会缺原版的库、assetIndex 还会退化成 legacy
     * (实测一个 fabric-loader-* 实例只带 7 个库、一个 jar 都没有)。这里用**安装时同一条合并规则**
     * 兜底,并在实例自己没有 jar 时改用原版的 client.jar —— 继承语义里"没有自己的 jar"就是这个意思。 */
    char fallback_jar[SXCL_JAVA_PATH_MAX * 2];
    fallback_jar[0] = '\0';
    {
        /* **先把 inheritsFrom 拷进本地缓冲**:合并成功时下面会把 doc 换成合并后的新文档,
         * 原文档一释放,指向它内部字符串的指针就成了野指针(第一版就是这么崩的:
         * ucrtbase strnlen 里 0xC0000005 —— 用-after-free)。 */
        const char *hit = sxcl_json_get_string(sxcl_json_root(doc), "inheritsFrom", NULL);
        char parent_id[SXCL_LOADER_CMD_ARG_MAX];
        parent_id[0] = '\0';
        if (hit && hit[0]) {
            (void)snprintf(parent_id, sizeof(parent_id), "%s", hit);
        }
        if (parent_id[0]) {
            char parent_json[SXCL_JAVA_PATH_MAX * 2];
            sxcl_json *parent_doc = sxcl_instance_read_json(req->game_dir, parent_id, parent_json,
                                                            sizeof(parent_json), NULL, 0);
            char *merged_text = NULL;
            char ferr[192];
            ferr[0] = '\0';
            if (parent_doc &&
                sxcl_loader_flatten_json(sxcl_json_root(doc), sxcl_json_root(parent_doc),
                                         req->version_name, parent_id, &merged_text, ferr,
                                         sizeof(ferr)) == SXCL_LOADER_OK &&
                merged_text) {
                char merr[192];
                merr[0] = '\0';
                sxcl_json *merged = sxcl_json_parse(merged_text, strlen(merged_text), merr,
                                                    sizeof(merr));
                if (merged) {
                    sxcl_json_free(doc);
                    doc = merged;
                }
            }
            free(merged_text);
            sxcl_json_free(parent_doc);

            /* 实例自己没有 jar 时用原版的(继承式版本常常就是这么摆的)。 */
            char own_jar[SXCL_JAVA_PATH_MAX * 2];
            char pleaf[SXCL_JAVA_PATH_MAX];
            char pdir[SXCL_JAVA_PATH_MAX * 2];
            char pversions[SXCL_JAVA_PATH_MAX * 2];
            /* 本文件的 join_path 是 void 版(拼不出来就留空串),所以这里先拼再看文件在不在。 */
            (void)snprintf(leaf, sizeof(leaf), "%s.jar", req->version_name);
            (void)snprintf(pleaf, sizeof(pleaf), "%s.jar", parent_id);
            join_path(own_jar, sizeof(own_jar), versions_dir, leaf);
            join_path(pversions, sizeof(pversions), req->game_dir, "versions");
            join_path(pdir, sizeof(pdir), pversions, parent_id);
            join_path(fallback_jar, sizeof(fallback_jar), pdir, pleaf);
            if (!sxcl_fs_exists(own_jar) && !sxcl_fs_exists(fallback_jar)) {
                fallback_jar[0] = '\0';   /* 原版的 jar 也不在:保持默认(如实走不通) */
            }
        }
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
            set_reason(out, SXCL_REASON_JAVA_NOT_FOUND);
            goto done;
        }
        java = found[order[0]];
    }
    out->java_major = java.major;
    out->java_is_64bit = java.is_64bit;
    copy_str(out->java_path, sizeof(out->java_path), java.path);
    copy_str(out->java_version, sizeof(out->java_version), java.version);

    /* ── 2b. 启动前补全文件(PCL 启动链的第 3 步"补全文件";用户点名要学) ──
     * 必须在**解压 natives 之前**:natives 就是从库列表里的 jar 解出来的,缺库时先解压
     * 只会解出半个目录,拼出来的命令行也会指着一堆不存在的路径(旧行为:只报告、不补,
     * 而且那份"缺什么"是从**游戏自己的日志**里读出来的 —— 也就是游戏已经崩了才知道)。
     *
     * 分工:这里只算"这个版本要哪些文件"(纯函数 sxcl_version_plan_build),真正的下载
     * 交给注入进来的执行器 —— 它手上有下载引擎(多候选路重试/分片/限速/断点续传/哈希缓存),
     * 启动层不该自己再写一套网络代码。
     * **补不齐不拦启动**(与 PCL 一致):如实记进结果,用户看到的是"缺 N 个文件",
     * 而不是"启动器崩了"。 */
    if (req->complete_files != 0 && req->engine_opts != NULL &&
        req->engine_opts->transport_factory != NULL) {
        char perr[192];
        perr[0] = '\0';
        sxcl_version_plan *plan =
            sxcl_version_plan_build(doc, req->game_dir, req->version_name, perr, sizeof(perr));
        if (!plan) {
            (void)snprintf(out->complete.error, sizeof(out->complete.error),
                           "列不出这个版本要哪些文件（%s）", perr[0] ? perr : "原因不明");
            SXCL_LOG_W("launch", "启动前补全:列不出文件清单(%s)", perr[0] ? perr : "原因不明");
        } else {
            /* 镜像第二路:认得出官方域名的补一条镜像候选,再按设置决定谁排第一
             * (与安装同一个口径;只调一次 prefer_mirror,见 manifest.h)。 */
            if (req->prefer_mirror) {
                char merr[192];
                merr[0] = '\0';
                if (sxcl_version_plan_add_mirror(plan, req->mirror_base, merr, sizeof(merr)) < 0) {
                    SXCL_LOG_W("launch", "启动前补全:补镜像候选失败(%s)",
                               merr[0] ? merr : "认不出这些 URL");
                }
                (void)sxcl_version_plan_prefer_mirror(plan);
            }
            sxcl_fetch_stats fstats;
            const int frc = sxcl_version_plan_fetch(plan, req->engine_opts,
                                                    req->complete_is_cancelled,
                                                    req->complete_cancel_ud, &fstats, perr,
                                                    sizeof(perr));
            /* ── 第二遍:资源对象(P0b)──
             * 第一遍把资源**索引**下下来了,这一遍才有得展开 5000+ 个对象。
             * 第二遍跑的是整张计划,所以"总数/命中"取第二遍的,"下载/失败/字节"两遍相加
             * (第一遍下过的在第二遍一定命中,不会重复计)。 */
            sxcl_fetch_stats astats;
            memset(&astats, 0, sizeof(astats));
            char aerr[192];
            aerr[0] = '\0';
            int arc = 0;
            if (frc == SXCL_FETCH_OK || frc == SXCL_FETCH_PARTIAL) {
                arc = complete_assets_pass(sxcl_json_root(doc), req, plan, &astats, aerr,
                                           sizeof(aerr));
            }
            if (arc > 0 && astats.total > 0) {
                /* 第二遍跑的是**整张计划**,所以"总数/命中/失败"都取第二遍的:
                 * 失败**不能相加** —— 第一遍没下成的那件在第二遍里还会失败,相加就重复计了
                 * (实测:一件库没下成,相加后报"失败 2 件")。
                 * "下载/字节"可以相加:第一遍下过的在第二遍一定命中,不会重复。 */
                out->complete.files_total = astats.total;
                out->complete.files_skipped = astats.skipped;
                out->complete.files_failed = astats.failed;
                out->complete.files_downloaded = fstats.downloaded + astats.downloaded;
                out->complete.bytes_done = fstats.bytes_done + astats.bytes_done;
            } else {
                out->complete.files_total = fstats.total;
                out->complete.files_downloaded = fstats.downloaded;
                out->complete.files_failed = fstats.failed;
                out->complete.files_skipped = fstats.skipped;
                out->complete.bytes_done = fstats.bytes_done;
            }
            if (arc < 0 && aerr[0]) {
                (void)snprintf(perr, sizeof(perr), "资源对象没能补完：%s", aerr);
            } else if (perr[0] == '\0' && aerr[0]) {
                (void)snprintf(perr, sizeof(perr), "%s", aerr);
            }
            if (perr[0]) {
                (void)snprintf(out->complete.error, sizeof(out->complete.error), "%s", perr);
            } else if (frc != SXCL_FETCH_OK) {
                (void)snprintf(out->complete.error, sizeof(out->complete.error),
                               "补全没跑成(返回码 %d)", frc);
            }
            out->complete_ran = 1;
            SXCL_LOG_I("launch",
                       "启动前补全:共 %d 个文件,命中已有 %d 个,下载 %d 个(失败 %d 个),写了 %lld 字节%s%s",
                       out->complete.files_total, out->complete.files_skipped,
                       out->complete.files_downloaded, out->complete.files_failed,
                       (long long)out->complete.bytes_done,
                       out->complete.error[0] ? " · " : "",
                       out->complete.error[0] ? out->complete.error : "");
            sxcl_version_plan_free(plan);
        }
    }

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
        set_error_reason(out, err, err_len, msg, SXCL_REASON_FILE_PERMISSION);
        goto done;
    }
    if (sxcl_options_set(options, SXCL_LAUNCH_GRAPHICS_KEY, requested) != 0 ||
        sxcl_options_save(options, options_path) != 0) {
        char msg[256];
        (void)snprintf(msg, sizeof(msg), "写不了 options.txt:%s(权限?)。", options_path);
        set_error_reason(out, err, err_len, msg, SXCL_REASON_FILE_PERMISSION);
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
        set_error_reason(out, err, err_len, msg, SXCL_REASON_NATIVES_EXTRACT_FAILED);
        goto done;
    }
    copy_str(out->natives_dir, sizeof(out->natives_dir), natives);
    if (sxcl_natives_prepare_json(doc, req->game_dir, natives, errbuf, sizeof(errbuf)) != 0) {
        char msg[256];
        (void)snprintf(msg, sizeof(msg), "准备原生库失败:%s", errbuf);
        set_error_reason(out, err, err_len, msg, SXCL_REASON_NATIVES_EXTRACT_FAILED);
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
    if (fallback_jar[0]) {
        ctx.client_jar = fallback_jar;   /* 实例自己没有 jar:用原版那份(见上面 1b) */
    }
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
        set_error_reason(out, err, err_len, msg, SXCL_REASON_CLASSPATH_BROKEN);
        goto done;
    }

    if (req->dry_run) {
        (void)snprintf(out->conclusion_text, sizeof(out->conclusion_text),
                       "只准备不启动:Java 与参数都已就绪,options.txt 已写入 %s。", requested);
        /* dry-run + 有 on_line 回调时,把"将要执行的命令行"逐行报出去。
         * **必须打码**:accessToken 是真凭据,用户名/UUID 是身份信息 —— 这两样都不进日志
         * (下一个参数是它们时替换成 ***)。为什么把 --username/--uuid 也算进来:
         * 这份命令行会写进我们的日志,而日志是会被"一键导出"发给别人的。 */
        log_argv(out, args); /* 先落盘(打码),再交给调用方显示 */
        if (req->on_line != NULL) {
            char head[512];
            (void)snprintf(head, sizeof(head), "argv: %s", out->java_path);
            (void)req->on_line(req->userdata, 0, head);
            const char *const *av = sxcl_launch_argv(args);
            for (size_t i = 0; av != NULL && av[i] != NULL; ++i) {
                /* 回调拿到的是**同一份已打码**的文本(见 log_argv),
                 * 所以命令行前端/界面显示出来的也不含明文凭据。 */
                char line[1024];
                (void)snprintf(line, sizeof(line), "  arg[%llu] %s", (unsigned long long)i,
                               argv_masked_text(av, i));
                (void)req->on_line(req->userdata, 0, line);
            }
        }
        set_reason(out, SXCL_REASON_EXIT_OK); /* 只准备:没崩,给"正常"这条,而不是"未知" */
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
        popts.on_started = driver_on_started;
        popts.userdata = &lc;
        log_argv(out, args); /* 实际跑的那一次也要留档(与 dry-run 那份内容一致,都是打码后的) */
        if (sxcl_process_run(&popts, &pr) != 0) {
            char msg[256];
            (void)snprintf(msg, sizeof(msg), "启动进程失败:%s", pr.error);
            set_error_reason(out, err, err_len, msg, SXCL_REASON_JAVA_BROKEN);
            goto done;
        }
        out->started = 1;
        if (out->pid == 0) {
            out->pid = pr.pid; /* on_started 没被调用时的兜底 */
        }
        out->exit_code = pr.exit_code;
        out->timed_out = pr.timed_out;
        out->killed_by_client = pr.killed_by_client;
        out->elapsed_ms = pr.elapsed_ms;
    }

    /* ── 7. 结论:先把游戏输出的缓冲冲刷干净(stdout 断了之后那几行只能靠它)── */
    (void)sxcl_log_game_flush();
    sxcl_log_summary_finish(&out->log);
    out->conclusion = out->log.conclusion;
    copy_str(out->conclusion_text, sizeof(out->conclusion_text), out->log.advice);
    /* 缺什么原样带出去(不做补全,只报告) */
    copy_str(out->missing, sizeof(out->missing), out->log.missing);

    /* ── 7b. 崩溃取证:崩了才去读游戏**自己写的**那两份文件 ──
     * 为什么必须有这一步:进程一崩 stdout 就断了,死因写在
     *   <game>/crash-reports/*.txt(最近的一份非空报告)与 <game>/logs/latest.log 里。
     * 读到的每一行都喂进**同一个** summary(与 stdout 同一套规则),读完再出一次结论。
     * 超时/被用户结束不算崩溃:那种情况读出来的多半是无关的历史报告,反而误导。 */
    if (out->started && !out->killed_by_client &&
        (out->exit_code != 0 || out->conclusion == SXCL_LOG_CONCLUSION_CRASH ||
         out->log.voted_crash)) {
        char scan_err[SXCL_CRASH_ERROR_MAX];
        const long long fed = sxcl_launch_scan_artifacts(req->game_dir, &out->log, &out->artifacts,
                                                         SXCL_CRASH_SCAN_ALL, 0, scan_err,
                                                         sizeof(scan_err));
        out->artifacts_scanned = 1;
        if (fed > 0) {
            sxcl_log_summary_finish(&out->log); /* 重出结论:证据变多了 */
            out->conclusion = out->log.conclusion;
            copy_str(out->conclusion_text, sizeof(out->conclusion_text), out->log.advice);
            copy_str(out->missing, sizeof(out->missing), out->log.missing);
        }
        copy_str(out->crash_report_path, sizeof(out->crash_report_path), out->artifacts.report_path);
        out->crash_report_lines = out->artifacts.report_lines;
        out->latest_log_lines = out->artifacts.latest_log_lines;
        SXCL_LOG_I("crash",
                   "崩溃取证:报告=%s(%s,%lld 行/共 %d 份) latest.log=%s(%s,%lld 行%s) 喂入 %lld 行",
                   out->artifacts.report_path[0] ? out->artifacts.report_path : "(没有)",
                   out->artifacts.report_path[0] ? out->artifacts.report_encoding : "-",
                   out->artifacts.report_lines, out->artifacts.reports_total,
                   out->artifacts.latest_log_path[0] ? out->artifacts.latest_log_path : "(没有)",
                   out->artifacts.latest_log_path[0] ? out->artifacts.latest_log_encoding : "-",
                   out->artifacts.latest_log_lines,
                   out->artifacts.latest_log_truncated ? ",只读了尾部" : "", fed);
    }

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

    /* 原因键的文案(短名/建议)最后统一填一次:上面所有分支都可能改过结论 */
    fill_reason(out);

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
    fill_reason(out);
    return rc;
}

/* ────────────────────────── 崩溃取证入口(第 3c 节)────────────────────────── */

typedef struct artifact_feed {
    sxcl_log_summary *summary;
} artifact_feed;

/** 取证行 -> 日志汇总的适配器(回调签名不同:取证回调还带一个"从哪读来的"来源)。
 *  永远返回 0:不因为某一行而中止取证(规则自己会挑最具体的那条)。 */
static int artifact_line(void *userdata, int source, const char *line)
{
    artifact_feed *feed = (artifact_feed *)userdata;
    (void)source;
    if (feed != NULL && feed->summary != NULL) {
        (void)sxcl_log_summary_add(feed->summary, line);
    }
    return 0;
}

long long sxcl_launch_scan_artifacts(const char *game_dir, sxcl_log_summary *summary,
                                     sxcl_crash_evidence *facts, unsigned flags, size_t max_bytes,
                                     char *err, size_t err_len)
{
    artifact_feed feed;
    long long fed = 0;
    if (err != NULL && err_len > 0u) {
        err[0] = '\0';
    }
    if (game_dir == NULL || *game_dir == '\0') {
        if (err != NULL && err_len > 0u) {
            (void)snprintf(err, err_len, "崩溃取证:缺少游戏目录");
        }
        return -1;
    }
    /* 把每一行喂进**同一个** summary —— 与 stdout 走同一套规则、同一张原因表,
     * 这样"从哪读到的"不会让结论分叉。 */
    feed.summary = summary;
    fed = sxcl_crash_scan(game_dir, flags, max_bytes, artifact_line, &feed, facts, err, err_len);
    if (summary != NULL && fed > 0) {
        sxcl_log_summary_finish(summary);
    }
    return fed;
}
