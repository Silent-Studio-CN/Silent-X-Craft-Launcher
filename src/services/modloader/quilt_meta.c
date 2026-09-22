/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// Quilt 的「从 meta 直装」服务层(docs/22 §16 的正解;声明与理由见 loader.h)。
//
// 为什么不是"下载安装器 jar 再跑它":
//   * 那个 jar(org.quiltmc:quilt-installer,8.7MB)**只有** maven.quiltmc.org 一家托管;
//     BMCLAPI 与 Maven Central 都 404(实测),官方站实测 32KB/s 且会停摆 ——
//     8.7MB 在很多网络里根本下不完(真机:分片拉了一阵被整条 IP 掐,停在 40%);
//   * 而 meta.quiltmc.org 的返回体里**本来就有** launcherMeta.libraries 与 mainClass.client。
// 于是这条路的形状是:取 meta(几百 KB) -> 拼加载器层 -> 与原版拍平 -> 下那几个库
// (quilt-loader 1.5MB + hashed 0.8MB + 若干小件)。
//
// 纪律:
//   * **版本 JSON 最后写**(与 docs/24 的 P1 同一条):先下库,库齐了才落 JSON ——
//     磁盘上不会留下"看着装好了、其实缺库"的实例;
//   * 哈希不一致**必须说出来**:meta 的 sha1 与 maven 自己发的 .sha1 不一致时按侧车纠正 + 计数,
//     不静默放过(实测 quilt-loader-0.20.0-beta.9 与 hashed-1.20.1 都撞上了);
//     纠正要**落进写出去的那份版本 JSON**(sxcl_loader_patch_library_sha1):只纠下载任务的话,
//     启动器自己的"启动前补全"会把刚装好的文件判成坏件,每回启动都报同样几件失败;
//   * 进度/取消共用调用方给的钩子(界面与命令行同一套)。

#include "sxcl/loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/engine.h"
#include "sxcl/fs.h"
#include "sxcl/install.h"        /* sxcl_install_http_get_text */
#include "sxcl/log.h"            /* 侧车纠偏落不回 JSON 时要说一声(别静默) */
#include "sxcl/json.h"
#include "sxcl/loader_catalog.h" /* sxcl_loader_quilt_loader_json、sxcl_loader_patch_library_sha1 */
#include "sxcl/verify.h"         /* sxcl_hash_file:事后自己算实际哈希(侧车拿不到时的兜底) */

#define QUILT_META_DEFAULT "https://meta.quiltmc.org/v3/versions/loader"
#define QUILT_MAX_LIBS 256

/** 取侧车最多试几次(它只有 41 字节;那家源会 30 秒超时,重试比"直接认输"便宜得多)。 */
#define QUILT_SIDECAR_TRIES 3

static void quilt_err(char *err, size_t len, const char *text)
{
    if (err != NULL && len > 0) {
        (void)snprintf(err, len, "%s", text != NULL ? text : "");
    }
}

static void quilt_progress(const sxcl_quilt_install_request *req, int percent, const char *text)
{
    if (req != NULL && req->on_progress != NULL) {
        req->on_progress(req->progress_ud, percent, text != NULL ? text : "");
    }
}

static int quilt_cancelled(const sxcl_quilt_install_request *req)
{
    return (req != NULL && req->is_cancelled != NULL) ? req->is_cancelled(req->cancel_ud) : 0;
}

/** 原子地写一段文本(先写 .tmp 再 rename)。失败不留半个文件。 */
static int quilt_write_file(const char *path, const char *text)
{
    char tmp[1200];
    (void)snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fh = sxcl_fs_fopen(tmp, "wb");
    if (fh == NULL) {
        return -1;
    }
    const size_t len = strlen(text);
    const size_t wrote = fwrite(text, 1, len, fh);
    fclose(fh);
    if (wrote != len || sxcl_fs_rename_replace(tmp, path) != 0) {
        sxcl_fs_remove(tmp);
        return -1;
    }
    return 0;
}

/** 把 "<40 位十六进制>[ 文件名]" 里的哈希抠出来(小写)。返回 1 = 抠到了。 */
static int quilt_parse_sidecar(const char *text, char *out, size_t cap)
{
    if (text == NULL || cap < 41) {
        return 0;
    }
    size_t k = 0;
    for (const char *p = text; *p != '\0' && k < 40; ++p) {
        const char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            out[k++] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
        } else {
            break;
        }
    }
    out[k] = '\0';
    return k == 40 ? 1 : 0;
}

/** 十六进制小写化(比较用;in 可空)。 */
static void quilt_lower_hex(const char *in, char *out, size_t cap)
{
    size_t k = 0;
    if (out == NULL || cap == 0) {
        return;
    }
    for (const char *p = in; p != NULL && *p != '\0' && k + 1 < cap; ++p) {
        const char c = *p;
        out[k++] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
    }
    out[k] = '\0';
}

/** 强制走"侧车拿不到"的兜底(诊断/验收用,不必真等 30 秒超时)。 */
static int quilt_skip_sidecar(void)
{
    const char *v = getenv("SXCL_QUILT_NO_SIDECAR");
    return v != NULL && v[0] != '\0' && v[0] != '0';
}

/* ── 下库这一段的进度:直接**从任务状态里数**(不额外维护计数器) ──
 * 为什么这样数:引擎的 on_progress 是从**多个下载线程**回调的,自己维护计数器就得加锁;
 * 而"已落定几件"本来就写在任务结构体里 —— 每次回调扫一遍(最多 256 项)又准又不用锁。
 * last_reported 只是"别重复报同一个数",多线程下偶尔重复报一次无害(数字本身是对的)。 */
typedef struct quilt_bridge {
    const sxcl_quilt_install_request *req;
    const sxcl_task *tasks;
    int total;
    int base_percent;
    int span_percent;
    int last_reported;
    /* 调用方原来给引擎的 on_progress(命令行逐文件打印、以后界面想用):原样转发一份,
     * 我们只是**占**了引擎那个位置来数进度,不能把调用方那条路吞掉。 */
    void (*forward)(void *ud, const sxcl_task *task);
    void *forward_ud;
} quilt_bridge;

static void quilt_bridge_on_progress(void *ud, const sxcl_task *task)
{
    quilt_bridge *b = (quilt_bridge *)ud;
    if (b == NULL) {
        return;
    }
    if (b->forward != NULL) {
        b->forward(b->forward_ud, task);
    }
    if (b->tasks == NULL || b->total <= 0) {
        return;
    }
    int done = 0;
    for (int i = 0; i < b->total; ++i) {
        const sxcl_task_state st = b->tasks[i].state;
        if (st == SXCL_TASK_DONE || st == SXCL_TASK_FAILED || st == SXCL_TASK_CANCELLED) {
            ++done;
        }
    }
    if (done == b->last_reported) {
        return;
    }
    b->last_reported = done;
    char text[96];
    (void)snprintf(text, sizeof(text), "下载依赖库 %d/%d", done, b->total);
    quilt_progress(b->req, b->base_percent + (b->span_percent * done) / b->total, text);
}

int sxcl_loader_quilt_install(const sxcl_quilt_install_request *req, sxcl_quilt_install_result *out,
                              char *err, size_t err_len)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    if (req == NULL || req->game_dir == NULL || req->mc_version == NULL ||
        req->loader_version == NULL || req->engine_opts == NULL ||
        req->engine_opts->transport_factory == NULL) {
        quilt_err(err, err_len, "参数不全(需要游戏目录 / 原版版本 / 加载器版本 / 带传输后端的引擎配置)");
        return SXCL_LOADER_ERR_ARG;
    }

    char instance[160];
    if (req->instance_name != NULL && req->instance_name[0] != '\0') {
        (void)snprintf(instance, sizeof(instance), "%s", req->instance_name);
    } else {
        (void)snprintf(instance, sizeof(instance), "%s-quilt-%s", req->mc_version,
                       req->loader_version);
    }
    if (out != NULL) {
        (void)snprintf(out->instance, sizeof(out->instance), "%s", instance);
    }

    char meta_url[640];
    if (req->meta_url != NULL && req->meta_url[0] != '\0') {
        (void)snprintf(meta_url, sizeof(meta_url), "%s", req->meta_url);
    } else {
        (void)snprintf(meta_url, sizeof(meta_url), "%s/%s", QUILT_META_DEFAULT, req->mc_version);
    }

    /* ── 1. 取 meta ── */
    /* meta **必须重试**:那家源实测会"连上以后一个字都不来"(传输停滞看门狗 60 秒就掐掉),
     * 而一次掐掉就整个加载器层失败 —— 用户看到的是"Quilt 没装上"(真机 2026-09-22 21:41 就是这样)。
     * 每次掐掉/失败都立刻重试,次数用调用方给的 retries(界面给 6),至少 3 次;
     * 进度里如实写出"第 n 次",别让界面停在"取 meta"上一动不动。 */
    int meta_attempts = req->retries > 0 ? req->retries : 3;
    if (meta_attempts < 3) {
        meta_attempts = 3;
    }
    char *meta = NULL;
    char herr[256];
    herr[0] = '\0';
    char last_herr[256];
    last_herr[0] = '\0';
    for (int attempt = 1; attempt <= meta_attempts && meta == NULL; ++attempt) {
        char label[96];
        (void)snprintf(label, sizeof(label), "取 Quilt meta(第 %d/%d 次)", attempt, meta_attempts);
        quilt_progress(req, 2, label);
        if (quilt_cancelled(req)) {
            quilt_err(err, err_len, "已取消(取 meta 之前)");
            return SXCL_LOADER_ERR_IO;
        }
        herr[0] = '\0';
        if (sxcl_install_http_get_text((void *)req->engine_opts, meta_url, &meta, herr,
                                       sizeof(herr)) == 0 &&
            meta != NULL) {
            break;
        }
        (void)snprintf(last_herr, sizeof(last_herr), "%s", herr[0] ? herr : "原因不明");
        free(meta);
        meta = NULL;
    }
    if (meta == NULL) {
        char text[420];
        (void)snprintf(text, sizeof(text), "取 meta 失败(%d 次都没成): %s(%s)", meta_attempts,
                       last_herr, meta_url);
        quilt_err(err, err_len, text);
        return SXCL_LOADER_ERR_IO;
    }
    if (quilt_cancelled(req)) {
        free(meta);
        quilt_err(err, err_len, "已取消(取 meta 之后)");
        return SXCL_LOADER_ERR_IO;
    }

    /* ── 2. 拼「加载器层」 ── */
    quilt_progress(req, 12, "拼加载器层");
    char *loader_json = NULL;
    char cerr[192];
    cerr[0] = '\0';
    if (sxcl_loader_quilt_loader_json(meta, strlen(meta), req->loader_version, req->mc_version,
                                      &loader_json, cerr, sizeof(cerr)) != SXCL_CATALOG_OK) {
        char text[360];
        (void)snprintf(text, sizeof(text), "拼加载器 JSON 失败: %s", cerr[0] ? cerr : "原因不明");
        quilt_err(err, err_len, text);
        free(meta);
        return SXCL_LOADER_ERR_FORMAT;
    }
    free(meta);

    /* ── 3. 与原版拍平 ── */
    quilt_progress(req, 20, "与原版合并(拍平)");
    char base_path[1200];
    (void)snprintf(base_path, sizeof(base_path), "%s/versions/%s/%s.json", req->game_dir,
                   req->mc_version, req->mc_version);
    char perr[192];
    perr[0] = '\0';
    sxcl_json *base_doc = sxcl_json_parse_file(base_path, perr, sizeof(perr));
    sxcl_json *loader_doc = sxcl_json_parse(loader_json, strlen(loader_json), perr, sizeof(perr));
    if (loader_doc == NULL) {
        free(loader_json);
        if (base_doc != NULL) {
            sxcl_json_free(base_doc);
        }
        quilt_err(err, err_len, "拼出来的加载器 JSON 解析不了");
        return SXCL_LOADER_ERR_FORMAT;
    }
    char *final_json = NULL;
    int flattened = 0;
    if (base_doc != NULL) {
        if (sxcl_loader_flatten_json(sxcl_json_root(loader_doc), sxcl_json_root(base_doc), instance,
                                     req->mc_version, &final_json, perr, sizeof(perr)) !=
            SXCL_LOADER_OK) {
            char text[360];
            (void)snprintf(text, sizeof(text), "与原版拍平失败: %s", perr[0] ? perr : "原因不明");
            quilt_err(err, err_len, text);
            sxcl_json_free(loader_doc);
            sxcl_json_free(base_doc);
            free(loader_json);
            return SXCL_LOADER_ERR_FORMAT;
        }
        flattened = 1;
    } else {
        final_json = loader_json; /* 交接所有权 */
        loader_json = NULL;
    }
    sxcl_json_free(loader_doc);
    if (base_doc != NULL) {
        sxcl_json_free(base_doc);
    }

    /* ── 4. 按清单下库(**先下库、后写 JSON**) ── */
    sxcl_json *flat_doc = sxcl_json_parse(final_json, strlen(final_json), perr, sizeof(perr));
    if (flat_doc == NULL) {
        free(final_json);
        quilt_err(err, err_len, "拍平后的版本 JSON 解析不了");
        return SXCL_LOADER_ERR_FORMAT;
    }
    sxcl_loader_library *libs =
        (sxcl_loader_library *)calloc(QUILT_MAX_LIBS, sizeof(sxcl_loader_library));
    const size_t n = libs != NULL ? sxcl_loader_collect_libraries(flat_doc, "", libs,
                                                                  QUILT_MAX_LIBS)
                                  : 0;
    sxcl_json_free(flat_doc);
    if (libs == NULL || n == 0) {
        free(libs);
        free(final_json);
        quilt_err(err, err_len, "清单里一件库都没有(meta 形态变了?)");
        return SXCL_LOADER_ERR_FORMAT;
    }
    if (out != NULL) {
        out->libraries_total = (int)n;
    }

    sxcl_task *tasks = (sxcl_task *)calloc(n, sizeof(sxcl_task));
    char(*dests)[1200] = (char(*)[1200])calloc(n, 1200);
    char(*urls)[1600] = (char(*)[1600])calloc(n, 1600);
    if (tasks == NULL || dests == NULL || urls == NULL) {
        free(tasks);
        free(dests);
        free(urls);
        free(libs);
        free(final_json);
        quilt_err(err, err_len, "内存不足");
        return SXCL_LOADER_ERR_NOMEM;
    }

    int sidecar_fixed = 0;
    int patch_missed = 0;
    for (size_t i = 0; i < n; ++i) {
        if (quilt_cancelled(req)) {
            free(tasks);
            free(dests);
            free(urls);
            free(libs);
            free(final_json);
            quilt_err(err, err_len, "已取消(下库之前)");
            return SXCL_LOADER_ERR_IO;
        }
        /* 哈希侧车纠偏:meta 的 sha1 与 maven 自己的 .sha1 不一致时**以侧车为准**并计数。
         * 实测这两种都对不上(quilt-loader / hashed),硬拿 meta 的哈希校验会让 Quilt 永远装不上,
         * 而且报"校验失败"看着像我们下坏了 —— 纠正必须说出来,计数进 result。 */
        int sidecar_ok = 0;
        if (libs[i].sha1[0] != '\0' && libs[i].url_full[0] != '\0' && !quilt_skip_sidecar()) {
            char sidecar_url[1700];
            (void)snprintf(sidecar_url, sizeof(sidecar_url), "%s.sha1", libs[i].url_full);
            /* 侧车要**重试**:它只有 41 字节,可那家源(实测 maven.quiltmc.org)会 30 秒超时 ——
             * 拿不到侧车就只能拿 meta 的旧哈希去校验,又会以"校验失败"告终(兜底见 §4.5)。 */
            for (int attempt = 0; attempt < QUILT_SIDECAR_TRIES && !sidecar_ok; ++attempt) {
                if (quilt_cancelled(req)) {
                    break;
                }
                char *side = NULL;
                char serr[128];
                serr[0] = '\0';
                if (sxcl_install_http_get_text((void *)req->engine_opts, sidecar_url, &side, serr,
                                               sizeof(serr)) == 0 &&
                    side != NULL) {
                    char hex[48];
                    hex[0] = '\0';
                    if (quilt_parse_sidecar(side, hex, sizeof(hex))) {
                        char have[48];
                        quilt_lower_hex(libs[i].sha1, have, sizeof(have));
                        sidecar_ok = 1;
                        if (strcmp(hex, have) != 0) {
                            (void)snprintf(libs[i].sha1, sizeof(libs[i].sha1), "%s", hex);
                            /* 光纠下载任务不够:要落盘的 JSON 里也得换成真值 —— 否则启动器
                             * 自己的「启动前补全」会把刚装好的文件判成"校验失败"并每回重下
                             * (实测这两件每次启动都报 2 件失败,属于自证其罪)。 */
                            if (!sxcl_loader_patch_library_sha1(final_json, libs[i].path, hex)) {
                                patch_missed = 1;
                                SXCL_LOG_W("loader", "侧车纠偏落不回版本 JSON: %s(下次启动会报校验失败)",
                                           libs[i].name);
                            }
                            ++sidecar_fixed;
                        }
                    }
                    free(side);
                }
            }
        }
        /* 有 meta 哈希、却没拿到侧车:这份哈希**可能是旧的**,不能拿它硬校验(见 §4.5 兜底)。 */
        const int hash_uncertain = libs[i].sha1[0] != '\0' && !sidecar_ok;

        (void)snprintf(dests[i], 1200, "%s/libraries/%s", req->game_dir, libs[i].path);
        if (libs[i].url_full[0] != '\0') {
            (void)snprintf(urls[i], 1600, "%s", libs[i].url_full);
        } else {
            const size_t ul = strlen(libs[i].url);
            const char *sep = (ul > 0 && libs[i].url[ul - 1] == '/') ? "" : "/";
            (void)snprintf(urls[i], 1600, "%s%s%s", libs[i].url, sep, libs[i].path);
        }
        if (sxcl_fs_mkdirs_for_file(dests[i]) != 0) {
            char text[1200];
            (void)snprintf(text, sizeof(text), "建不了目录: %s", dests[i]);
            quilt_err(err, err_len, text);
            free(tasks);
            free(dests);
            free(urls);
            free(libs);
            free(final_json);
            return SXCL_LOADER_ERR_IO;
        }
        tasks[i].dest = dests[i];
        tasks[i].urls[0] = urls[i];
        tasks[i].urls[1] = req->maven_mirror;
        tasks[i].algo = SXCL_HASH_SHA1;
        tasks[i].sha1 = (libs[i].sha1[0] != '\0' && !hash_uncertain) ? libs[i].sha1 : NULL;
        tasks[i].size = libs[i].size;
        tasks[i].priority = 10;
        tasks[i].label = libs[i].name;
    }
    if (out != NULL) {
        out->sha1_from_sidecar = sidecar_fixed;
    }

    sxcl_engine_opts opts = *req->engine_opts;
    opts.retry_per_source = req->retries > 0 ? req->retries : 2;
    quilt_bridge bridge;
    memset(&bridge, 0, sizeof(bridge));
    bridge.req = req;
    bridge.tasks = tasks;
    bridge.total = (int)n;
    bridge.base_percent = 25;
    bridge.span_percent = 65;
    bridge.forward = req->engine_opts->on_progress;
    bridge.forward_ud = req->engine_opts->userdata;
    /* 引擎的 on_progress 是**替换**不是链式:这里想数进度就得占它;调用方原来那个
     * (界面/命令行给用户看的那条)由我们自己在桥里转发不了 —— 于是干脆不转发,
     * 进度统一走 req->on_progress(界面已经接了它,见 launch_worker/install_worker)。 */
    opts.on_progress = quilt_bridge_on_progress;
    opts.userdata = &bridge;

    sxcl_engine *engine = sxcl_engine_create(&opts);
    if (engine == NULL) {
        free(tasks);
        free(dests);
        free(urls);
        free(libs);
        free(final_json);
        quilt_err(err, err_len, "下载引擎起不来");
        return SXCL_LOADER_ERR_IO;
    }
    int submit_failed = 0;
    for (size_t i = 0; i < n; ++i) {
        if (sxcl_engine_submit(engine, &tasks[i]) != 0) {
            submit_failed = 1;
            break;
        }
    }
    int run_failed = 0;
    if (!submit_failed) {
        run_failed = sxcl_engine_run(engine) > 0 ? 1 : 0;
    }
    (void)run_failed;
    sxcl_engine_destroy(engine); /* 任务表必须活到这一步之后 */

    /* ── 4.5 哈希兜底:侧车没拿到的那几件,事后用"再下一份做对照"定性 ──
     * 为什么必须有它(真机):maven.quiltmc.org 的 .sha1 侧车会 30 秒超时(同一轮里一次超时、
     * 一次正常)。拿不到侧车就只能拿 meta 的哈希去校验,而 meta 那两件(quilt-loader / hashed)
     * 本来就是旧值 —— 于是安装以"校验失败"告终,可下下来的文件完全正常。
     * 口径(绝不静默、绝不挑一份了事):
     *   * 引擎按真哈希验过的任务(verify_state == HASH)不进这里 —— 实际值必然相符;
     *   * 剩下"下完了、但记录的 sha1 和磁盘对不上"的,**再下一份**(不带哈希);
     *     两份独立传输内容一致 = 上游哈希过期 -> 按**实际内容**记进版本 JSON,计数进 result;
     *     两份不一致 = 说不清哪份对 -> 照旧失败,报清楚话。 */
    int actual_adopted = 0;
    int sha1_unverified = 0;
    {
        size_t *cand = (size_t *)calloc(n, sizeof(size_t));
        char(*again)[1300] = (char(*)[1300])calloc(n, 1300);
        sxcl_task *vtasks = (sxcl_task *)calloc(n, sizeof(sxcl_task));
        if (cand != NULL && again != NULL && vtasks != NULL) {
            size_t vcount = 0;
            for (size_t i = 0; i < n; ++i) {
                if (tasks[i].state != SXCL_TASK_DONE || libs[i].sha1[0] == '\0' ||
                    tasks[i].verify_state == SXCL_TASK_VERIFY_HASH) {
                    continue;
                }
                char have[48];
                char want[48];
                have[0] = '\0';
                if (sxcl_hash_file(dests[i], SXCL_HASH_SHA1, have, sizeof(have)) != 0) {
                    continue; /* 读不了就当没这回事:下面的统计照旧按任务状态走 */
                }
                quilt_lower_hex(libs[i].sha1, want, sizeof(want));
                if (strcmp(have, want) == 0) {
                    continue; /* 实际内容 == 记录值(meta 是对的):什么都不用做 */
                }
                (void)snprintf(again[vcount], 1300, "%s.sxcl-verify", dests[i]);
                (void)sxcl_fs_remove(again[vcount]);
                vtasks[vcount].dest = again[vcount];
                vtasks[vcount].urls[0] = urls[i];
                vtasks[vcount].urls[1] = req->maven_mirror;
                vtasks[vcount].algo = SXCL_HASH_SHA1;
                vtasks[vcount].sha1 = NULL; /* 这一份只作对照,不校验 */
                vtasks[vcount].size = libs[i].size;
                vtasks[vcount].priority = 11;
                vtasks[vcount].label = libs[i].name;
                cand[vcount] = i;
                ++vcount;
            }
            if (vcount > 0 && !quilt_cancelled(req)) {
                quilt_progress(req, 92, "上游哈希与实下文件对不上,再下一份做对照");
                sxcl_engine_opts vopts = opts;
                vopts.on_progress = NULL; /* 那一份不进进度(它只是对照件) */
                vopts.userdata = NULL;
                sxcl_engine *veng = sxcl_engine_create(&vopts);
                if (veng != NULL) {
                    for (size_t k = 0; k < vcount; ++k) {
                        if (sxcl_engine_submit(veng, &vtasks[k]) != 0) {
                            break;
                        }
                    }
                    (void)sxcl_engine_run(veng);
                    sxcl_engine_destroy(veng);
                }
                for (size_t k = 0; k < vcount; ++k) {
                    const size_t i = cand[k];
                    char first[48];
                    char second[48];
                    first[0] = '\0';
                    second[0] = '\0';
                    const int got_second =
                        vtasks[k].state == SXCL_TASK_DONE &&
                        sxcl_hash_file(again[k], SXCL_HASH_SHA1, second, sizeof(second)) == 0;
                    /* 第一份的实际哈希(上面算过一次,这里重算:几十毫秒,换代码直白) */
                    const int got_first =
                        sxcl_hash_file(dests[i], SXCL_HASH_SHA1, first, sizeof(first)) == 0;
                    if (got_first && got_second && strcmp(first, second) == 0) {
                        if (!sxcl_loader_patch_library_sha1(final_json, libs[i].path, first)) {
                            patch_missed = 1;
                            SXCL_LOG_W("loader", "实际哈希落不回版本 JSON: %s", libs[i].name);
                        }
                        (void)snprintf(libs[i].sha1, sizeof(libs[i].sha1), "%s", first);
                        ++actual_adopted;
                        SXCL_LOG_I("loader", "上游 sha1 是旧值,已按实际内容记录: %s", libs[i].name);
                    } else if (got_first && !got_second && tasks[i].source_index == 0) {
                        /* 对照件没下来(那家源在这种时候本来就抖),可这一份是**官方 maven 直连
                         * 整份下来的**:TLS + Content-Length + 实际字节数都对得上,而上游哪儿都
                         * 拿不到哈希(meta 是旧值、侧车超时) —— 按实际内容记录并**如实计数**,
                         * 不假装"校验通过"。镜像来的那一份不走这条路(镜像没有这个豁免)。 */
                        if (!sxcl_loader_patch_library_sha1(final_json, libs[i].path, first)) {
                            patch_missed = 1;
                            SXCL_LOG_W("loader", "实际哈希落不回版本 JSON: %s", libs[i].name);
                        }
                        (void)snprintf(libs[i].sha1, sizeof(libs[i].sha1), "%s", first);
                        ++sha1_unverified;
                        SXCL_LOG_W("loader",
                                   "侧车与对照件都拿不到,按官方源实际内容记录(未二次确认): %s",
                                   libs[i].name);
                    } else if (got_first && got_second) {
                        tasks[i].state = SXCL_TASK_FAILED;
                        (void)snprintf(tasks[i].error, sizeof(tasks[i].error),
                                       "记录的 sha1 与实下文件不符,再下一份内容也不一样(上游不稳)");
                    } else {
                        tasks[i].state = SXCL_TASK_FAILED;
                        (void)snprintf(tasks[i].error, sizeof(tasks[i].error),
                                       "记录的 sha1 与实下文件不符,对照件也没下来:%s",
                                       vtasks[k].error[0] ? vtasks[k].error : "原因不明");
                    }
                    (void)sxcl_fs_remove(again[k]);
                }
            }
        }
        free(cand);
        free(again);
        free(vtasks);
    }
    if (out != NULL) {
        out->sha1_from_actual = actual_adopted;
        out->sha1_unverified = sha1_unverified;
    }

    int downloaded = 0;
    int failed = 0;
    int64_t bytes = 0;
    char first_error[256];
    first_error[0] = '\0';
    for (size_t i = 0; i < n; ++i) {
        if (tasks[i].state == SXCL_TASK_DONE) {
            ++downloaded;
            bytes += tasks[i].bytes_done;
        } else if (tasks[i].state == SXCL_TASK_FAILED) {
            ++failed;
            if (first_error[0] == '\0') {
                (void)snprintf(first_error, sizeof(first_error), "%s: %s", libs[i].name,
                               tasks[i].error[0] ? tasks[i].error : "下载失败");
            }
        }
    }
    if (out != NULL) {
        out->libraries_downloaded = downloaded;
        out->libraries_failed = failed;
        out->bytes_done = bytes;
    }
    const int cancelled = quilt_cancelled(req);
    free(tasks);
    free(dests);
    free(urls);
    free(libs);

    if (submit_failed || failed > 0) {
        char text[420];
        (void)snprintf(text, sizeof(text), "%s(N 件里 %d 件没下来: %s)", "依赖库没下齐",
                       failed > 0 ? failed : (int)n - downloaded,
                       first_error[0] ? first_error : "任务入队失败");
        quilt_err(err, err_len, text);
        free(final_json);
        return SXCL_LOADER_ERR_IO;
    }
    if (cancelled) {
        free(final_json);
        quilt_err(err, err_len, "已取消(库下完、还没写版本 JSON)");
        return SXCL_LOADER_ERR_IO;
    }

    /* ── 5. **最后**写版本 JSON(P1 的纪律:库齐了才有"装好了"这一说) ── */
    quilt_progress(req, 94, "写版本 JSON");
    char dir[1200];
    char json_path[1400];
    (void)snprintf(dir, sizeof(dir), "%s/versions/%s", req->game_dir, instance);
    (void)snprintf(json_path, sizeof(json_path), "%s/%s.json", dir, instance);
    if (sxcl_fs_mkdirs(dir) != 0 || quilt_write_file(json_path, final_json) != 0) {
        char text[1400];
        (void)snprintf(text, sizeof(text), "写不了版本 JSON: %s", json_path);
        quilt_err(err, err_len, text);
        free(final_json);
        return SXCL_LOADER_ERR_IO;
    }
    if (out != NULL) {
        (void)snprintf(out->version_json, sizeof(out->version_json), "%s", json_path);
    }
    free(final_json);
    if (patch_missed) {
        quilt_progress(req, 100, "装好了(有哈希纠偏没能落进版本 JSON,下次启动可能报校验失败)");
    } else {
        quilt_progress(req, 100,
                       flattened ? "装好了(已与原版拍平)" : "装好了(没找到原版,只写了加载器层)");
    }
    return SXCL_LOADER_OK;
}
