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
#include "sxcl/loader_catalog.h" /* sxcl_loader_quilt_loader_json */

#define QUILT_META_DEFAULT "https://meta.quiltmc.org/v3/versions/loader"
#define QUILT_MAX_LIBS 256

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
        if (libs[i].sha1[0] != '\0' && libs[i].url_full[0] != '\0') {
            char sidecar_url[1700];
            (void)snprintf(sidecar_url, sizeof(sidecar_url), "%s.sha1", libs[i].url_full);
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
                    size_t hk = 0;
                    for (const char *p = libs[i].sha1; *p != '\0' && hk < 40; ++p) {
                        const char c = *p;
                        have[hk++] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
                    }
                    have[hk] = '\0';
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
        tasks[i].sha1 = libs[i].sha1[0] != '\0' ? libs[i].sha1 : NULL;
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
    sxcl_engine_destroy(engine); /* 任务表必须活到这一步之后 */
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
