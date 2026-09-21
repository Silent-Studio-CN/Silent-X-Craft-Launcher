/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/install.h"
#include "sxcl/manifest.h"
#include "sxcl/natives.h"
#include "sxcl/net.h"

/* ── 阶段表(顺序 = 枚举数值 = 执行顺序) ── */

static const char *const kStageIds[] = {
    "manifest",   "version_json", "client_jar",     "libraries", "asset_index",
    "asset_objects", "loader_installer", "loader_run", "natives", "finish",
};

static const char *const kStageNames[] = {
    "获取版本清单", "下载并解析版本 JSON", "下载客户端 jar", "下载依赖库", "下载资源索引",
    "下载资源对象", "下载加载器安装器", "执行加载器安装", "解压 natives", "整理文件",
};

/* 表长与枚举不一致时直接编不过(阶段表是 UI 与日志的公共契约,不许悄悄漂移)。 */
_Static_assert(sizeof(kStageIds) / sizeof(kStageIds[0]) == (size_t)SXCL_INSTALL_STAGE_COUNT,
               "kStageIds 长度与 sxcl_install_stage 不一致");
_Static_assert(sizeof(kStageNames) / sizeof(kStageNames[0]) == (size_t)SXCL_INSTALL_STAGE_COUNT,
               "kStageNames 长度与 sxcl_install_stage 不一致");

const char *sxcl_install_stage_id(sxcl_install_stage stage) {
    if (stage == SXCL_INSTALL_STAGE_END) {
        return "none"; /* 没失败(哨兵值) */
    }
    if (stage < 0 || (int)stage >= SXCL_INSTALL_STAGE_COUNT) {
        return "unknown";
    }
    return kStageIds[(int)stage];
}

const char *sxcl_install_stage_name(sxcl_install_stage stage) {
    if (stage == SXCL_INSTALL_STAGE_END) {
        return "未失败";
    }
    if (stage < 0 || (int)stage >= SXCL_INSTALL_STAGE_COUNT) {
        return "未知阶段";
    }
    return kStageNames[(int)stage];
}

int sxcl_install_stage_is_loader(sxcl_install_stage stage) {
    return (stage == SXCL_INSTALL_STAGE_LOADER_INSTALLER || stage == SXCL_INSTALL_STAGE_LOADER_RUN) ? 1 : 0;
}

const char *sxcl_install_code_name(int code) {
    switch (code) {
    case SXCL_INSTALL_OK:                return "ok";
    case SXCL_INSTALL_ERR_ARG:           return "arg";
    case SXCL_INSTALL_ERR_MANIFEST:      return "manifest";
    case SXCL_INSTALL_ERR_VERSION:       return "version";
    case SXCL_INSTALL_ERR_CLIENT_JAR:    return "client_jar";
    case SXCL_INSTALL_ERR_LOADER:        return "loader";
    case SXCL_INSTALL_ERR_NATIVES:       return "natives";
    case SXCL_INSTALL_ERR_IO:            return "io";
    case SXCL_INSTALL_ERR_NOMEM:         return "nomem";
    case SXCL_INSTALL_ERR_CANCELLED:     return "cancelled";
    default:                             return "unknown";
    }
}

int sxcl_install_code_retryable(int code) {
    switch (code) {
    case SXCL_INSTALL_ERR_MANIFEST:
    case SXCL_INSTALL_ERR_VERSION:
    case SXCL_INSTALL_ERR_CLIENT_JAR:
    case SXCL_INSTALL_ERR_LOADER:
    case SXCL_INSTALL_ERR_IO:
        return 1; /* 网络/镜像抽风一类:重试往往就好了 */
    default:
        return 0; /* 参数/内存/natives/取消:重试也一样 */
    }
}

/* ── 计划查询 ── */

/** 资源级别的归一值:0 = 不碰资源,1 = 只下索引,2 = 全量。 */
static int assets_level(const sxcl_install_plan *plan) {
    if (!plan) {
        return 0;
    }
    switch (plan->assets) {
    case SXCL_INSTALL_ASSETS_NONE:
        return 0;
    case SXCL_INSTALL_ASSETS_INDEX:
        return 1;
    case SXCL_INSTALL_ASSETS_DEFAULT:
    case SXCL_INSTALL_ASSETS_FULL:
    default:
        return 2;
    }
}

int sxcl_install_plan_has_loader(const sxcl_install_plan *plan) {
    if (!plan) {
        return 0;
    }
    const int kind = (int)plan->loader;
    return (kind > (int)SXCL_LOADER_VANILLA && kind <= (int)SXCL_LOADER_OPTIFINE) ? 1 : 0;
}

const char *sxcl_install_plan_instance(const sxcl_install_plan *plan) {
    if (!plan || !plan->version_id) {
        return "";
    }
    if (plan->instance_name && *plan->instance_name) {
        return plan->instance_name;
    }
    return plan->version_id;
}

int sxcl_install_plan_stage_included(const sxcl_install_plan *plan, sxcl_install_stage stage) {
    if (!plan || (int)stage < 0 || (int)stage >= SXCL_INSTALL_STAGE_COUNT) {
        return 0;
    }
    switch (stage) {
    case SXCL_INSTALL_STAGE_MANIFEST:
    case SXCL_INSTALL_STAGE_VERSION_JSON:
    case SXCL_INSTALL_STAGE_CLIENT_JAR:
    case SXCL_INSTALL_STAGE_LIBRARIES:
    case SXCL_INSTALL_STAGE_NATIVES:
    case SXCL_INSTALL_STAGE_FINISH:
        return 1;
    case SXCL_INSTALL_STAGE_ASSET_INDEX:
        return assets_level(plan) >= 1 ? 1 : 0;
    case SXCL_INSTALL_STAGE_ASSET_OBJECTS:
        return assets_level(plan) >= 2 ? 1 : 0;
    case SXCL_INSTALL_STAGE_LOADER_INSTALLER:
    case SXCL_INSTALL_STAGE_LOADER_RUN:
        return sxcl_install_plan_has_loader(plan);
    default:
        return 0;
    }
}

static void build_stage_table(const sxcl_install_plan *plan, sxcl_install_stage *stages, size_t *count) {
    *count = 0;
    for (int i = 0; i < SXCL_INSTALL_STAGE_COUNT; ++i) {
        if (sxcl_install_plan_stage_included(plan, (sxcl_install_stage)i)) {
            stages[(*count)++] = (sxcl_install_stage)i;
        }
    }
}

size_t sxcl_install_plan_stage_count(const sxcl_install_plan *plan) {
    sxcl_install_stage list[SXCL_INSTALL_STAGE_COUNT];
    if (!plan) {
        return 0;
    }
    size_t count = 0;
    build_stage_table(plan, list, &count);
    return count;
}

sxcl_install_stage sxcl_install_plan_stage_at(const sxcl_install_plan *plan, size_t index) {
    sxcl_install_stage list[SXCL_INSTALL_STAGE_COUNT];
    size_t count = 0;
    if (!plan) {
        return SXCL_INSTALL_STAGE_END;
    }
    build_stage_table(plan, list, &count);
    return index < count ? list[index] : SXCL_INSTALL_STAGE_END;
}

/* ── 小工具 ── */

static char *dup_cstr(const char *s) {
    const size_t n = s ? strlen(s) : 0;
    char *p = (char *)malloc(n + 1);
    if (!p) {
        return NULL;
    }
    if (n) {
        memcpy(p, s, n);
    }
    p[n] = '\0';
    return p;
}

static void set_text(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    if (!dst || cap == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    dst[cap - 1] = '\0';
}

/** 往缓冲末尾追加(截断而不是溢出;不用 strncat —— MSVC 会为它报 C4996)。 */
static void append_text(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0 || !src) {
        return;
    }
    const size_t used = strlen(dst);
    if (used + 1 >= cap) {
        return;
    }
    size_t room = cap - used - 1;
    size_t len = strlen(src);
    if (len > room) {
        len = room;
    }
    memcpy(dst + used, src, len);
    dst[used + len] = '\0';
}

/** dir + "/" + rel(与 manifest.c 的 join_path 同一套规则:dir 末尾没分隔符就补一个)。 */
static int path_join(char *out, size_t cap, const char *dir, const char *rel) {
    if (!out || cap == 0 || !dir || !rel) {
        return -1;
    }
    const size_t a = strlen(dir);
    const size_t b = strlen(rel);
    const int need = (a > 0 && dir[a - 1] != '/' && dir[a - 1] != '\\') ? 1 : 0;
    if (a + (size_t)need + b + 1 > cap) {
        return -1;
    }
    memcpy(out, dir, a);
    size_t n = a;
    if (need) {
        out[n++] = '/';
    }
    memcpy(out + n, rel, b + 1);
    return 0;
}

/* ── 把版本 JSON 放到启动层要的位置(见 install.h 的说明) ──
 * 下载路径(sxcl-dl version)与安装路径(sxcl_install_run)都走这里,
 * 免得两边对"版本 JSON 该放哪"有不同理解。 */
int sxcl_install_write_version_json(const char *game_dir, const char *version_id,
                                   const char *json_path, char *err, size_t err_len) {
    if (game_dir == NULL || game_dir[0] == 0 || version_id == NULL || version_id[0] == 0 ||
        json_path == NULL || json_path[0] == 0) {
        set_text(err, err_len, "参数不合法(游戏目录/版本名/JSON 路径都不能为空)");
        return SXCL_INSTALL_ERR_ARG;
    }
    /* 版本名是路径的一段:只允许安全字符,不让它跳出 versions/ */
    for (const char *p = version_id; *p != 0; ++p) {
        const char c = *p;
        const int ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            set_text(err, err_len, "版本名里有不允许的字符(只允许字母数字与 . _ -):%s", version_id);
            return SXCL_INSTALL_ERR_ARG;
        }
    }
    if (strstr(version_id, "..") != NULL) {
        set_text(err, err_len, "版本名里不允许出现 ..:%s", version_id);
        return SXCL_INSTALL_ERR_ARG;
    }

    char rel[SXCL_INSTALL_PATH_MAX];
    char dest[SXCL_INSTALL_PATH_MAX];
    set_text(rel, sizeof(rel), "versions/%s/%s.json", version_id, version_id);
    if (path_join(dest, sizeof(dest), game_dir, rel) != 0) {
        set_text(err, err_len, "版本 JSON 路径太长:%s", rel);
        return SXCL_INSTALL_ERR_ARG;
    }

    FILE *src = sxcl_fs_fopen(json_path, "rb");
    if (src == NULL) {
        set_text(err, err_len, "读不了版本 JSON 源文件:%s", json_path);
        return SXCL_INSTALL_ERR_IO;
    }
    char tmp[SXCL_INSTALL_PATH_MAX + 8];
    set_text(tmp, sizeof(tmp), "%s.tmp", dest);
    if (sxcl_fs_mkdirs_for_file(dest) != 0) {
        fclose(src);
        set_text(err, err_len, "建不了版本目录:%s", dest);
        return SXCL_INSTALL_ERR_IO;
    }
    FILE *dst = sxcl_fs_fopen(tmp, "wb");
    if (dst == NULL) {
        fclose(src);
        set_text(err, err_len, "写不了版本 JSON(临时文件):%s", tmp);
        return SXCL_INSTALL_ERR_IO;
    }
    char buf[16 * 1024];
    int failed = 0;
    for (;;) {
        const size_t got = fread(buf, 1, sizeof(buf), src);
        if (got > 0 && fwrite(buf, 1, got, dst) != got) {
            failed = 1;
            break;
        }
        if (got < sizeof(buf)) {
            break;
        }
    }
    fclose(src);
    if (fclose(dst) != 0) {
        failed = 1;
    }
    if (failed) {
        (void)sxcl_fs_remove(tmp);
        set_text(err, err_len, "版本 JSON 写不完整:%s", tmp);
        return SXCL_INSTALL_ERR_IO;
    }
    if (sxcl_fs_rename_replace(tmp, dest) != 0) {
        (void)sxcl_fs_remove(tmp);
        set_text(err, err_len, "版本 JSON 改名失败(目标被占用?):%s", dest);
        return SXCL_INSTALL_ERR_IO;
    }
    return 0;
}

/** 取路径最后一段(用于"当前文件"显示)。 */
static const char *path_leaf(const char *path) {
    const char *leaf = path ? path : "";
    for (const char *p = leaf; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            leaf = p + 1;
        }
    }
    return leaf;
}

/* ── 运行状态 ── */

typedef struct install_run {
    const sxcl_install_request *req;
    const sxcl_install_plan *plan;
    const sxcl_install_io *io;
    sxcl_install_result *out;

    const char *game_dir;
    const char *instance;
    const char *asset_index_id;

    char version_json_path[SXCL_INSTALL_PATH_MAX];
    char client_jar_path[SXCL_INSTALL_PATH_MAX];
    char asset_index_path[SXCL_INSTALL_PATH_MAX];
    char natives_dir[SXCL_INSTALL_PATH_MAX];
    char installer_path[SXCL_INSTALL_PATH_MAX];
    int installer_downloaded; /* 1 = 安装器是本次下到的(收尾时按需删) */
    int has_installer_path;

    sxcl_json *manifest_doc;
    sxcl_version_list *versions;
    const sxcl_version_entry *entry;
    const char *version_json_url;
    const char *version_json_sha1;
    int64_t version_json_size;

    sxcl_json *version_doc;
    sxcl_version_plan *version_plan;

    sxcl_install_stage stages[SXCL_INSTALL_STAGE_COUNT];
    size_t stage_count;
    size_t stage_index;
    sxcl_install_stage stage;

    /* 当前批次(回报钩子要靠它算阶段内进度) */
    sxcl_task **cur_tasks;
    size_t cur_count;
    const char *cur_label;

    /* 复用缓冲:任务指针数组(每阶段重新填) */
    sxcl_task **batch;
    size_t batch_cap;

    int stage_percent;                  /* 当前阶段百分比(单调) */
    int last_percent;                   /* 整体百分比(单调) */
    /* 最近一次上报的文件/字节数(批次结束后 cur_tasks 会被清掉,阶段结束事件还要用它) */
    size_t ev_files_total;
    size_t ev_files_done;
    size_t ev_files_skipped;
    size_t ev_files_failed;
    int64_t ev_bytes_done;
    int64_t ev_bytes_total;
    int cancelled;
    size_t files_skipped_total;
    size_t files_failed_total;
    int64_t bytes_done_total;
    char summary[SXCL_INSTALL_STATUS_MAX]; /* 阶段结束时的人话总结 */
} install_run;

static int fail(install_run *r, int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->out->error, sizeof(r->out->error), fmt, ap);
    va_end(ap);
    r->out->error[sizeof(r->out->error) - 1] = '\0';
    return code;
}

static int check_cancel(install_run *r) {
    if (r->cancelled) {
        return 1;
    }
    if (r->req->is_cancelled && r->req->is_cancelled(r->req->cancel_userdata) != 0) {
        r->cancelled = 1;
        return 1;
    }
    return 0;
}

/* ── 进度上报 ── */

/** 本批任务的合计(文件与字节)。任务数很多时是 O(n),但只在有进度事件时算。 */
static void batch_totals(install_run *r, size_t *done, size_t *failed, size_t *skipped,
                         int64_t *bytes_done, int64_t *bytes_total) {
    size_t d = 0, f = 0, s = 0;
    int64_t bd = 0, bt = 0;
    for (size_t i = 0; i < r->cur_count; ++i) {
        const sxcl_task *t = r->cur_tasks[i];
        if (!t) {
            continue;
        }
        if (t->size > 0) {
            bt += t->size;
        }
        if (t->bytes_done > 0) {
            bd += t->bytes_done;
        }
        if (t->state == SXCL_TASK_FAILED) {
            ++f;
        } else if (t->state == SXCL_TASK_DONE) {
            ++d;
            /* engine.c 的快路径把这句话写进 error(见 run_task):用它统计"跳过",判定仍归引擎。 */
            if (t->error[0] && strcmp(t->error, "已存在且校验通过") == 0) {
                ++s;
            }
        }
    }
    *done = d;
    *failed = f;
    *skipped = s;
    *bytes_done = bd;
    *bytes_total = bt;
}

static void emit_progress(install_run *r, sxcl_install_event event, int stage_percent, const char *status,
                          const sxcl_task *task) {
    if (!r->req->on_progress) {
        return;
    }
    if (stage_percent < 0) {
        stage_percent = 0;
    }
    if (stage_percent > 100) {
        stage_percent = 100;
    }
    if (stage_percent > r->stage_percent) {
        r->stage_percent = stage_percent;
    }
    stage_percent = r->stage_percent;

    size_t total_stages = r->stage_count ? r->stage_count : 1;
    size_t index = r->stage_index < r->stage_count ? r->stage_index : (r->stage_count ? r->stage_count - 1 : 0);
    int percent = (int)((index * 100 + (size_t)stage_percent) / total_stages);
    if (percent > 100) {
        percent = 100;
    }
    if (percent < r->last_percent) {
        percent = r->last_percent;
    } else {
        r->last_percent = percent;
    }

    sxcl_install_progress p;
    memset(&p, 0, sizeof(p));
    p.event = event;
    p.stage = r->stage;
    p.stage_index = index;
    p.stage_total = r->stage_count;
    p.stage_percent = stage_percent;
    p.percent = percent;

    if (r->cur_count > 0 && r->cur_tasks) {
        size_t done = 0, failed = 0, skipped = 0;
        int64_t bd = 0, bt = 0;
        batch_totals(r, &done, &failed, &skipped, &bd, &bt);
        r->ev_files_total = r->cur_count;
        r->ev_files_done = done;
        r->ev_files_skipped = skipped;
        r->ev_files_failed = failed;
        r->ev_bytes_done = bd;
        r->ev_bytes_total = bt;
    }
    p.bytes_done = r->ev_bytes_done;
    p.bytes_total = r->ev_bytes_total;
    p.files_done = r->ev_files_done;
    p.files_total = r->ev_files_total;
    p.files_skipped = r->ev_files_skipped;
    p.files_failed = r->ev_files_failed;

    set_text(p.status, sizeof(p.status), "%s",
             (status && *status) ? status : sxcl_install_stage_name(r->stage));
    if (task) {
        const char *label = task->label ? task->label : (task->dest ? path_leaf(task->dest) : "");
        set_text(p.current, sizeof(p.current), "%s", label);
    } else if (r->cur_label) {
        set_text(p.current, sizeof(p.current), "%s", r->cur_label);
    }
    r->req->on_progress(r->req->userdata, &p);
}

/** 下载阶段的进度文本(人话;对齐 Python 版的明细格式,ETA 引擎没给就不编)。 */
static void download_status(install_run *r, char *out, size_t cap) {
    size_t done = 0, failed = 0, skipped = 0;
    int64_t bd = 0, bt = 0;
    batch_totals(r, &done, &failed, &skipped, &bd, &bt);
    (void)skipped;
    (void)failed;
    const double mb_done = (double)bd / (1024.0 * 1024.0);
    const double mb_total = (double)bt / (1024.0 * 1024.0);
    double speed = 0.0;
    const char *leaf = "";
    for (size_t i = 0; i < r->cur_count; ++i) {
        const sxcl_task *t = r->cur_tasks[i];
        if (!t) {
            continue;
        }
        if (t->speed_bps > speed) {
            speed = t->speed_bps;
        }
        if (t->state == SXCL_TASK_RUNNING && t->label) {
            leaf = path_leaf(t->label);
        }
    }
    if (r->cur_count <= 1) {
        set_text(out, cap, "%s: %.1f/%.1f MB | %.1f MB/s | %s", r->cur_label ? r->cur_label : "下载",
                 mb_done, mb_total, speed / (1024.0 * 1024.0), leaf);
    } else {
        set_text(out, cap, "%s: %zu/%zu 个文件 | %.1f/%.1f MB | %.1f MB/s | %s",
                 r->cur_label ? r->cur_label : "下载", done, r->cur_count, mb_done, mb_total,
                 speed / (1024.0 * 1024.0), leaf);
    }
}

/** 当前批次已经完成的百分比(文件数与字节数两个口径取大的那个,两个都单调不减)。 */
static int stage_percent_of(install_run *r) {
    if (r->cur_count == 0 || !r->cur_tasks) {
        return r->stage_percent;
    }
    size_t done = 0, failed = 0, skipped = 0;
    int64_t bd = 0, bt = 0;
    batch_totals(r, &done, &failed, &skipped, &bd, &bt);
    const int by_files = (int)((done * 100) / r->cur_count);
    const int by_bytes = bt > 0 ? (int)((bd * 100) / bt) : 0;
    int percent = by_files > by_bytes ? by_files : by_bytes;
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    return percent;
}

/* 下载钩子里的回报:算进度 + 轮询取消(返回非 0 = 让下载器停下)。 */
static int install_report(void *userdata, const sxcl_task *task, size_t index, size_t done, size_t total) {
    install_run *r = (install_run *)userdata;
    (void)index;
    (void)done;
    (void)total;
    if (check_cancel(r)) {
        return 1;
    }
    char status[SXCL_INSTALL_STATUS_MAX];
    download_status(r, status, sizeof(status));
    emit_progress(r, SXCL_INSTALL_EVENT_STAGE_PROGRESS, stage_percent_of(r), status, task);
    return 0;
}

/* ── 一批下载 ── */

/** 下载钩子的取消查询(转发给编排层的 is_cancelled,顺带把"已取消"记下来)。 */
static int download_is_cancelled(void *userdata) {
    return check_cancel((install_run *)userdata);
}

/**
 * 把一批任务交给下载钩子,并按失败策略决定要不要中断。
 * 返回 0 = 可以继续(允许有非致命失败);负 = SXCL_INSTALL_ERR_*(整体中断)。
 */
static int download_batch(install_run *r, sxcl_install_stage stage, const char *label, sxcl_task **tasks,
                          size_t count, int fatal, int fail_code, const char *fail_what) {
    if (count == 0) {
        return 0;
    }
    r->cur_tasks = tasks;
    r->cur_count = count;
    r->cur_label = label;

    sxcl_install_download request;
    memset(&request, 0, sizeof(request));
    request.stage = stage;
    request.label = label;
    request.tasks = (sxcl_task *const *)tasks;
    request.count = count;
    request.fatal = fatal;
    request.engine_opts = r->plan->engine_opts;
    request.report = install_report;
    request.report_userdata = r;
    request.is_cancelled = download_is_cancelled;
    request.cancel_userdata = r;

    sxcl_install_download_stats stats;
    memset(&stats, 0, sizeof(stats));
    char err[SXCL_INSTALL_ERROR_MAX];
    err[0] = '\0';

    const int rc = r->io->download(r->io->userdata, &request, &stats, err, sizeof(err));

    if (check_cancel(r)) {
        return SXCL_INSTALL_ERR_CANCELLED;
    }
    if (rc < 0) {
        r->cur_tasks = NULL;
        r->cur_count = 0;
        return fail(r, fail_code, "%s下载失败: %s", fail_what, err[0] ? err : "下载器没跑起来");
    }

    size_t done = 0, failed = 0, skipped = 0;
    int64_t bd = 0, bt = 0;
    batch_totals(r, &done, &failed, &skipped, &bd, &bt);
    if (stats.files_failed > failed) {
        failed = stats.files_failed; /* 假实现可能只填统计不写任务状态 */
    }
    if (stats.files_skipped > skipped) {
        skipped = stats.files_skipped;
    }
    if (stats.files_done > done) {
        done = stats.files_done;
    }

    r->files_failed_total += failed;
    r->files_skipped_total += skipped;
    r->bytes_done_total += stats.bytes_done > 0 ? stats.bytes_done : bd;

    if (failed > 0 && fatal) {
        char names[200];
        names[0] = '\0';
        size_t listed = 0;
        for (size_t i = 0; i < count && listed < 3; ++i) {
            const sxcl_task *t = tasks[i];
            if (!t || t->state != SXCL_TASK_FAILED) {
                continue;
            }
            const char *name = t->label ? t->label : (t->dest ? path_leaf(t->dest) : "?");
            if (listed > 0) {
                append_text(names, sizeof(names), ", ");
            }
            append_text(names, sizeof(names), name);
            ++listed;
        }
        r->cur_tasks = NULL;
        r->cur_count = 0;
        if (listed > 0) {
            return fail(r, fail_code, "%s: %d/%d 个文件失败(例如 %s)", fail_what, (int)failed, (int)count,
                        names);
        }
        return fail(r, fail_code, "%s: %d/%d 个文件失败", fail_what, (int)failed, (int)count);
    }

    /* 阶段内进度收尾到 100% */
    {
        char status[SXCL_INSTALL_STATUS_MAX];
        download_status(r, status, sizeof(status));
        emit_progress(r, SXCL_INSTALL_EVENT_STAGE_PROGRESS, 100, status, NULL);
    }
    if (failed > 0) {
        set_text(r->summary, sizeof(r->summary), "%s: %zu 个文件(%zu 个失败,已记录)", label, count, failed);
    } else if (skipped > 0) {
        set_text(r->summary, sizeof(r->summary), "%s: %zu 个文件(%zu 个已存在,跳过下载)", label, count,
                 skipped);
    } else {
        set_text(r->summary, sizeof(r->summary), "%s: %zu 个文件完成", label, count);
    }
    r->cur_tasks = NULL;
    r->cur_count = 0;
    return 0;
}

/* ── 各阶段 ── */

/** 这次安装要用的镜像根:计划里给了就用它;prefer_mirror=1 且没给就用核心默认(BMCLAPI)。
 *  返回 NULL = 这次不补镜像(与老行为一致:官方一条路)。 */
static const char *install_mirror_base(const sxcl_install_plan *plan) {
    if (plan->mirror_base && *plan->mirror_base) {
        return plan->mirror_base;
    }
    return plan->prefer_mirror ? SXCL_MIRROR_BMCLAPI_BASE : NULL;
}

/** 资源对象的镜像根:plan.mirror_base 是"镜像根"(与 add_mirror 一个口径,如
 *  https://bmclapi2.bangbang93.com),资源对象在它下面的 /assets;调用方要是已经写成
 *  ".../assets" 就不再拼一层(免得出现 /assets/assets)。空 = 这次不配资源对象镜像。 */
static void install_asset_mirror_base(const sxcl_install_plan *plan, char *out, size_t cap) {
    out[0] = '\0';
    const char *root = install_mirror_base(plan);
    if (!root || !*root) {
        return;
    }
    const size_t n = strlen(root);
    static const char suffix[] = "/assets";
    const size_t sn = sizeof(suffix) - 1;
    if (n >= sn && strcmp(root + n - sn, suffix) == 0) {
        snprintf(out, cap, "%s", root);
    } else {
        snprintf(out, cap, "%s%s", root, suffix);
    }
}

static int stage_manifest(install_run *r) {
    char err[SXCL_INSTALL_ERROR_MAX];
    err[0] = '\0';
    char *text = NULL;

    if (r->plan->manifest_text && *r->plan->manifest_text) {
        text = dup_cstr(r->plan->manifest_text);
        if (!text) {
            return fail(r, SXCL_INSTALL_ERR_NOMEM, "内存不足(复制版本清单)");
        }
    } else {
        const char *url = (r->plan->manifest_url && *r->plan->manifest_url) ? r->plan->manifest_url
                                                                            : SXCL_INSTALL_MANIFEST_URL;
        /* 镜像优先:先试镜像,不通再走官方(自动切)。不开开关就一条路都不加,与老行为一致。 */
        char mirror[1024];
        const char *mirror_url = NULL;
        const char *base = install_mirror_base(r->plan);
        if (r->plan->prefer_mirror && base &&
            sxcl_manifest_mirror_url(url, base, mirror, sizeof(mirror)) == 0) {
            mirror_url = mirror;
        }
        const char *first = mirror_url ? mirror_url : url;
        if (r->io->fetch_text(r->io->userdata, first, &text, err, sizeof(err)) != 0 || !text) {
            if (!mirror_url) {
                return fail(r, SXCL_INSTALL_ERR_MANIFEST, "取版本清单失败: %s", err[0] ? err : "未知原因");
            }
            if (r->io->fetch_text(r->io->userdata, url, &text, err, sizeof(err)) != 0 || !text) {
                return fail(r, SXCL_INSTALL_ERR_MANIFEST, "取版本清单失败(镜像与官方都不通): %s",
                            err[0] ? err : "未知原因");
            }
        }
    }

    r->manifest_doc = sxcl_json_parse(text, strlen(text), err, sizeof(err));
    free(text);
    if (!r->manifest_doc) {
        return fail(r, SXCL_INSTALL_ERR_MANIFEST, "版本清单不是合法 JSON: %s", err[0] ? err : "解析失败");
    }
    r->versions = sxcl_version_list_build(r->manifest_doc);
    if (!r->versions || sxcl_version_list_count(r->versions) == 0) {
        return fail(r, SXCL_INSTALL_ERR_MANIFEST, "版本清单里没有 versions 数组");
    }
    r->entry = sxcl_version_list_find(r->versions, r->plan->version_id);
    if (!r->entry || !r->entry->url || !*r->entry->url) {
        return fail(r, SXCL_INSTALL_ERR_MANIFEST, "版本清单里没有版本 %s(清单共 %d 个版本)",
                    r->plan->version_id, (int)sxcl_version_list_count(r->versions));
    }
    r->version_json_url = r->entry->url;
    r->version_json_sha1 = (r->entry->sha1 && *r->entry->sha1) ? r->entry->sha1 : NULL;
    r->version_json_size = r->entry->size;

    set_text(r->summary, sizeof(r->summary), "版本清单: 共 %d 个版本,目标 %s",
             (int)sxcl_version_list_count(r->versions), r->plan->version_id);
    emit_progress(r, SXCL_INSTALL_EVENT_STAGE_PROGRESS, 100, r->summary, NULL);
    return 0;
}

static int stage_version_json(install_run *r) {
    sxcl_task task;
    memset(&task, 0, sizeof(task));
    task.dest = r->version_json_path;
    /* 版本 JSON 的第二路(镜像):镜像优先时它排第一,官方退成第二候选 */
    char vjson_mirror[1024];
    vjson_mirror[0] = '\0';
    const char *mirror_base = install_mirror_base(r->plan);
    const char *vjson_mirror_url = NULL;
    if (mirror_base && sxcl_manifest_mirror_url(r->version_json_url, mirror_base, vjson_mirror,
                                                sizeof(vjson_mirror)) == 0) {
        vjson_mirror_url = vjson_mirror;
    }
    if (r->plan->prefer_mirror && vjson_mirror_url) {
        task.urls[0] = vjson_mirror_url;
        task.urls[1] = r->version_json_url;
    } else {
        task.urls[0] = r->version_json_url;
        task.urls[1] = vjson_mirror_url;
    }
    task.sha1 = r->version_json_sha1;
    task.algo = SXCL_HASH_SHA1;
    task.size = r->version_json_size;
    task.priority = 0;
    task.label = "版本 JSON";

    sxcl_task *batch[1];
    batch[0] = &task;
    r->cur_tasks = batch;
    r->cur_count = 1;
    r->cur_label = "版本 JSON";
    const int rc = download_batch(r, SXCL_INSTALL_STAGE_VERSION_JSON, "版本 JSON", batch, 1, 1,
                                  SXCL_INSTALL_ERR_VERSION, "版本 JSON");
    if (rc != 0) {
        return rc;
    }

    char err[SXCL_INSTALL_ERROR_MAX];
    err[0] = '\0';
    r->version_doc = sxcl_json_parse_file(r->version_json_path, err, sizeof(err));
    if (!r->version_doc) {
        return fail(r, SXCL_INSTALL_ERR_VERSION, "版本 JSON 解析失败: %s", err[0] ? err : "解析失败");
    }

    const sxcl_json_value *root = sxcl_json_root(r->version_doc);
    const sxcl_json_value *assets = sxcl_json_get(root, "assetIndex");
    r->asset_index_id = sxcl_json_get_string(assets, "id", "assets");

    char rel[SXCL_INSTALL_PATH_MAX];
    set_text(rel, sizeof(rel), "assets/indexes/%s.json", r->asset_index_id);
    if (path_join(r->asset_index_path, sizeof(r->asset_index_path), r->game_dir, rel) != 0) {
        return fail(r, SXCL_INSTALL_ERR_ARG, "资源索引路径太长: %s/%s", r->game_dir, rel);
    }

    /* 下载计划:客户端 jar + 资源索引 + 依赖库(资源对象稍后在它自己的阶段里追加) */
    r->version_plan = sxcl_version_plan_build(r->version_doc, r->game_dir, r->instance, err, sizeof(err));
    if (!r->version_plan) {
        return fail(r, SXCL_INSTALL_ERR_VERSION, "版本 JSON 生成下载计划失败: %s", err[0] ? err : "未知原因");
    }

    /* 计划装配完、开始下载(客户端 jar/依赖库/资源索引这一批)之前:
     *   1) 给每个文件补一条镜像第二路 —— 以前 install.c 只给资源对象配了镜像,
     *      客户端 jar/依赖库/资源索引只有官方一条路,设置里的下载源对它们等于没有;
     *   2) prefer_mirror=1 时把镜像挪到第一路(官方自动退成第二候选)。
     * 注意 prefer_mirror **只调这一次**(见 manifest.h):资源对象是后面才追加的另一批,
     * 它们的顺序在 stage_asset_objects 里按同一个开关一次定好,不能再调第二次(会换回去)。 */
    if (mirror_base) {
        char merr[SXCL_INSTALL_ERROR_MAX];
        merr[0] = '\0';
        /* 补不上第二路不致命:官方那条路还在;失败信息不吞掉,写进 err 交给下面的人话文案 */
        (void)sxcl_version_plan_add_mirror(r->version_plan, mirror_base, merr, sizeof(merr));
        if (r->plan->prefer_mirror) {
            (void)sxcl_version_plan_prefer_mirror(r->version_plan);
        }
    }

    set_text(r->summary, sizeof(r->summary), "版本 JSON 已就绪: %s (%d 个下载条目)",
             r->version_json_path, (int)sxcl_version_plan_count(r->version_plan));
    return 0;
}

/** 按目标路径把计划里的任务分堆(客户端 jar / 资源索引 / 其余 = 依赖库)。 */
static int task_is_client_jar(const install_run *r, const sxcl_task *t) {
    return t && t->dest && strcmp(t->dest, r->client_jar_path) == 0;
}

static int task_is_asset_index(const install_run *r, const sxcl_task *t) {
    return t && t->dest && strcmp(t->dest, r->asset_index_path) == 0;
}

static int collect_batch(install_run *r, int want_client_jar, int want_asset_index, int want_rest) {
    const size_t total = sxcl_version_plan_count(r->version_plan);
    if (total > r->batch_cap) {
        sxcl_task **grown = (sxcl_task **)realloc(r->batch, total * sizeof(sxcl_task *));
        if (!grown) {
            return -1;
        }
        r->batch = grown;
        r->batch_cap = total;
    }
    size_t n = 0;
    for (size_t i = 0; i < total; ++i) {
        sxcl_task *t = sxcl_version_plan_task(r->version_plan, i);
        if (task_is_client_jar(r, t)) {
            if (want_client_jar) {
                r->batch[n++] = t;
            }
        } else if (task_is_asset_index(r, t)) {
            if (want_asset_index) {
                r->batch[n++] = t;
            }
        } else if (want_rest) {
            r->batch[n++] = t;
        }
    }
    return (int)n;
}

static int stage_client_jar(install_run *r) {
    const int n = collect_batch(r, 1, 0, 0);
    if (n < 0) {
        return fail(r, SXCL_INSTALL_ERR_NOMEM, "内存不足(收集客户端 jar 任务)");
    }
    if (n == 0) {
        return fail(r, SXCL_INSTALL_ERR_CLIENT_JAR, "版本 JSON 里没有客户端 jar 信息");
    }
    const int rc = download_batch(r, SXCL_INSTALL_STAGE_CLIENT_JAR, "客户端 jar", r->batch, (size_t)n, 1,
                                  SXCL_INSTALL_ERR_CLIENT_JAR, "客户端 jar");
    if (rc != 0) {
        return rc;
    }
    set_text(r->summary, sizeof(r->summary), "客户端 jar 已就绪: %s", path_leaf(r->client_jar_path));
    return 0;
}

static int stage_libraries(install_run *r) {
    const int n = collect_batch(r, 0, 0, 1);
    if (n < 0) {
        return fail(r, SXCL_INSTALL_ERR_NOMEM, "内存不足(收集依赖库任务)");
    }
    if (n == 0) {
        r->stage_percent = 100;
        set_text(r->summary, sizeof(r->summary), "依赖库: 这个版本没有依赖库");
        emit_progress(r, SXCL_INSTALL_EVENT_STAGE_PROGRESS, 100, r->summary, NULL);
        return 0;
    }
    return download_batch(r, SXCL_INSTALL_STAGE_LIBRARIES, "依赖库", r->batch, (size_t)n, 0,
                          SXCL_INSTALL_ERR_IO, "依赖库");
}

static int stage_asset_index(install_run *r) {
    const int n = collect_batch(r, 0, 1, 0);
    if (n < 0) {
        return fail(r, SXCL_INSTALL_ERR_NOMEM, "内存不足(收集资源索引任务)");
    }
    if (n == 0) {
        r->asset_index_path[0] = '\0';
        emit_progress(r, SXCL_INSTALL_EVENT_STAGE_PROGRESS, 100,
                      "这个版本没有资源索引,跳过资源下载", NULL);
        set_text(r->summary, sizeof(r->summary), "资源索引: 该版本没有");
        return 0;
    }
    const int rc = download_batch(r, SXCL_INSTALL_STAGE_ASSET_INDEX, "资源索引", r->batch, (size_t)n, 1,
                                  SXCL_INSTALL_ERR_IO, "资源索引");
    if (rc != 0) {
        return rc;
    }
    set_text(r->summary, sizeof(r->summary), "资源索引已就绪: %s", path_leaf(r->asset_index_path));
    return 0;
}

static int stage_asset_objects(install_run *r) {
    if (!r->asset_index_path[0]) {
        set_text(r->summary, sizeof(r->summary), "资源对象: 没有资源索引,跳过");
        return 0;
    }
    char err[SXCL_INSTALL_ERROR_MAX];
    err[0] = '\0';
    sxcl_json *index = sxcl_json_parse_file(r->asset_index_path, err, sizeof(err));
    if (!index) {
        return fail(r, SXCL_INSTALL_ERR_IO, "资源索引解析失败: %s", err[0] ? err : "解析失败");
    }
    const size_t before = sxcl_version_plan_count(r->version_plan);
    /* 资源对象是后追加的一批:候选顺序在这里一次定好。镜像优先 -> 参数位置对调
     * (base=镜像的 /assets,第二候选=官方 CDN),这样不必再调一次 prefer_mirror
     * (调第二次会把版本计划那批已经换好的又换回官方在前)。 */
    char asset_mirror_base[1024];
    install_asset_mirror_base(r->plan, asset_mirror_base, sizeof(asset_mirror_base));
    const char *asset_base = r->plan->asset_base_url;
    const char *asset_mirror = (asset_mirror_base[0] != '\0') ? asset_mirror_base : NULL;
    if (r->plan->prefer_mirror && asset_mirror) {
        const char *official = (r->plan->asset_base_url && *r->plan->asset_base_url)
                                   ? r->plan->asset_base_url
                                   : SXCL_ASSET_OBJECTS_BASE;
        asset_base = asset_mirror;
        asset_mirror = official;
    }
    const int added = sxcl_version_plan_add_asset_objects(r->version_plan, index, r->game_dir,
                                                         asset_base, asset_mirror, err, sizeof(err));
    sxcl_json_free(index);
    if (added < 0) {
        return fail(r, SXCL_INSTALL_ERR_IO, "展开资源对象失败: %s", err[0] ? err : "未知原因");
    }
    const size_t after = sxcl_version_plan_count(r->version_plan);
    if (after <= before) {
        r->stage_percent = 100;
        set_text(r->summary, sizeof(r->summary), "资源对象: 索引里没有需要下载的对象");
        emit_progress(r, SXCL_INSTALL_EVENT_STAGE_PROGRESS, 100, r->summary, NULL);
        return 0;
    }
    if (after > r->batch_cap) {
        sxcl_task **grown = (sxcl_task **)realloc(r->batch, after * sizeof(sxcl_task *));
        if (!grown) {
            return fail(r, SXCL_INSTALL_ERR_NOMEM, "内存不足(展开资源对象任务)");
        }
        r->batch = grown;
        r->batch_cap = after;
    }
    size_t n = 0;
    for (size_t i = before; i < after; ++i) {
        sxcl_task *t = sxcl_version_plan_task(r->version_plan, i);
        if (t) {
            r->batch[n++] = t;
        }
    }
    return download_batch(r, SXCL_INSTALL_STAGE_ASSET_OBJECTS, "资源对象", r->batch, n, 0,
                          SXCL_INSTALL_ERR_IO, "资源对象");
}

static int stage_loader_installer(install_run *r) {
    if (r->plan->installer_jar && *r->plan->installer_jar) {
        if (!sxcl_fs_exists(r->plan->installer_jar)) {
            return fail(r, SXCL_INSTALL_ERR_LOADER, "加载器安装器不在: %s", r->plan->installer_jar);
        }
        set_text(r->installer_path, sizeof(r->installer_path), "%s", r->plan->installer_jar);
        r->has_installer_path = 1;
        r->installer_downloaded = 0;
        emit_progress(r, SXCL_INSTALL_EVENT_STAGE_PROGRESS, 100, "加载器安装器已就绪(复用已有的)",
                      NULL);
        set_text(r->summary, sizeof(r->summary), "加载器安装器: 复用 %s", path_leaf(r->installer_path));
        return 0;
    }
    if (!r->plan->installer_url || !*r->plan->installer_url) {
        return fail(r, SXCL_INSTALL_ERR_LOADER,
                    "没有加载器安装器地址(plan.installer_url / installer_jar 都为空)");
    }

    sxcl_task task;
    memset(&task, 0, sizeof(task));
    task.dest = r->installer_path;
    /* 安装器也有第二路(能映射的镜像站才认;认不出 URL 就还是官方一条路,不造假 URL) */
    char installer_mirror[1024];
    installer_mirror[0] = '\0';
    const char *installer_base = install_mirror_base(r->plan);
    const char *installer_mirror_url = NULL;
    if (installer_base && sxcl_manifest_mirror_url(r->plan->installer_url, installer_base,
                                                   installer_mirror, sizeof(installer_mirror)) == 0) {
        installer_mirror_url = installer_mirror;
    }
    if (r->plan->prefer_mirror && installer_mirror_url) {
        task.urls[0] = installer_mirror_url;
        task.urls[1] = r->plan->installer_url;
    } else {
        task.urls[0] = r->plan->installer_url;
        task.urls[1] = installer_mirror_url;
    }
    task.algo = SXCL_HASH_SHA1;
    task.size = 0; /* 安装器没有官方哈希:只校验"下下来了且非空" */
    task.priority = 0;
    task.label = "加载器安装器";

    sxcl_task *batch[1];
    batch[0] = &task;
    r->cur_tasks = batch;
    r->cur_count = 1;
    r->cur_label = "加载器安装器";
    const int rc = download_batch(r, SXCL_INSTALL_STAGE_LOADER_INSTALLER, "加载器安装器", batch, 1, 1,
                                  SXCL_INSTALL_ERR_LOADER, "加载器安装器");
    if (rc != 0) {
        return rc;
    }
    int64_t size = 0;
    if (sxcl_fs_stat(r->installer_path, &size, NULL) != 0 || size <= 0) {
        return fail(r, SXCL_INSTALL_ERR_LOADER, "加载器安装器下载后为空: %s", r->installer_path);
    }
    r->installer_downloaded = 1;
    r->has_installer_path = 1;
    set_text(r->summary, sizeof(r->summary), "加载器安装器已就绪: %s", path_leaf(r->installer_path));
    return 0;
}

/* 加载器安装器的进度(百分之 + 它自己打印的标记文本) */
static void on_loader_progress(void *userdata, int percent, const char *status) {
    install_run *r = (install_run *)userdata;
    char text[SXCL_INSTALL_STATUS_MAX];
    if (status && *status) {
        set_text(text, sizeof(text), "加载器安装: %s", status);
    } else {
        set_text(text, sizeof(text), "加载器安装进行中…");
    }
    emit_progress(r, SXCL_INSTALL_EVENT_STAGE_PROGRESS, percent, text, NULL);
}

/**
 * 方式 B(解包安装)会把"要下载的依赖库"交回来:在这里走一遍下载钩子。
 * 任何一个库失败都算加载器安装失败(与 Python 的 _stage_download_loader_libs 一致)。
 */
static int on_loader_libraries(void *userdata, const sxcl_loader_library *libs, size_t count) {
    install_run *r = (install_run *)userdata;
    if (count == 0) {
        return 0;
    }
    /* 竞技场要装两类串:目标路径(<游戏目录>/libraries/<path>,比 path 长不少)与完整 URL。
     * 这里按"每项实际长度 + 余量"算,别用 path 的长度近似 —— 少算一个字节就是堆越界。 */
    const size_t game_len = strlen(r->game_dir);
    size_t arena_size = 0;
    for (size_t i = 0; i < count; ++i) {
        arena_size += game_len + strlen(libs[i].path) + 32; /* "/libraries/" + NUL + 余量 */
        arena_size += strlen(libs[i].url) + strlen(libs[i].path) + 8;
    }
    char *arena = (char *)malloc(arena_size ? arena_size : 1);
    sxcl_task *tasks = (sxcl_task *)calloc(count, sizeof(sxcl_task));
    sxcl_task **ptrs = (sxcl_task **)malloc(count * sizeof(sxcl_task *));
    if (!arena || !tasks || !ptrs) {
        free(arena);
        free(tasks);
        free(ptrs);
        fail(r, SXCL_INSTALL_ERR_NOMEM, "内存不足(加载器依赖库任务)");
        return 1;
    }
    size_t off = 0;
    int bad_path = 0;
    char dest[SXCL_INSTALL_PATH_MAX];
    char rel[SXCL_INSTALL_PATH_MAX];
    for (size_t i = 0; i < count; ++i) {
        set_text(rel, sizeof(rel), "libraries/%s", libs[i].path);
        if (path_join(dest, sizeof(dest), r->game_dir, rel) != 0) {
            bad_path = 1;
            break;
        }
        const size_t dn = strlen(dest) + 1;
        const size_t ul = strlen(libs[i].url);
        const char *sep = (ul > 0 && libs[i].url[ul - 1] == '/') ? "" : "/";
        const size_t need = dn + ul + strlen(sep) + strlen(libs[i].path) + 1;
        if (off + need > arena_size) { /* 防御:算错了也绝不越界写 */
            bad_path = 1;
            break;
        }
        memcpy(arena + off, dest, dn);
        tasks[i].dest = arena + off;
        off += dn;

        set_text(arena + off, arena_size - off, "%s%s%s", libs[i].url, sep, libs[i].path);
        tasks[i].urls[0] = arena + off;
        off += strlen(arena + off) + 1;

        tasks[i].algo = SXCL_HASH_SHA1;
        tasks[i].size = 0; /* 加载器依赖库没有官方哈希:只做大小/可读检查(与 Python 一致) */
        tasks[i].priority = 10;
        tasks[i].label = libs[i].name;
        ptrs[i] = &tasks[i];
    }
    int rc = 0;
    if (bad_path) {
        rc = fail(r, SXCL_INSTALL_ERR_LOADER, "加载器依赖库路径太长: %s", rel);
    } else {
        rc = download_batch(r, SXCL_INSTALL_STAGE_LOADER_RUN, "加载器依赖库", ptrs, count, 1,
                            SXCL_INSTALL_ERR_LOADER, "加载器依赖库");
    }
    free(arena);
    free(tasks);
    free(ptrs);
    return rc == 0 ? 0 : 1;
}

/** 加载器安装器内部的取消检查(转发给编排层的 is_cancelled)。 */
static int loader_is_cancelled(void *userdata) {
    return check_cancel((install_run *)userdata);
}

static int stage_loader_run(install_run *r) {
    sxcl_loader_install_request request;
    memset(&request, 0, sizeof(request));
    request.game_dir = r->game_dir;
    request.instance_name = r->instance;
    request.base_version = r->plan->version_id;
    request.kind = r->plan->loader;
    request.loader_version = r->plan->loader_version;
    request.installer_jar = r->installer_path;
    request.java_path = r->plan->java_path;
    request.mirror_maven = r->plan->loader_mirror_maven;
    request.timeout_ms = r->plan->loader_timeout_ms;
    request.on_progress = on_loader_progress;
    request.userdata = r;
    request.is_cancelled = loader_is_cancelled;
    request.cancel_userdata = r;
    request.on_libraries = on_loader_libraries;

    sxcl_loader_install_result result;
    memset(&result, 0, sizeof(result));
    const int rc = r->io->loader_install(r->io->userdata, &request, &result);

    if (check_cancel(r) || result.fail_stage == SXCL_LOADER_FAIL_CANCELLED) {
        return SXCL_INSTALL_ERR_CANCELLED;
    }
    if (rc != 0 || !result.ok) {
        return fail(r, SXCL_INSTALL_ERR_LOADER, "加载器安装失败(%s): %s",
                    sxcl_loader_fail_stage_name(result.fail_stage),
                    result.error[0] ? result.error : "安装器没有给出原因(详见日志)");
    }
    set_text(r->summary, sizeof(r->summary), "加载器安装完成: %s", r->instance);
    emit_progress(r, SXCL_INSTALL_EVENT_STAGE_PROGRESS, 100, r->summary, NULL);
    return 0;
}

static int stage_natives(install_run *r) {
    char err[SXCL_INSTALL_ERROR_MAX];
    err[0] = '\0';
    int count = -1;
    const int rc = r->io->natives_prepare(r->io->userdata, r->version_doc, r->game_dir, r->natives_dir,
                                          &count, err, sizeof(err));
    if (rc != 0) {
        return fail(r, SXCL_INSTALL_ERR_NATIVES, "解压 natives 失败: %s", err[0] ? err : "未知原因");
    }
    r->out->natives_files = count;
    if (count >= 0) {
        set_text(r->summary, sizeof(r->summary), "natives: %d 个文件已就绪", count);
    } else {
        set_text(r->summary, sizeof(r->summary), "natives 已就绪: %s", r->natives_dir);
    }
    emit_progress(r, SXCL_INSTALL_EVENT_STAGE_PROGRESS, 100, r->summary, NULL);
    return 0;
}

/** 删掉计划内所有任务的未完成产物(<dest>.part 与 <dest>.part.json)。 */
static void remove_part_files(install_run *r) {
    const size_t total = sxcl_version_plan_count(r->version_plan);
    char path[SXCL_INSTALL_PATH_MAX + 16];
    for (size_t i = 0; i < total; ++i) {
        const sxcl_task *t = sxcl_version_plan_task(r->version_plan, i);
        if (!t || !t->dest) {
            continue;
        }
        set_text(path, sizeof(path), "%s.part", t->dest);
        (void)sxcl_fs_remove(path);
        set_text(path, sizeof(path), "%s.part.json", t->dest);
        (void)sxcl_fs_remove(path);
    }
    if (r->has_installer_path) {
        set_text(path, sizeof(path), "%s.part", r->installer_path);
        (void)sxcl_fs_remove(path);
        set_text(path, sizeof(path), "%s.part.json", r->installer_path);
        (void)sxcl_fs_remove(path);
    }
}

static int stage_finish(install_run *r) {
    emit_progress(r, SXCL_INSTALL_EVENT_CLEANUP, r->stage_percent, "清理临时文件…", NULL);
    remove_part_files(r);
    if (r->installer_downloaded && !r->plan->keep_installer && r->has_installer_path) {
        (void)sxcl_fs_remove(r->installer_path);
        set_text(r->summary, sizeof(r->summary), "临时文件与加载器安装器已清理");
    } else {
        set_text(r->summary, sizeof(r->summary), "临时文件已清理");
    }
    emit_progress(r, SXCL_INSTALL_EVENT_STAGE_PROGRESS, 100, r->summary, NULL);
    return 0;
}

static int run_one_stage(install_run *r) {
    switch (r->stage) {
    case SXCL_INSTALL_STAGE_MANIFEST:
        return stage_manifest(r);
    case SXCL_INSTALL_STAGE_VERSION_JSON:
        return stage_version_json(r);
    case SXCL_INSTALL_STAGE_CLIENT_JAR:
        return stage_client_jar(r);
    case SXCL_INSTALL_STAGE_LIBRARIES:
        return stage_libraries(r);
    case SXCL_INSTALL_STAGE_ASSET_INDEX:
        return stage_asset_index(r);
    case SXCL_INSTALL_STAGE_ASSET_OBJECTS:
        return stage_asset_objects(r);
    case SXCL_INSTALL_STAGE_LOADER_INSTALLER:
        return stage_loader_installer(r);
    case SXCL_INSTALL_STAGE_LOADER_RUN:
        return stage_loader_run(r);
    case SXCL_INSTALL_STAGE_NATIVES:
        return stage_natives(r);
    case SXCL_INSTALL_STAGE_FINISH:
        return stage_finish(r);
    default:
        return fail(r, SXCL_INSTALL_ERR_ARG, "未知阶段: %d", (int)r->stage);
    }
}

/* ── 默认实现(真表) ── */

/** 引擎进度 → 编排层回报(返回非 0 = 取消,顺手把引擎叫停)。 */
typedef struct engine_bridge {
    const sxcl_install_download *request;
    sxcl_engine *engine;
    size_t settled;
    int stop;
} engine_bridge;

static void engine_on_progress(void *userdata, const sxcl_task *task) {
    engine_bridge *b = (engine_bridge *)userdata;
    if (b->stop) {
        return;
    }
    size_t index = SIZE_MAX;
    for (size_t i = 0; i < b->request->count; ++i) {
        if (b->request->tasks[i] == task) {
            index = i;
            break;
        }
    }
    if (task && task->state != SXCL_TASK_PENDING && task->state != SXCL_TASK_RUNNING) {
        ++b->settled; /* 一个任务只会落定一次(引擎在 run_task 之后回调一次) */
    }
    if (b->request->report) {
        const int rc = b->request->report(b->request->report_userdata, task, index, b->settled,
                                          b->request->count);
        if (rc != 0) {
            b->stop = 1;
            sxcl_engine_cancel(b->engine);
        }
    }
}

static int default_download(void *userdata, const sxcl_install_download *request,
                            sxcl_install_download_stats *stats, char *err, size_t err_len) {
    if (err && err_len) {
        err[0] = '\0';
    }
    const sxcl_engine_opts *opts = request->engine_opts ? request->engine_opts
                                                        : (const sxcl_engine_opts *)userdata;
    if (!opts || !opts->transport_factory) {
        set_text(err, err_len, "没有配置传输后端(engine_opts.transport_factory 为空)");
        return -1;
    }
    if (!request->tasks || request->count == 0) {
        return 0;
    }

    engine_bridge bridge;
    memset(&bridge, 0, sizeof(bridge));
    bridge.request = request;

    sxcl_engine_opts local = *opts;
    local.on_progress = engine_on_progress;
    local.userdata = &bridge;

    sxcl_engine *engine = sxcl_engine_create(&local);
    if (!engine) {
        set_text(err, err_len, "创建下载引擎失败");
        return -1;
    }
    bridge.engine = engine;
    for (size_t i = 0; i < request->count; ++i) {
        if (request->tasks[i] && sxcl_engine_submit(engine, request->tasks[i]) != 0) {
            sxcl_engine_destroy(engine);
            set_text(err, err_len, "任务入队失败(第 %d 个)", (int)i);
            return -1;
        }
    }
    const int failed = sxcl_engine_run(engine);
    const int stopped = bridge.stop;
    sxcl_engine_destroy(engine); /* 任务由调用方持有,引擎必须在钩子返回前放掉 */

    if (stopped && request->is_cancelled && request->is_cancelled(request->cancel_userdata)) {
        set_text(err, err_len, "已取消");
        /* 取消不算"失败文件数",让编排层按 is_cancelled 判 */
        return request->count > 0 ? 1 : 0;
    }
    if (failed < 0) {
        set_text(err, err_len, "下载引擎参数错误");
        return -1;
    }

    if (stats) {
        stats->files_total = request->count;
        stats->bytes_total = 0;
        stats->bytes_done = 0;
        for (size_t i = 0; i < request->count; ++i) {
            const sxcl_task *t = request->tasks[i];
            if (!t) {
                continue;
            }
            if (t->size > 0) {
                stats->bytes_total += t->size;
            }
            if (t->bytes_done > 0) {
                stats->bytes_done += t->bytes_done;
            }
            if (t->state == SXCL_TASK_DONE) {
                ++stats->files_done;
                if (t->error[0] && strcmp(t->error, "已存在且校验通过") == 0) {
                    ++stats->files_skipped;
                }
            } else if (t->state == SXCL_TASK_FAILED || t->state == SXCL_TASK_CANCELLED) {
                ++stats->files_failed;
            }
        }
    }
    return failed;
}

static int default_fetch_text(void *userdata, const char *url, char **out_text, char *err,
                              size_t err_len) {
    return sxcl_install_http_get_text(userdata, url, out_text, err, err_len);
}

static int default_loader_install(void *userdata, const sxcl_loader_install_request *request,
                                  sxcl_loader_install_result *out) {
    (void)userdata;
    return sxcl_loader_install(request, out);
}

static int default_natives_prepare(void *userdata, const sxcl_json *version_json, const char *game_dir,
                                   const char *natives_dir, int *out_count, char *err, size_t err_len) {
    (void)userdata;
    const int rc = sxcl_natives_prepare_json(version_json, game_dir, natives_dir, err, err_len);
    if (out_count) {
        *out_count = rc == 0 ? sxcl_natives_last_count() : -1;
    }
    return rc;
}

static const sxcl_install_io kDefaultIo = {
    default_fetch_text,
    default_download,
    default_loader_install,
    default_natives_prepare,
    NULL, /* userdata:调用方可以塞 const sxcl_engine_opts *(取文本要用) */
};

const sxcl_install_io *sxcl_install_default_io(void) {
    return &kDefaultIo;
}

/* ── 取文本(默认实现) ── */

#define SXCL_INSTALL_TEXT_MAX ((int64_t)8 * 1024 * 1024) /* 清单也就几 MB,超了说明地址不对 */

int sxcl_install_http_get_text(void *userdata, const char *url, char **out_text, char *err,
                               size_t err_len) {
    if (err && err_len) {
        err[0] = '\0';
    }
    if (out_text) {
        *out_text = NULL;
    }
    if (!url || !*url || !out_text) {
        set_text(err, err_len, "取文本:参数不完整");
        return SXCL_NET_ERR_BAD_ARG;
    }
    const sxcl_engine_opts *opts = (const sxcl_engine_opts *)userdata;
    if (!opts || !opts->transport_factory) {
        set_text(err, err_len, "取文本需要传输后端(engine_opts.transport_factory 为空)");
        return SXCL_NET_ERR_UNSUPPORTED;
    }
    sxcl_transport *transport = opts->transport_factory(opts->userdata);
    if (!transport || !transport->request || !transport->read || !transport->close_body) {
        if (transport && transport->destroy) {
            transport->destroy(transport->ctx);
        }
        set_text(err, err_len, "创建传输后端失败");
        return SXCL_NET_ERR_UNSUPPORTED;
    }

    sxcl_http_request request;
    memset(&request, 0, sizeof(request));
    request.url = url;
    request.method = "GET";
    request.range_start = -1;
    request.range_end = -1;
    request.timeout_ms = 30000;
    request.extra_headers = NULL;

    sxcl_http_response response;
    memset(&response, 0, sizeof(response));
    sxcl_http_body *body = NULL;
    const int rc = transport->request(transport->ctx, &request, &response, &body);
    if (rc != SXCL_NET_OK) {
        set_text(err, err_len, "请求失败(网络/连接): %s", url);
        transport->destroy(transport->ctx);
        return rc;
    }
    if (response.status < 200 || response.status >= 300) {
        set_text(err, err_len, "服务器返回 HTTP %d: %s", response.status, url);
        if (body) {
            transport->close_body(transport->ctx, body);
        }
        transport->destroy(transport->ctx);
        return SXCL_NET_ERR_IO;
    }

    size_t cap = 64 * 1024;
    size_t used = 0;
    char *text = (char *)malloc(cap);
    if (!text) {
        if (body) {
            transport->close_body(transport->ctx, body);
        }
        transport->destroy(transport->ctx);
        set_text(err, err_len, "内存不足(取文本)");
        return SXCL_NET_ERR_IO;
    }
    for (;;) {
        if (used + 4096 + 1 > cap) {
            if ((int64_t)cap >= SXCL_INSTALL_TEXT_MAX) {
                free(text);
                transport->close_body(transport->ctx, body);
                transport->destroy(transport->ctx);
                set_text(err, err_len, "响应太大(超过 %d 字节): %s", (int)SXCL_INSTALL_TEXT_MAX, url);
                return SXCL_NET_ERR_IO;
            }
            const size_t next = cap * 2;
            char *grown = (char *)realloc(text, next);
            if (!grown) {
                free(text);
                transport->close_body(transport->ctx, body);
                transport->destroy(transport->ctx);
                set_text(err, err_len, "内存不足(取文本扩容)");
                return SXCL_NET_ERR_IO;
            }
            text = grown;
            cap = next;
        }
        const int64_t got = transport->read(transport->ctx, body, text + used, cap - used - 1);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            free(text);
            transport->close_body(transport->ctx, body);
            transport->destroy(transport->ctx);
            set_text(err, err_len, "读取响应失败: %s", url);
            return SXCL_NET_ERR_IO;
        }
        used += (size_t)got;
    }
    transport->close_body(transport->ctx, body);
    transport->destroy(transport->ctx);
    text[used] = '\0';
    *out_text = text;
    return 0;
}

/* ── 入口 ── */

static void install_release(install_run *r) {
    free(r->batch);
    r->batch = NULL;
    if (r->version_plan) {
        sxcl_version_plan_free(r->version_plan);
        r->version_plan = NULL;
    }
    if (r->version_doc) {
        sxcl_json_free(r->version_doc);
        r->version_doc = NULL;
    }
    if (r->versions) {
        sxcl_version_list_free(r->versions);
        r->versions = NULL;
    }
    if (r->manifest_doc) {
        sxcl_json_free(r->manifest_doc);
        r->manifest_doc = NULL;
    }
}

static void install_cleanup(install_run *r, const char *why) {
    emit_progress(r, SXCL_INSTALL_EVENT_CLEANUP, r->stage_percent, why, NULL);
    remove_part_files(r);
}

int sxcl_install_run(const sxcl_install_request *req, sxcl_install_result *out) {
    if (!out) {
        return SXCL_INSTALL_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->fail_stage = SXCL_INSTALL_STAGE_END;
    out->fail_stage_id = sxcl_install_stage_id(SXCL_INSTALL_STAGE_END);
    out->natives_files = -1;

    if (!req || !req->plan) {
        out->code = SXCL_INSTALL_ERR_ARG;
        set_text(out->error, sizeof(out->error), "没有安装计划");
        out->retryable = sxcl_install_code_retryable(out->code);
        return out->code;
    }
    const sxcl_install_plan *plan = req->plan;
    if (!plan->game_dir || !*plan->game_dir) {
        out->code = SXCL_INSTALL_ERR_ARG;
        set_text(out->error, sizeof(out->error), "没有游戏目录");
        out->retryable = sxcl_install_code_retryable(out->code);
        return out->code;
    }
    if (!plan->version_id || !*plan->version_id) {
        out->code = SXCL_INSTALL_ERR_ARG;
        set_text(out->error, sizeof(out->error), "没有要装的版本号");
        out->retryable = sxcl_install_code_retryable(out->code);
        return out->code;
    }
    const sxcl_install_io *io = req->io ? req->io : sxcl_install_default_io();
    if (!io->fetch_text || !io->download || !io->natives_prepare) {
        out->code = SXCL_INSTALL_ERR_ARG;
        set_text(out->error, sizeof(out->error), "依赖注入表不完整(取文本/下载/natives 都要有)");
        out->retryable = sxcl_install_code_retryable(out->code);
        return out->code;
    }

    install_run run;
    memset(&run, 0, sizeof(run));
    run.req = req;
    run.plan = plan;
    run.io = io;
    run.out = out;
    run.game_dir = plan->game_dir;
    run.instance = sxcl_install_plan_instance(plan);
    run.stage = SXCL_INSTALL_STAGE_MANIFEST;

    const int has_loader = sxcl_install_plan_has_loader(plan);
    if (has_loader) {
        if (!io->loader_install) {
            out->code = SXCL_INSTALL_ERR_ARG;
            set_text(out->error, sizeof(out->error), "含加载器的计划缺少加载器安装实现");
            out->retryable = sxcl_install_code_retryable(out->code);
            return out->code;
        }
        if (!plan->java_path || !*plan->java_path) {
            out->code = SXCL_INSTALL_ERR_ARG;
            set_text(out->error, sizeof(out->error), "含加载器的计划缺少 Java 路径(安装器要它)");
            out->retryable = sxcl_install_code_retryable(out->code);
            return out->code;
        }
        if (!sxcl_loader_kind_implemented(plan->loader)) {
            out->code = SXCL_INSTALL_ERR_ARG;
            set_text(out->error, sizeof(out->error), "还没有 %s 的安装实现",
                     sxcl_loader_kind_name(plan->loader));
            out->retryable = sxcl_install_code_retryable(out->code);
            return out->code;
        }
    }

    char rel[SXCL_INSTALL_PATH_MAX];
    set_text(rel, sizeof(rel), "versions/%s/%s.json", run.instance, run.instance);
    if (path_join(run.version_json_path, sizeof(run.version_json_path), run.game_dir, rel) != 0) {
        out->code = SXCL_INSTALL_ERR_ARG;
        set_text(out->error, sizeof(out->error), "版本 JSON 路径太长: %s", rel);
        out->retryable = 0;
        return out->code;
    }
    set_text(out->version_json_path, sizeof(out->version_json_path), "%s", run.version_json_path);
    set_text(rel, sizeof(rel), "versions/%s/%s.jar", run.instance, run.instance);
    if (path_join(run.client_jar_path, sizeof(run.client_jar_path), run.game_dir, rel) != 0) {
        out->code = SXCL_INSTALL_ERR_ARG;
        set_text(out->error, sizeof(out->error), "客户端 jar 路径太长: %s", rel);
        out->retryable = 0;
        return out->code;
    }
    set_text(rel, sizeof(rel), "versions/%s/%s-natives", run.instance, run.instance);
    if (path_join(run.natives_dir, sizeof(run.natives_dir), run.game_dir, rel) != 0) {
        out->code = SXCL_INSTALL_ERR_ARG;
        set_text(out->error, sizeof(out->error), "natives 路径太长: %s", rel);
        out->retryable = 0;
        return out->code;
    }
    set_text(out->natives_dir, sizeof(out->natives_dir), "%s", run.natives_dir);
    if (has_loader) {
        set_text(rel, sizeof(rel), "versions/%s/%s-installer.jar", run.instance,
                 sxcl_loader_kind_id(plan->loader));
        if (path_join(run.installer_path, sizeof(run.installer_path), run.game_dir, rel) != 0) {
            out->code = SXCL_INSTALL_ERR_ARG;
            set_text(out->error, sizeof(out->error), "加载器安装器路径太长: %s", rel);
            out->retryable = 0;
            return out->code;
        }
    }

    build_stage_table(plan, run.stages, &run.stage_count);

    int code = SXCL_INSTALL_OK;
    for (run.stage_index = 0; run.stage_index < run.stage_count; ++run.stage_index) {
        run.stage = run.stages[run.stage_index];
        run.stage_percent = 0;
        run.cur_tasks = NULL;
        run.cur_count = 0;
        run.cur_label = NULL;
        run.summary[0] = '\0';
        run.ev_files_total = 0;
        run.ev_files_done = 0;
        run.ev_files_skipped = 0;
        run.ev_files_failed = 0;
        run.ev_bytes_done = 0;
        run.ev_bytes_total = 0;

        if (check_cancel(&run)) {
            code = SXCL_INSTALL_ERR_CANCELLED;
            break;
        }
        char status[SXCL_INSTALL_STATUS_MAX];
        set_text(status, sizeof(status), "%s…", sxcl_install_stage_name(run.stage));
        emit_progress(&run, SXCL_INSTALL_EVENT_STAGE_BEGIN, 0, status, NULL);

        code = run_one_stage(&run);
        if (code != 0) {
            break;
        }
        ++out->stages_done;
        if (!run.summary[0]) {
            set_text(run.summary, sizeof(run.summary), "%s完成", sxcl_install_stage_name(run.stage));
        }
        emit_progress(&run, SXCL_INSTALL_EVENT_STAGE_END, 100, run.summary, NULL);
    }

    out->bytes_done = run.bytes_done_total;
    out->files_skipped = run.files_skipped_total;
    out->files_failed = run.files_failed_total;

    if (code == SXCL_INSTALL_OK) {
        out->code = SXCL_INSTALL_OK;
        out->percent = 100;
        out->fail_stage = SXCL_INSTALL_STAGE_END;
        out->fail_stage_index = run.stage_count;
        out->fail_stage_id = sxcl_install_stage_id(SXCL_INSTALL_STAGE_END);
        char status[SXCL_INSTALL_STATUS_MAX];
        set_text(status, sizeof(status), "安装完成: %s", run.instance);
        emit_progress(&run, SXCL_INSTALL_EVENT_DONE, 100, status, NULL);
        install_release(&run);
        return SXCL_INSTALL_OK;
    }

    out->code = code;
    out->cancelled = (code == SXCL_INSTALL_ERR_CANCELLED);
    out->retryable = out->cancelled ? 0 : sxcl_install_code_retryable(code);
    out->fail_stage = run.stage;
    out->fail_stage_index = run.stage_index;
    out->fail_stage_id = sxcl_install_stage_id(run.stage);
    if (out->cancelled && !out->error[0]) {
        set_text(out->error, sizeof(out->error), "已取消安装");
    }
    char status[SXCL_INSTALL_STATUS_MAX];
    set_text(status, sizeof(status), "%s%s",
             out->cancelled ? "已取消安装: " : "安装失败: ",
             out->error[0] ? out->error : sxcl_install_stage_name(run.stage));
    install_cleanup(&run, out->cancelled ? "清理未完成的下载文件…" : "安装失败,清理未完成的下载文件…");
    out->percent = run.last_percent;
    emit_progress(&run, SXCL_INSTALL_EVENT_DONE, run.stage_percent, status, NULL);
    install_release(&run);
    return out->code;
}
