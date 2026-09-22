/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include "sxcl/loader.h"

#include "sxcl/fs.h"
#include "sxcl/process.h"
#include "sxcl/processor.h"
#include "sxcl/zip.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <direct.h>   /* _getcwd */
#  include <windows.h>
#else
#  include <dirent.h>
#  include <unistd.h>
#endif

/* 加载器自己的 maven(坐标没写 url 时用;与 Python 版 ForgeAnalyzer/NeoForgeAnalyzer 一致)。 */
#define MAVEN_FORGE     "https://maven.minecraftforge.net/"
#define MAVEN_NEOFORGE  "https://maven.neoforged.net/releases/"
#define MAVEN_FABRIC    "https://maven.fabricmc.net/"

#define FALLBACK_PERCENT 40
#define POST_PERCENT     85

/* ── UTF-8 路径与文件小工具(与 src/core/settings.c 同一套做法) ── */

#if defined(_WIN32)
static wchar_t *utf8_to_wide(const char *utf8)
{
    const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (need <= 0) {
        return NULL;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wide) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, need) != need) {
        free(wide);
        return NULL;
    }
    return wide;
}

static char *wide_to_utf8(const wchar_t *wide)
{
    const int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (need <= 0) {
        return NULL;
    }
    char *utf8 = (char *)malloc((size_t)need);
    if (!utf8) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, need, NULL, NULL) != need) {
        free(utf8);
        return NULL;
    }
    return utf8;
}
#endif

static FILE *open_utf8(const char *path, int for_write)
{
#if defined(_WIN32)
    wchar_t *wide = utf8_to_wide(path);
    if (!wide) {
        return NULL;
    }
    FILE *fh = NULL;
    if (_wfopen_s(&fh, wide, for_write ? L"wb" : L"rb") != 0) {
        fh = NULL;
    }
    free(wide);
    return fh;
#else
    return fopen(path, for_write ? "wb" : "rb");
#endif
}

/* 拼路径(统一用 '/'。Windows 的 API 一样认)。a 末尾与 b 开头的分隔符会被吃掉。 */
static int join_path(char *out, size_t cap, const char *a, const char *b)
{
    if (!out || cap == 0 || !a || !b) {
        return -1;
    }
    size_t la = strlen(a);
    while (la > 1 && (a[la - 1] == '/' || a[la - 1] == '\\')) {
        --la;
    }
    size_t skip = 0;
    while (b[skip] == '/' || b[skip] == '\\') {
        ++skip;
    }
    const int written = snprintf(out, cap, "%.*s/%s", (int)la, a, b + skip);
    if (written <= 0 || (size_t)written >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

static int write_text_atomic(const char *path, const char *text)
{
    char tmp[SXCL_LOADER_CMD_ARG_MAX + 8];
    const int written = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (written <= 0 || (size_t)written >= sizeof(tmp)) {
        return -1;
    }
    if (sxcl_fs_mkdirs_for_file(path) != 0) {
        return -1;
    }
    FILE *fh = open_utf8(tmp, 1);
    if (!fh) {
        return -1;
    }
    const size_t len = strlen(text);
    const int ok = (fwrite(text, 1, len, fh) == len);
    if (fclose(fh) != 0 || !ok) {
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    if (sxcl_fs_rename_replace(tmp, path) != 0) {
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    return 0;
}

static int copy_file(const char *src, const char *dst, int skip_existing)
{
    if (skip_existing && sxcl_fs_exists(dst)) {
        return 1;   /* 已经在了,不覆盖 */
    }
    FILE *in = open_utf8(src, 0);
    if (!in) {
        return -1;
    }
    if (sxcl_fs_mkdirs_for_file(dst) != 0) {
        (void)fclose(in);
        return -1;
    }
    char tmp[SXCL_LOADER_CMD_ARG_MAX + 8];
    const int written = snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    if (written <= 0 || (size_t)written >= sizeof(tmp)) {
        (void)fclose(in);
        return -1;
    }
    FILE *out = open_utf8(tmp, 1);
    if (!out) {
        (void)fclose(in);
        return -1;
    }
    char buffer[64 * 1024];
    int ok = 0;
    for (;;) {
        const size_t got = fread(buffer, 1, sizeof(buffer), in);
        if (got > 0 && fwrite(buffer, 1, got, out) != got) {
            break;
        }
        if (got < sizeof(buffer)) {
            ok = (ferror(in) == 0);
            break;
        }
    }
    (void)fclose(in);
    if (fclose(out) != 0) {
        ok = 0;
    }
    if (!ok || sxcl_fs_rename_replace(tmp, dst) != 0) {
        (void)sxcl_fs_remove(tmp);
        return -1;
    }
    return 0;
}

/* ── 目录遍历 ── */

/** 回调返回非 0 = 停止遍历。name 是条目名,full 是完整路径。 */
typedef int (*dir_visit_fn)(void *ud, const char *full, const char *name, int is_dir);

static int list_dir(const char *dir, dir_visit_fn visit, void *ud)
{
    char full[SXCL_LOADER_CMD_ARG_MAX];
#if defined(_WIN32)
    wchar_t *wdir = utf8_to_wide(dir);
    if (!wdir) {
        return -1;
    }
    const size_t len = wcslen(wdir);
    wchar_t *pattern = (wchar_t *)malloc((len + 3) * sizeof(wchar_t));
    if (!pattern) {
        free(wdir);
        return -1;
    }
    wcscpy(pattern, wdir);
    if (len > 0 && wdir[len - 1] != L'\\' && wdir[len - 1] != L'/') {
        wcscat(pattern, L"\\");
    }
    wcscat(pattern, L"*");
    free(wdir);

    WIN32_FIND_DATAW entry;
    HANDLE handle = FindFirstFileW(pattern, &entry);
    free(pattern);
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    int stopped = 0;
    do {
        if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) {
            continue;
        }
        char *name = wide_to_utf8(entry.cFileName);
        if (!name) {
            continue;
        }
        const int is_dir = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (join_path(full, sizeof(full), dir, name) == 0) {
            if (visit(ud, full, name, is_dir) != 0) {
                stopped = 1;
            }
        }
        free(name);
    } while (!stopped && FindNextFileW(handle, &entry));
    (void)FindClose(handle);
    return 0;
#else
    DIR *handle = opendir(dir);
    if (!handle) {
        return -1;
    }
    int stopped = 0;
    for (struct dirent *entry = readdir(handle); entry && !stopped; entry = readdir(handle)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (join_path(full, sizeof(full), dir, entry->d_name) != 0) {
            continue;
        }
        const int is_dir = sxcl_fs_is_dir(full);
        if (visit(ud, full, entry->d_name, is_dir) != 0) {
            stopped = 1;
        }
    }
    (void)closedir(handle);
    return 0;
#endif
}

static int has_suffix_ci(const char *name, const char *suffix)
{
    const size_t n = strlen(name);
    const size_t s = strlen(suffix);
    if (n < s) {
        return 0;
    }
    for (size_t i = 0; i < s; ++i) {
        char a = name[n - s + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

typedef struct dir_probe {
    int json_files;
    int any_files;
} dir_probe;

static int probe_dir_cb(void *ud, const char *full, const char *name, int is_dir)
{
    (void)full;
    dir_probe *probe = (dir_probe *)ud;
    if (!is_dir) {
        ++probe->any_files;
        if (has_suffix_ci(name, ".json")) {
            ++probe->json_files;
        }
    }
    return 0;
}

/* Python: _version_ready —— 版本目录里必须有 JSON(jar 可以继承原版,不强制)。 */
static int dir_has_json(const char *dir)
{
    if (!sxcl_fs_is_dir(dir)) {
        return 0;
    }
    dir_probe probe;
    probe.json_files = 0;
    probe.any_files = 0;
    if (list_dir(dir, probe_dir_cb, &probe) != 0) {
        return 0;
    }
    return probe.json_files > 0;
}

/* 非递归:把 src 目录里的文件复制到 dst(目录跳过)。返回复制的文件数,<0 失败。 */
static int copy_files_cb(void *ud, const char *full, const char *name, int is_dir)
{
    (void)name;
    if (is_dir) {
        return 0;
    }
    const char *dst_dir = (const char *)ud;
    char target[SXCL_LOADER_CMD_ARG_MAX];
    if (join_path(target, sizeof(target), dst_dir, name) != 0) {
        return 0;
    }
    (void)copy_file(full, target, 0);
    return 0;
}

static int copy_dir_files(const char *src, const char *dst)
{
    if (!sxcl_fs_is_dir(src)) {
        return -1;
    }
    if (sxcl_fs_mkdirs(dst) != 0) {
        return -1;
    }
    dir_probe probe;
    probe.json_files = 0;
    probe.any_files = 0;
    if (list_dir(src, probe_dir_cb, &probe) != 0) {
        return -1;
    }
    if (probe.any_files == 0) {
        return 0;
    }
    return list_dir(src, copy_files_cb, (void *)dst);
}

/* 递归:把 src 下的 *.jar 补进 dst(已经存在的不覆盖)——
 * Python: _install_optifine 里"它写的库(optifine/OptiFine、launchwrapper-of…)也要搬"。 */
typedef struct jar_walk {
    const char *dst;
    int copied;
    int failed;
} jar_walk;

static int copy_tree_jars_impl(const char *src, const char *dst, jar_walk *state);

static int copy_tree_jars_cb(void *ud, const char *full, const char *name, int is_dir)
{
    jar_walk *state = (jar_walk *)ud;
    char target[SXCL_LOADER_CMD_ARG_MAX];
    if (is_dir) {
        if (join_path(target, sizeof(target), state->dst, name) != 0) {
            state->failed = 1;
            return 0;
        }
        if (sxcl_fs_mkdirs(target) != 0) {
            state->failed = 1;
            return 0;
        }
        if (copy_tree_jars_impl(full, target, state) != 0) {
            state->failed = 1;
        }
        return 0;
    }
    if (!has_suffix_ci(name, ".jar")) {
        return 0;
    }
    if (join_path(target, sizeof(target), state->dst, name) != 0) {
        state->failed = 1;
        return 0;
    }
    const int rc = copy_file(full, target, 1);
    if (rc < 0) {
        state->failed = 1;
    } else if (rc == 0) {
        ++state->copied;
    }
    return 0;
}

static int copy_tree_jars_impl(const char *src, const char *dst, jar_walk *state)
{
    state->dst = dst;
    return list_dir(src, copy_tree_jars_cb, state);
}

static int copy_tree_jars(const char *src, const char *dst)
{
    if (!sxcl_fs_is_dir(src)) {
        return 0;
    }
    jar_walk state;
    state.dst = dst;
    state.copied = 0;
    state.failed = 0;
    if (copy_tree_jars_impl(src, dst, &state) != 0 || state.failed) {
        return -1;
    }
    return state.copied;
}

/* ── 命令与进度(纯函数,可单测) ── */

static int cmd_add_arg(sxcl_loader_cmd *cmd, const char *text)
{
    if (!cmd || cmd->argc >= SXCL_LOADER_CMD_MAX_ARGS) {
        return -1;
    }
    if (text) {
        const size_t len = strlen(text);
        if (len + 1 > sizeof(cmd->args[0])) {
            return -1;
        }
        memcpy(cmd->args[cmd->argc], text, len + 1);
    } else {
        cmd->args[cmd->argc][0] = '\0';
    }
    ++cmd->argc;
    return 0;
}

static int cmd_set_field(char *dst, size_t cap, const char *text)
{
    const size_t len = text ? strlen(text) : 0;
    if (len + 1 > cap) {
        dst[0] = '\0';
        return -1;
    }
    if (text) {
        memcpy(dst, text, len + 1);
    } else {
        dst[0] = '\0';
    }
    return 0;
}

static int render_install_dir_arg(char *out, size_t cap, const char *dir)
{
    const int written = snprintf(out, cap, "--installDir=%s", dir);
    return (written <= 0 || (size_t)written >= cap) ? -1 : 0;
}

/* 所有组合都以 "-jar <安装器>" 开头(Python: base = [java, "-jar", installer])。 */
static int cmd_begin(sxcl_loader_cmd *cmd, const sxcl_loader_cmd_env *env, const char *work_dir,
                     const char *desc)
{
    memset(cmd, 0, sizeof(*cmd));
    if (cmd_set_field(cmd->program, sizeof(cmd->program), env->java_path) != 0) {
        return -1;
    }
    if (cmd_set_field(cmd->work_dir, sizeof(cmd->work_dir), work_dir) != 0) {
        return -1;
    }
    if (cmd_set_field(cmd->desc, sizeof(cmd->desc), desc) != 0) {
        return -1;
    }
    if (env->fake_appdata && env->fake_appdata[0] &&
        cmd_set_field(cmd->appdata, sizeof(cmd->appdata), env->fake_appdata) != 0) {
        return -1;
    }
    if (cmd_add_arg(cmd, "-jar") != 0 || cmd_add_arg(cmd, env->installer_jar) != 0) {
        return -1;
    }
    return 0;
}

size_t sxcl_loader_build_commands(sxcl_loader_kind kind, const sxcl_loader_cmd_env *env,
                                  sxcl_loader_cmd *out, size_t out_cap)
{
    if (!env || !env->java_path || !env->installer_jar || !env->game_dir || !out || out_cap == 0) {
        return 0;
    }
    const char *game_dir = env->game_dir;
    size_t count = 0;
    sxcl_loader_cmd cmd;
    char scratch[SXCL_LOADER_CMD_ARG_MAX];

    if (kind == SXCL_LOADER_FORGE || kind == SXCL_LOADER_NEOFORGE) {
        /* 1) 首选:--installClient <游戏目录>(任务点名的写法;目录是参数的"值")。 */
        if (cmd_begin(&cmd, env, game_dir, "现代 --installClient <目录>") == 0 &&
            cmd_add_arg(&cmd, "--installClient") == 0 && cmd_add_arg(&cmd, game_dir) == 0 &&
            (!env->mirror_maven || !env->mirror_maven[0] ||
             (cmd_add_arg(&cmd, "--mirror") == 0 && cmd_add_arg(&cmd, env->mirror_maven) == 0))) {
            out[count++] = cmd;
        }
        /* 2) 装到当前目录(工作目录已经是游戏目录)。 */
        if (count < out_cap && cmd_begin(&cmd, env, game_dir, "现代 --installClient（装到当前目录）") == 0 &&
            cmd_add_arg(&cmd, "--installClient") == 0 &&
            (!env->mirror_maven || !env->mirror_maven[0] ||
             (cmd_add_arg(&cmd, "--mirror") == 0 && cmd_add_arg(&cmd, env->mirror_maven) == 0))) {
            out[count++] = cmd;
        }
        /* 3) 老安装器只认 --installDir=<目录>。 */
        if (count < out_cap && render_install_dir_arg(scratch, sizeof(scratch), game_dir) == 0 &&
            cmd_begin(&cmd, env, game_dir, "旧 --installDir=<目录>") == 0 &&
            cmd_add_arg(&cmd, scratch) == 0 &&
            (!env->mirror_maven || !env->mirror_maven[0] ||
             (cmd_add_arg(&cmd, "--mirror") == 0 && cmd_add_arg(&cmd, env->mirror_maven) == 0))) {
            out[count++] = cmd;
        }
        /* 4) 兜底(依旧不带"只有 jar"的命令行,不弹 GUI)。 */
        if (count < out_cap && cmd_begin(&cmd, env, game_dir, "兜底 --installClient --target") == 0 &&
            cmd_add_arg(&cmd, "--installClient") == 0 && cmd_add_arg(&cmd, "--target") == 0 &&
            cmd_add_arg(&cmd, game_dir) == 0) {
            out[count++] = cmd;
        }
        return count;
    }

    if (kind == SXCL_LOADER_FABRIC || kind == SXCL_LOADER_QUILT) {
        /* Python: _build_command_variants 的 Fabric 分支,但参数是**单横线**的,而且 -dir 要指
         * **游戏目录**(安装器在那儿找 launcher_profiles.json,再把版本建到
         * <游戏目录>/versions/<它自己的名字>,由 adopt_generated 收编)。
         *
         * 实测(fabric-installer 1.1.2,2026-09-22):
         *   单横线 + -dir <游戏目录> -> "Installing 1.21.11 with fabric 0.19.5" + "Creating profile",
         *                              退出码 0,生成 versions/fabric-loader-0.19.5-1.21.11/;
         *   双横线(--mcversion …)    -> 参数**全部被忽略**:退到"当前最新正式版"(那天是 26.3),
         *                              再去 <版本目录> 找 launcher_profiles.json,报
         *                              "Could not find a valid launcher profile .json",退出码 1。
         * 也就是:**双横线那一版从来没成功过** —— 之前装出来的 fabric 实例就是被这一条坑的。 */
        const char *base = env->base_version ? env->base_version : "";
        const char *loader = env->loader_version ? env->loader_version : "";
        const char *instance = env->instance_name ? env->instance_name : "";
        const int with_name = (instance[0] != '\0');
        for (int variant = 0; variant < 2; ++variant) {
            if (count >= out_cap) {
                break;
            }
            if (variant == 0 && !with_name) {
                continue;   /* 没有实例名时两组参数一模一样,只留一组 */
            }
            if (cmd_begin(&cmd, env, game_dir, variant == 0 ? "Fabric client -name" : "Fabric client") != 0 ||
                cmd_add_arg(&cmd, "client") != 0) {
                continue;
            }
            int ok = 1;
            if (base[0]) {
                ok = ok && cmd_add_arg(&cmd, "-mcversion") == 0 && cmd_add_arg(&cmd, base) == 0;
            }
            if (ok && loader[0]) {
                ok = ok && cmd_add_arg(&cmd, "-loader") == 0 && cmd_add_arg(&cmd, loader) == 0;
            }
            if (ok) {
                ok = ok && cmd_add_arg(&cmd, "-dir") == 0 && cmd_add_arg(&cmd, game_dir) == 0;
            }
            if (ok && variant == 0) {
                ok = ok && cmd_add_arg(&cmd, "-name") == 0 && cmd_add_arg(&cmd, instance) == 0;
            }
            if (ok) {
                out[count++] = cmd;
            }
        }
        return count;
    }

    if (kind == SXCL_LOADER_OPTIFINE) {
        /* OptiFine 只有一条:--installClient <沙箱里的假游戏目录>,而且必须在沙箱 APPDATA 下跑
         * (它忽略我们传的目录,只认 %APPDATA%\.minecraft)。 */
        if (cmd_begin(&cmd, env, game_dir, "OptiFine --installClient（沙箱）") == 0 &&
            cmd_add_arg(&cmd, "--installClient") == 0 && cmd_add_arg(&cmd, game_dir) == 0) {
            out[count++] = cmd;
        }
        return count;
    }

    (void)scratch;
    return 0;   /* vanilla:没有安装器可跑 */
}

int sxcl_loader_parse_progress(const char *line, sxcl_loader_progress *out)
{
    if (!out) {
        return SXCL_LOADER_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->percent = -1;
    out->stage = SXCL_LOADER_PROGRESS_NONE;
    if (!line) {
        return SXCL_LOADER_OK;
    }

    /* 完成标记:先看这一行是不是安装器在说"装完了"。 */
    char trimmed[SXCL_LOADER_TEXT_MAX];
    size_t t = 0;
    while (line[t] == ' ' || line[t] == '\t') {
        ++t;
    }
    size_t e = strlen(line);
    while (e > t && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) {
        --e;
    }
    size_t copy_len = e - t;
    if (copy_len >= sizeof(trimmed)) {
        copy_len = sizeof(trimmed) - 1;
    }
    memcpy(trimmed, line + t, copy_len);
    trimmed[copy_len] = '\0';

    int is_true = (trimmed[0] != '\0');
    for (size_t i = 0; trimmed[i]; ++i) {
        char c = trimmed[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != "true"[i]) {
            is_true = 0;
            break;
        }
    }
    if (is_true) {
        /* Python: printed_true = any(l.strip().lower() == "true" ...) */
        out->matched = 1;
        out->finished = 1;
        out->percent = 100;
        out->stage = SXCL_LOADER_PROGRESS_FINISHED;
        (void)snprintf(out->text, sizeof(out->text), "安装器报告完成");
        return SXCL_LOADER_OK;
    }
    if (strstr(line, "installation was successful") || strstr(line, "successfully installed")) {
        out->matched = 1;
        out->finished = 1;
        out->percent = 100;
        out->stage = SXCL_LOADER_PROGRESS_FINISHED;
        (void)snprintf(out->text, sizeof(out->text), "安装器报告安装成功");
        return SXCL_LOADER_OK;
    }

    if (strstr(line, "Extracting json")) {
        out->matched = 1;
        out->percent = 35;
        out->stage = SXCL_LOADER_PROGRESS_EXTRACT_JSON;
        (void)snprintf(out->text, sizeof(out->text), "解压版本信息");
    } else if (strstr(line, "Downloading libraries")) {
        out->matched = 1;
        out->percent = 45;
        out->stage = SXCL_LOADER_PROGRESS_DOWNLOAD_LIBS;
        (void)snprintf(out->text, sizeof(out->text), "下载支持库");
    } else if (strstr(line, "Building Processors")) {
        out->matched = 1;
        out->percent = 60;
        out->stage = SXCL_LOADER_PROGRESS_BUILD_PROCESSORS;
        (void)snprintf(out->text, sizeof(out->text), "执行安装处理器");
    } else if (strncmp(line, "Task: ", 6) == 0) {
        out->matched = 1;
        out->percent = 70;
        out->stage = SXCL_LOADER_PROGRESS_TASK;
        (void)snprintf(out->text, sizeof(out->text), "处理器任务 %s", line + 6);
    }
    return SXCL_LOADER_OK;
}

/* ── 沙箱 APPDATA ── */

static int g_saved_appdata_valid = 0;
#if defined(_WIN32)
static wchar_t g_saved_appdata[1024];

static int sandbox_env_push(const char *appdata)
{
    if (!appdata || !appdata[0]) {
        return 0;
    }
    wchar_t *wide = utf8_to_wide(appdata);
    if (!wide) {
        return 0;
    }
    g_saved_appdata_valid = 0;
    const DWORD cap = (DWORD)(sizeof(g_saved_appdata) / sizeof(g_saved_appdata[0]));
    if (GetEnvironmentVariableW(L"APPDATA", g_saved_appdata, cap) > 0) {
        g_saved_appdata_valid = 1;
    }
    (void)SetEnvironmentVariableW(L"APPDATA", wide);
    free(wide);
    return 1;
}

static void sandbox_env_pop(int pushed)
{
    if (!pushed) {
        return;
    }
    (void)SetEnvironmentVariableW(L"APPDATA", g_saved_appdata_valid ? g_saved_appdata : NULL);
    g_saved_appdata_valid = 0;
}
#else
static char g_saved_appdata[1024];

static int sandbox_env_push(const char *appdata)
{
    if (!appdata || !appdata[0]) {
        return 0;
    }
    const char *old = getenv("APPDATA");
    g_saved_appdata_valid = 0;
    if (old && strlen(old) < sizeof(g_saved_appdata)) {
        memcpy(g_saved_appdata, old, strlen(old) + 1);
        g_saved_appdata_valid = 1;
    }
    (void)setenv("APPDATA", appdata, 1);
    return 1;
}

static void sandbox_env_pop(int pushed)
{
    if (!pushed) {
        return;
    }
    if (g_saved_appdata_valid) {
        (void)setenv("APPDATA", g_saved_appdata, 1);
    } else {
        (void)unsetenv("APPDATA");
    }
    g_saved_appdata_valid = 0;
}
#endif

/* ── 临时目录与递归删除(OptiFine 沙箱用) ── */

/* 建一个"一次性临时根目录"并返回它的路径。tag 只用来区分用途(OptiFine 沙箱 / processors 取文件),
 * 都落在系统临时目录下,名字里带 pid 与 tick,保证并发调用也不会撞。 */
static int make_temp_root(char *out, size_t cap, const char *tag)
{
    int written = -1;
#if defined(_WIN32)
    wchar_t wtmp[SXCL_LOADER_CMD_ARG_MAX];
    const DWORD got = GetTempPathW((DWORD)(sizeof(wtmp) / sizeof(wtmp[0])), wtmp);
    if (got == 0 || got >= (DWORD)(sizeof(wtmp) / sizeof(wtmp[0]))) {
        wcscpy(wtmp, L".");
    }
    char *base = wide_to_utf8(wtmp);
    if (!base) {
        return -1;
    }
    size_t len = strlen(base);
    while (len > 1 && (base[len - 1] == '\\' || base[len - 1] == '/')) {
        base[--len] = '\0';
    }
    written = snprintf(out, cap, "%s/sxcl-%s-%lu-%lu", base, tag ? tag : "tmp",
                       (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    free(base);
#else
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) {
        base = "/tmp";
    }
    written = snprintf(out, cap, "%s/sxcl-%s-%ld-%ld", base, tag ? tag : "tmp", (long)getpid(),
                       (long)time(NULL));
#endif
    if (written <= 0 || (size_t)written >= cap) {
        out[0] = '\0';
        return -1;
    }
    return sxcl_fs_mkdirs(out);
}

static int remove_dir(const char *dir)
{
#if defined(_WIN32)
    wchar_t *wide = utf8_to_wide(dir);
    if (!wide) {
        return -1;
    }
    const BOOL ok = RemoveDirectoryW(wide);
    free(wide);
    return ok ? 0 : -1;
#else
    return rmdir(dir);
#endif
}

static int remove_tree(const char *dir);

static int remove_tree_cb(void *ud, const char *full, const char *name, int is_dir)
{
    (void)ud;
    (void)name;
    if (is_dir) {
        (void)remove_tree(full);
    } else {
        (void)sxcl_fs_remove(full);
    }
    return 0;
}

static int remove_tree(const char *dir)
{
    if (!sxcl_fs_is_dir(dir)) {
        return 0;
    }
    (void)list_dir(dir, remove_tree_cb, NULL);
    return remove_dir(dir);
}

static int contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) {
        return 0;
    }
    for (size_t i = 0; hay[i]; ++i) {
        size_t k = 0;
        for (; needle[k]; ++k) {
            char a = hay[i + k];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            char b = needle[k];
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a == '\0' || a != b) {
                break;
            }
        }
        if (needle[k] == '\0') {
            return 1;
        }
    }
    return 0;
}

/* ── 安装上下文 ── */

typedef struct install_ctx {
    const sxcl_loader_install_request *req;
    sxcl_loader_install_result *res;
    int percent;
    int finished;      /* 这一轮看到安装器的完成标记 */
    int cancelled;     /* 用户取消 */
    int timed_out;     /* 安装器进程超时 */
    /** 沙箱 APPDATA 的兜底通道:1 = 除了 opts->env,还临时改本进程的 APPDATA。
     *  默认 0(只走 process.h 的 env,这是首选通道);只有在"子进程明显没认环境块"时
     *  才由 install_optifine 打开重试一次。 */
    int process_env_fallback;
    sxcl_loader_fail_stage fail_stage;
    char last_error[SXCL_LOADER_ERROR_MAX];
    /* 安装器输出的最后几行(Python: _last_output 的 deque(maxlen=80),失败时拿它们当人话原因)。
     * 这就是"退出码 1"背后真正的解释,不给用户看等于让人瞎猜。 */
    char last_lines[3][SXCL_LOADER_TEXT_MAX];
    int last_line_count;
    /* 拍平用的**合并基准**(原版版本 JSON)。必须在安装器开工**之前**读下来:
     * 引擎刚把原版 JSON 放在 versions/<实例>/<实例>.json,安装器随后会把它盖掉。
     * NULL = 找不到(那就拍不了,只能按继承式写,并如实报出来)。 */
    sxcl_json *base_doc;
    char base_id[SXCL_LOADER_CMD_ARG_MAX];   /**< 原版版本号(写进 clientVersion) */
    /* 方式 B 重放 processors 时要用的两样东西(都是"这一趟"的现场,不是持久状态):
     * 活动中的安装器 zip(data 里的裸值要从它里面取)与取出来的临时目录。 */
    sxcl_zip *zip;
    char temp_dir[SXCL_LOADER_CMD_ARG_MAX];
} install_ctx;

static void ctx_report(install_ctx *ctx, int percent, const char *fmt, ...)
{
    char status[SXCL_LOADER_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    if (percent > ctx->percent) {
        ctx->percent = percent;
    }
    ctx->res->percent = ctx->percent;
    if (ctx->req->on_progress) {
        ctx->req->on_progress(ctx->req->userdata, ctx->percent, status);
    }
}

static void ctx_fail(install_ctx *ctx, sxcl_loader_fail_stage stage, const char *fmt, ...)
{
    ctx->fail_stage = stage;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(ctx->last_error, sizeof(ctx->last_error), fmt, ap);
    va_end(ap);
}

static int ctx_cancelled(const install_ctx *ctx)
{
    return (ctx->req->is_cancelled && ctx->req->is_cancelled(ctx->req->cancel_userdata)) ? 1 : 0;
}

static int install_on_line(void *ud, int is_stderr, const char *line)
{
    install_ctx *ctx = (install_ctx *)ud;
    if (line && line[0]) {
        /* 原始行:先给调用方(CLI 的 --verbose / 启动器的日志),再自己留一份尾巴。
         * 回调返回非 0 = 调用方要求终止这次安装(与 process.h 的 on_line 同语义)。 */
        if (ctx->req->on_line && ctx->req->on_line(ctx->req->userdata, is_stderr, line) != 0) {
            ctx->cancelled = 1;
            return 1;
        }
        const size_t copy = strlen(line) < sizeof(ctx->last_lines[0]) - 1
                                ? strlen(line)
                                : sizeof(ctx->last_lines[0]) - 1;
        if (ctx->last_line_count < 3) {
            memcpy(ctx->last_lines[ctx->last_line_count], line, copy);
            ctx->last_lines[ctx->last_line_count][copy] = '\0';
            ++ctx->last_line_count;
        } else {
            /* 只有三行,直接整体前移一格,不值得上环形索引 */
            memcpy(ctx->last_lines[0], ctx->last_lines[1], sizeof(ctx->last_lines[0]));
            memcpy(ctx->last_lines[1], ctx->last_lines[2], sizeof(ctx->last_lines[0]));
            memcpy(ctx->last_lines[2], line, copy);
            ctx->last_lines[2][copy] = '\0';
        }
    }
    if (line && line[0]) {
        sxcl_loader_progress progress;
        if (sxcl_loader_parse_progress(line, &progress) == SXCL_LOADER_OK && progress.matched) {
            ctx_report(ctx, progress.percent >= 0 ? progress.percent : ctx->percent, progress.text);
            if (progress.finished) {
                ctx->finished = 1;
                return 1;   /* 看到完成标记就主动收工 —— 别死等安装器自己退出 */
            }
        }
    }
    /* process.h 的 on_line 是"跑的过程中"唯一能插进我们代码的地方,取消只能在这里查。 */
    if (ctx_cancelled(ctx)) {
        ctx->cancelled = 1;
        return 1;
    }
    return 0;
}

/* 安装器最后说的那句话 —— 失败时它就是"人话原因"的主体。 */
static const char *ctx_last_line(const install_ctx *ctx)
{
    return ctx->last_line_count > 0 ? ctx->last_lines[ctx->last_line_count - 1] : "";
}

static int effective_timeout_ms(const sxcl_loader_install_request *req)
{
    if (req->timeout_ms > 0) {
        return req->timeout_ms;
    }
    return req->kind == SXCL_LOADER_OPTIFINE ? SXCL_LOADER_OPTIFINE_TIMEOUT_MS
                                             : SXCL_LOADER_DEFAULT_TIMEOUT_MS;
}

/* 跑一组命令行。返回 sxcl_process_run 的返回值(0 = 跑完了,-1 = 没启动起来)。 */
static int run_variant(install_ctx *ctx, const sxcl_loader_cmd *cmd, sxcl_process_result *pres)
{
    const char *args[SXCL_LOADER_CMD_MAX_ARGS + 1];
    size_t argc = cmd->argc;
    if (argc > SXCL_LOADER_CMD_MAX_ARGS) {
        argc = SXCL_LOADER_CMD_MAX_ARGS;
    }
    for (size_t i = 0; i < argc; ++i) {
        args[i] = cmd->args[i];
    }
    args[argc] = NULL;

    char appdata_env[SXCL_LOADER_CMD_ARG_MAX + 16];
    const char *env[2];
    size_t env_count = 0;
    if (cmd->appdata[0]) {
        (void)snprintf(appdata_env, sizeof(appdata_env), "APPDATA=%s", cmd->appdata);
        env[env_count++] = appdata_env;
    }
    env[env_count] = NULL;

    sxcl_process_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.program = cmd->program;
    opts.args = args;
    opts.work_dir = cmd->work_dir[0] ? cmd->work_dir : NULL;
    opts.env = env_count > 0 ? env : NULL;
    opts.timeout_ms = effective_timeout_ms(ctx->req);
    opts.on_line = install_on_line;
    opts.userdata = ctx;

    ctx->finished = 0;
    ctx->cancelled = 0;
    ctx->last_line_count = 0;
    /* 沙箱 APPDATA 的首选通道就是 opts->env(process.h 两条腿都实现了:
     * POSIX 用 putenv,Windows 用"父环境 + 覆盖项 → 环境块 + CREATE_UNICODE_ENVIRONMENT")。
     * 只有兜底模式才额外临时改本进程的环境变量 —— 那是给"进程后端不认 env"的情况准备的,
     * 正常情况下不动全局状态。 */
    const int pushed = ctx->process_env_fallback ? sandbox_env_push(cmd->appdata) : 0;
    const int rc = sxcl_process_run(&opts, pres);
    sandbox_env_pop(pushed);
    return rc;
}

/* ── 产物判定 ── */

/* Python: _handle_generated_files —— 安装器生成的文件不一定落在我们预设的目录名里,按候选名找回来。 */
/* 收编安装器生成的目录后要清掉引擎放下的原版 JSON(实现见后面的拍平一节)。 */
static void drop_stale_vanilla(const char *instance_dir, const char *instance_name);

static int adopt_generated(install_ctx *ctx, const char *versions_dir, const char *instance_dir)
{
    const char *mc = ctx->req->base_version ? ctx->req->base_version : "";
    const char *loader = ctx->req->loader_version ? ctx->req->loader_version : "";
    const char *id = sxcl_loader_kind_id(ctx->req->kind);
    char names[4][192];
    size_t count = 0;
    if (ctx->req->kind == SXCL_LOADER_FABRIC && loader[0] && mc[0]) {
        (void)snprintf(names[count], sizeof(names[0]), "fabric-loader-%s-%s", loader, mc);
        ++count;
    }
    if (mc[0] && loader[0]) {
        (void)snprintf(names[count], sizeof(names[0]), "%s-%s-%s", mc, id, loader);
        ++count;
    }
    if (loader[0]) {
        (void)snprintf(names[count], sizeof(names[0]), "%s-%s-%s", id, loader, mc);
        ++count;
        (void)snprintf(names[count], sizeof(names[0]), "%s-%s", id, loader);
        ++count;
    }
    for (size_t i = 0; i < count; ++i) {
        char candidate[SXCL_LOADER_CMD_ARG_MAX];
        if (join_path(candidate, sizeof(candidate), versions_dir, names[i]) != 0) {
            continue;
        }
        if (strcmp(candidate, instance_dir) == 0 || !dir_has_json(candidate)) {
            continue;
        }
        (void)copy_dir_files(candidate, instance_dir);
        if (!dir_has_json(instance_dir)) {
            return 0;
        }
        /* 版本目录里原本躺着的是**引擎刚放下的原版 JSON**(拍平的合并基准,已经在 ctx->base_doc 里)。
         * 它和安装器写出来的 JSON 会抢同一个文件名(normalize 把每份 JSON 都改成 <实例名>.json),
         * 谁赢要看目录顺序 —— 那是不确定的。基准已经拿到手,这里把它清掉,只留安装器那一份。 */
        drop_stale_vanilla(instance_dir, ctx->req->instance_name);
        return dir_has_json(instance_dir);
    }
    return 0;
}

static int variant_succeeded(install_ctx *ctx, const sxcl_process_result *pres,
                             const char *versions_dir, const char *instance_dir)
{
    /* 结束判据:返回码为 0,或者看到安装器的完成标记(那时是我们主动终止的进程)。
     * 不管哪种,产物都必须真的在 —— "jar 生成了"不算数。 */
    if (!ctx->finished && pres->exit_code != 0) {
        return 0;
    }
    if (dir_has_json(instance_dir)) {
        return 1;
    }
    return adopt_generated(ctx, versions_dir, instance_dir);
}

/* Python: _normalize_version_files —— 把版本 JSON 的 id 与文件名统一成实例名
 * (启动器是按目录名找版本的)。 */
typedef struct normalize_ctx {
    const char *instance_name;
    char desired[SXCL_LOADER_CMD_ARG_MAX];
    int failed;
} normalize_ctx;

static int normalize_cb(void *ud, const char *full, const char *name, int is_dir)
{
    normalize_ctx *state = (normalize_ctx *)ud;
    if (is_dir || !has_suffix_ci(name, ".json")) {
        return 0;
    }
    char err[192];
    err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse_file(full, err, sizeof(err));
    if (!doc) {
        return 0;   /* Python: 读不了就跳过,不算失败 */
    }
    const sxcl_loader_json_override overrides[1] = {{"id", state->instance_name}};
    char *text = NULL;
    if (sxcl_loader_json_dump(sxcl_json_root(doc), overrides, 1, &text, err, sizeof(err)) ==
            SXCL_LOADER_OK && text) {
        if (write_text_atomic(state->desired, text) != 0) {
            state->failed = 1;
        } else if (strcmp(full, state->desired) != 0) {
            (void)sxcl_fs_remove(full);
        }
        free(text);
    } else {
        state->failed = 1;
    }
    sxcl_json_free(doc);
    return 0;
}

static int normalize_instance(const char *instance_dir, const char *instance_name)
{
    normalize_ctx state;
    memset(&state, 0, sizeof(state));
    state.instance_name = instance_name;
    char leaf[SXCL_LOADER_CMD_ARG_MAX];
    const int written = snprintf(leaf, sizeof(leaf), "%s.json", instance_name);
    if (written <= 0 || (size_t)written >= sizeof(leaf)) {
        return -1;
    }
    if (join_path(state.desired, sizeof(state.desired), instance_dir, leaf) != 0) {
        return -1;
    }
    if (!sxcl_fs_is_dir(instance_dir)) {
        return -1;
    }
    (void)list_dir(instance_dir, normalize_cb, &state);
    return state.failed ? -1 : 0;
}

/* ── 方式 B:解包安装 ── */

static const char *maven_for_kind(sxcl_loader_kind kind)
{
    if (kind == SXCL_LOADER_NEOFORGE) {
        return MAVEN_NEOFORGE;
    }
    if (kind == SXCL_LOADER_FABRIC || kind == SXCL_LOADER_QUILT) {
        return MAVEN_FABRIC;
    }
    return MAVEN_FORGE;
}

static char *read_zip_text(sxcl_zip *zip, const char *name)
{
    const int index = sxcl_zip_find(zip, name);
    if (index < 0) {
        return NULL;
    }
    const int64_t size = sxcl_zip_size_at(zip, (size_t)index);
    if (size < 0 || size > (int64_t)8 * 1024 * 1024) {
        return NULL;
    }
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        return NULL;
    }
    size_t got = 0;
    if (sxcl_zip_extract_memory(zip, name, text, (size_t)size, &got) != SXCL_ZIP_OK) {
        free(text);
        return NULL;
    }
    text[got] = '\0';
    return text;
}

/* 把一个 jar 条目解到 <game>/libraries/<rel>(rel 用 '/' 分隔)。 */
static int extract_to_libraries(sxcl_zip *zip, const char *entry, const char *rel, const char *game_dir)
{
    char dir[SXCL_LOADER_CMD_ARG_MAX];
    char dest[SXCL_LOADER_CMD_ARG_MAX];
    if (join_path(dir, sizeof(dir), game_dir, "libraries") != 0) {
        return -1;
    }
    if (join_path(dest, sizeof(dest), dir, rel) != 0) {
        return -1;
    }
    return sxcl_zip_extract_file(zip, entry, dest) == SXCL_ZIP_OK ? 0 : -1;
}

static size_t count_colons(const char *text)
{
    size_t n = 0;
    for (; text && *text; ++text) {
        if (*text == ':') {
            ++n;
        }
    }
    return n;
}

static void strip_leading_slashes(const char *text, char *out, size_t cap)
{
    while (*text == '/' || *text == '\\') {
        ++text;
    }
    size_t n = strlen(text);
    while (n > 0 && (text[n - 1] == '/')) {
        --n;
    }
    if (n + 1 > cap) {
        out[0] = '\0';
        return;
    }
    memcpy(out, text, n);
    out[n] = '\0';
    for (size_t i = 0; i < n; ++i) {
        if (out[i] == '\\') {
            out[i] = '/';
        }
    }
}

static void copy_vanilla_client(const char *game_dir, const char *mc, const char *instance_dir,
                                const char *instance_name)
{
    char leaf[SXCL_LOADER_CMD_ARG_MAX];
    char target[SXCL_LOADER_CMD_ARG_MAX];
    char src_dir[SXCL_LOADER_CMD_ARG_MAX];
    char src[SXCL_LOADER_CMD_ARG_MAX];
    if (snprintf(leaf, sizeof(leaf), "%s.jar", instance_name) <= 0 ||
        join_path(target, sizeof(target), instance_dir, leaf) != 0) {
        return;
    }
    if (sxcl_fs_exists(target)) {
        return;
    }
    if (join_path(src_dir, sizeof(src_dir), game_dir, "versions") != 0 ||
        join_path(src_dir, sizeof(src_dir), src_dir, mc) != 0) {
        return;
    }
    if (snprintf(leaf, sizeof(leaf), "%s.jar", mc) <= 0 ||
        join_path(src, sizeof(src), src_dir, leaf) != 0) {
        return;
    }
    if (sxcl_fs_exists(src)) {
        (void)copy_file(src, target, 0);   /* Python: 原版 client.jar 有就复制一份(继承也能跑,复制更保险) */
    }
}

/* ── 拍平:读原版 -> 合并 -> 原子写盘（规则与理由见 loader.h 的 sxcl_loader_flatten_json） ── */

/* 读一整份文本文件(UTF-8)。失败返回 NULL(不区分"不在"与"读不动")。 */
static char *read_text_file(const char *path)
{
    FILE *fh = open_utf8(path, 0);
    if (!fh) {
        return NULL;
    }
    if (fseek(fh, 0, SEEK_END) != 0) {
        (void)fclose(fh);
        return NULL;
    }
    const long size = ftell(fh);
    if (size < 0 || size > (long)(64L * 1024L * 1024L) || fseek(fh, 0, SEEK_SET) != 0) {
        (void)fclose(fh);
        return NULL;
    }
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        (void)fclose(fh);
        return NULL;
    }
    const size_t got = fread(text, 1, (size_t)size, fh);
    (void)fclose(fh);
    text[got] = '\0';
    return text;
}

/* 该拿哪个原版来合并:JSON 里的 inheritsFrom 最准(安装器自己写的),其次调用方给的原版版本号。 */
static const char *pick_base_id(const sxcl_json_value *loader_root, const char *fallback)
{
    const char *own = loader_root ? sxcl_json_get_string(loader_root, "inheritsFrom", NULL) : NULL;
    if (own && own[0]) {
        return own;
    }
    return (fallback && fallback[0]) ? fallback : NULL;
}

/* has_inherits:这份版本 JSON 是不是"靠继承"的(决定能不能拍平 —— 已经是独立版本的再合并
 * 会把原版的游戏参数/库**再加一遍**,那是损坏而不是拍平)。 */
static int json_has_inherits(const sxcl_json_value *root)
{
    const char *own = root ? sxcl_json_get_string(root, "inheritsFrom", NULL) : NULL;
    return (own && own[0]) ? 1 : 0;
}

/* 读 <game>/versions/<base>/<base>.json -> 拍平 -> 原子写进 target_path。
 * 失败把原因写进 err:**不静默退回继承式**(拍平不了就是拍平不了,由调用方决定怎么办)。 */
static int flatten_write_json(const sxcl_json_value *base_root, const char *base_id,
                              const char *instance_name, const sxcl_json_value *loader_root,
                              const char *target_path, char *err, size_t err_len)
{
    if (err && err_len) {
        err[0] = '\0';
    }
    if (!base_id || !base_id[0]) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "不知道原版版本号,拍不出独立版本 JSON");
        }
        return SXCL_LOADER_ERR_ARG;
    }

    if (!base_root) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "没有可合并的原版版本 JSON");
        }
        return SXCL_LOADER_ERR_ARG;
    }

    char *merged = NULL;
    int rc = sxcl_loader_flatten_json(loader_root, base_root, instance_name, base_id, &merged, err,
                                      err_len);
    if (rc == SXCL_LOADER_OK && merged) {
        if (write_text_atomic(target_path, merged) != 0) {
            rc = SXCL_LOADER_ERR_IO;
            if (err && err_len) {
                (void)snprintf(err, err_len, "拍平后的版本 JSON 写不进去：%s", target_path);
            }
        }
    }
    free(merged);
    return rc;
}

/* 一份版本 JSON 是不是"原版"(Mojang 那份):拍平的**合并基准必须是它**。
 * 判据用 downloads.client —— 原版必有,加载器自己写的版本 JSON 不会有。 */
static int looks_like_vanilla(const sxcl_json_value *root)
{
    if (!root || sxcl_json_type_of(root) != SXCL_JSON_OBJECT || json_has_inherits(root)) {
        return 0;
    }
    return sxcl_json_get(sxcl_json_get(root, "downloads"), "client") != NULL;
}

/* 读一份版本 JSON 文件并解析(失败返回 NULL)。 */
static sxcl_json *load_json_file(const char *path)
{
    char *text = read_text_file(path);
    if (!text) {
        return NULL;
    }
    char perr[192];
    perr[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(text, strlen(text), perr, sizeof(perr));
    free(text);
    return doc;
}

/* 清掉版本目录里"引擎放下的那份原版 JSON"(它的内容已经作为合并基准读进内存了)。
 * 只删**确实是原版形态**的那份:安装器自己写出来的 JSON(带 inheritsFrom)绝不动。 */
static void drop_stale_vanilla(const char *instance_dir, const char *instance_name)
{
    char leaf[SXCL_LOADER_CMD_ARG_MAX];
    char path[SXCL_LOADER_CMD_ARG_MAX];
    const int wrote = snprintf(leaf, sizeof(leaf), "%s.json", instance_name);
    if (wrote <= 0 || (size_t)wrote >= sizeof(leaf) ||
        join_path(path, sizeof(path), instance_dir, leaf) != 0) {
        return;
    }
    sxcl_json *doc = load_json_file(path);
    if (!doc) {
        return;
    }
    const int vanilla = looks_like_vanilla(sxcl_json_root(doc));
    sxcl_json_free(doc);
    if (vanilla) {
        (void)sxcl_fs_remove(path);
    }
}

/* 找拍平用的合并基准(原版版本 JSON),两处找:
 *   ① versions/<实例>/<实例>.json —— **引擎在 loader_run 之前刚把原版 JSON 放在这儿**
 *      (install.c 的 version_json 阶段的目标就是它),安装器稍后才会把它盖掉,
 *      所以调用方必须在开工前读一次(见 sxcl_loader_install 的 ctx.base_doc);
 *   ② versions/<原版>/<原版>.json —— 原版自己单独装过的情况。
 * 两处都必须"看着像原版"才算 —— 宁可拍不了,也不拿加载器自己写的 JSON 当基准瞎合。 */
static sxcl_json *load_base_doc(const char *game_dir, const char *base_version,
                                const char *instance_dir, const char *instance_name)
{
    char leaf[SXCL_LOADER_CMD_ARG_MAX];
    char path[SXCL_LOADER_CMD_ARG_MAX];

    if (instance_dir && instance_dir[0] && instance_name && instance_name[0]) {
        const int wrote = snprintf(leaf, sizeof(leaf), "%s.json", instance_name);
        if (wrote > 0 && (size_t)wrote < sizeof(leaf) &&
            join_path(path, sizeof(path), instance_dir, leaf) == 0) {
            sxcl_json *doc = load_json_file(path);
            if (doc) {
                if (looks_like_vanilla(sxcl_json_root(doc))) {
                    return doc;
                }
                sxcl_json_free(doc);
            }
        }
    }
    if (game_dir && game_dir[0] && base_version && base_version[0]) {
        char sub[SXCL_LOADER_CMD_ARG_MAX];
        char dir[SXCL_LOADER_CMD_ARG_MAX];
        const int wrote = snprintf(leaf, sizeof(leaf), "%s.json", base_version);
        if (wrote > 0 && (size_t)wrote < sizeof(leaf) &&
            join_path(sub, sizeof(sub), game_dir, "versions") == 0 &&
            join_path(dir, sizeof(dir), sub, base_version) == 0 &&
            join_path(path, sizeof(path), dir, leaf) == 0) {
            sxcl_json *doc = load_json_file(path);
            if (doc) {
                if (looks_like_vanilla(sxcl_json_root(doc))) {
                    return doc;
                }
                sxcl_json_free(doc);
            }
        }
    }
    return NULL;
}

/* 把 versions/<实例名>/<实例名>.json 拍平一次(方式 A 的安装器产物走这条)。
 * 已经是独立版本(没有 inheritsFrom)就**什么都不做**并如实说明 —— 再合并会把原版的
 * 游戏参数与库再加一遍。note 里回一句人话(成功 = 用了哪个原版)。 */
static int flatten_instance_on_disk(install_ctx *ctx, const char *instance_dir,
                                    const char *instance_name, char *note, size_t note_len)
{
    char leaf[SXCL_LOADER_CMD_ARG_MAX];
    char path[SXCL_LOADER_CMD_ARG_MAX];
    const int wrote = snprintf(leaf, sizeof(leaf), "%s.json", instance_name);
    if (wrote <= 0 || (size_t)wrote >= sizeof(leaf) ||
        join_path(path, sizeof(path), instance_dir, leaf) != 0) {
        if (note && note_len) {
            (void)snprintf(note, note_len, "版本 JSON 的路径拼不出来");
        }
        return SXCL_LOADER_ERR_ARG;
    }

    sxcl_json *doc = load_json_file(path);
    if (!doc) {
        if (note && note_len) {
            (void)snprintf(note, note_len, "读不到 %s", path);
        }
        return SXCL_LOADER_ERR_IO;
    }

    const sxcl_json_value *root = sxcl_json_root(doc);
    int rc = SXCL_LOADER_OK;
    if (!json_has_inherits(root)) {
        if (note && note_len) {
            (void)snprintf(note, note_len, "版本 JSON 本来就是独立的(没有 inheritsFrom),不用拍平");
        }
    } else {
        sxcl_json *base_doc = ctx->base_doc;
        int owns_base = 0;
        if (!base_doc) {
            base_doc = load_base_doc(ctx->req->game_dir, ctx->base_id, NULL, NULL);
            owns_base = 1;
        }
        char ferr[SXCL_LOADER_ERROR_MAX];
        ferr[0] = '\0';
        rc = flatten_write_json(base_doc ? sxcl_json_root(base_doc) : NULL, ctx->base_id, instance_name,
                                root, path, ferr, sizeof(ferr));
        if (owns_base) {
            sxcl_json_free(base_doc);
        }
        if (note && note_len) {
            (void)snprintf(note, note_len, "%s",
                           rc == SXCL_LOADER_OK ? ctx->base_id : (ferr[0] ? ferr : "原因不明"));
        }
    }
    sxcl_json_free(doc);
    return rc;
}

/* 非拍平路径的写法:只改 id(必要时补 clientVersion = 原版版本号,PCL 的拍平标记)。
 * **不再凭空补 inheritsFrom** —— 独立版本被硬塞一个 inheritsFrom 会让实例扫描报
 * "前置版本缺失"(PCL 装出来的版本就没有这个键)。 */
static int write_inheriting_json(install_ctx *ctx, const sxcl_json_value *root, const char *target,
                                 const char *client_version)
{
    const sxcl_loader_json_override overrides[2] = {
        {"id", ctx->req->instance_name},
        {"clientVersion", client_version ? client_version : ""},
    };
    const size_t count = (client_version && client_version[0]) ? 2u : 1u;
    char derr[192];
    derr[0] = '\0';
    char *text = NULL;
    if (sxcl_loader_json_dump(root, overrides, count, &text, derr, sizeof(derr)) != SXCL_LOADER_OK ||
        !text) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "版本 JSON 写不出来（%s）", derr[0] ? derr : "格式不对");
        free(text);
        return -1;
    }
    const int rc = write_text_atomic(target, text);
    free(text);
    if (rc != 0) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "版本 JSON 落盘失败：%s", target);
        return -1;
    }
    return 0;
}

/* 数"版本 JSON 里声明的依赖库"有几件不在磁盘上 —— 装完的核对(见 loader.h 的说明)。
 * 判据只看文件在不在:**不做哈希**(那是下载引擎在安装/补全时的活,这里重复做纯属白读盘)。 */
int sxcl_loader_count_missing_libraries(const char *game_dir, const char *instance_dir,
                                        const char *instance_name, char *first_missing,
                                        size_t first_missing_len)
{
    if (first_missing && first_missing_len > 0) {
        first_missing[0] = '\0';
    }
    if (!game_dir || !instance_dir || !instance_name || !instance_name[0]) {
        return 0;
    }
    char leaf[SXCL_LOADER_CMD_ARG_MAX];
    char path[SXCL_LOADER_CMD_ARG_MAX];
    const int wrote = snprintf(leaf, sizeof(leaf), "%s.json", instance_name);
    if (wrote <= 0 || (size_t)wrote >= sizeof(leaf) ||
        join_path(path, sizeof(path), instance_dir, leaf) != 0) {
        return 0;
    }
    sxcl_json *doc = load_json_file(path);
    if (!doc) {
        return 0;   /* 没得核对(JSON 不在):调用方自己的 dir_has_json 已经管这件事了 */
    }
    const sxcl_json_value *libraries = sxcl_json_get(sxcl_json_root(doc), "libraries");
    const size_t count = sxcl_json_size(libraries);
    int missing = 0;
    for (size_t i = 0; i < count; ++i) {
        const sxcl_json_value *lib = sxcl_json_at(libraries, i);
        const char *rel = sxcl_json_get_string(
            sxcl_json_get(sxcl_json_get(lib, "downloads"), "artifact"), "path", NULL);
        if (!rel || !rel[0]) {
            continue;   /* 这条没有 artifact 路径(平台不适用 / 只有 classifiers):不核对 */
        }
        char dir[SXCL_LOADER_CMD_ARG_MAX * 2];
        char file[SXCL_LOADER_CMD_ARG_MAX * 2];
        if (join_path(dir, sizeof(dir), game_dir, "libraries") != 0 ||
            join_path(file, sizeof(file), dir, rel) != 0) {
            continue;
        }
        if (!sxcl_fs_exists(file)) {
            ++missing;
            if (first_missing && first_missing_len > 0 && first_missing[0] == '\0') {
                (void)snprintf(first_missing, first_missing_len, "%s", rel);
            }
        }
    }
    sxcl_json_free(doc);
    return missing;
}

/* Python: _extract_install —— 不启动安装器进程,直接从 jar 里拼出版本 JSON(老 Forge 只能这么装)。 */
/* ── processors 重放（1.13+ Forge / NeoForge 的安装真身，见 sxcl/processor.h） ── */

/** data 里的裸值（安装器 zip 里的相对路径）取出来落到临时文件，把临时文件路径交回去。
 *  FCL 用 Files.createTempFile 随机名；我们用"条目路径把斜杠换成下划线"当文件名 ——
 *  同样是"一条数据一个文件"，顺带避免了不同目录下同名条目（如两个 args.txt）互相覆盖。 */
static int processor_take_file(void *ud, const char *entry, char *out, size_t cap)
{
    install_ctx *ctx = (install_ctx *)ud;
    if (!ctx->zip || !ctx->temp_dir[0] || !entry || !entry[0]) {
        return -1;
    }
    /* 清单里写的是 "/data/client.lzma"，zip 里的条目名是 "data/client.lzma" ——
     * 我们按名字精确查条目，所以要先去掉前导斜杠（FCL 那边是 zip 文件系统的绝对路径，能直接开）。 */
    char name[SXCL_LOADER_CMD_ARG_MAX];
    strip_leading_slashes(entry, name, sizeof(name));
    if (!name[0]) {
        return -1;
    }
    char file_name[SXCL_LOADER_CMD_ARG_MAX];
    (void)snprintf(file_name, sizeof(file_name), "%s", name);
    for (size_t i = 0; file_name[i]; ++i) {
        if (file_name[i] == '/') {
            file_name[i] = '_';
        }
    }
    char dest[SXCL_LOADER_CMD_ARG_MAX];
    if (join_path(dest, sizeof(dest), ctx->temp_dir, file_name) != 0) {
        return -1;
    }
    if (sxcl_zip_extract_file(ctx->zip, name, dest) != 0) {
        return -1;
    }
    const int wrote = snprintf(out, cap, "%s", dest);
    return (wrote > 0 && (size_t)wrote < cap) ? 0 : -1;
}

/** 处理器输出的一行：转发给调用方（日志/界面）+ 自己留一行尾巴 + 检查取消。
 *  刻意**不**走 install_on_line —— 那里会解析安装器的进度标记，处理器的输出不该被当成
 *  "安装器完工"（那会让方式 A 的判定逻辑误判）。 */
static int processor_on_line(void *ud, int is_stderr, const char *line)
{
    install_ctx *ctx = (install_ctx *)ud;
    if (line && line[0]) {
        if (ctx->req->on_line && ctx->req->on_line(ctx->req->userdata, is_stderr, line) != 0) {
            ctx->cancelled = 1;
            return 1;
        }
        const size_t len = strlen(line);
        const size_t copy = len < sizeof(ctx->last_lines[0]) - 1 ? len : sizeof(ctx->last_lines[0]) - 1;
        memcpy(ctx->last_lines[2], line, copy);
        ctx->last_lines[2][copy] = '\0';
        ctx->last_line_count = 1;
    }
    if (ctx_cancelled(ctx)) {
        ctx->cancelled = 1;
        return 1;
    }
    return 0;
}

/** 跑一条处理器：走 sxcl/process.h（于是超时、日志、取消全都是安装流程那一套）。 */
static int processor_run(void *ud, const char *program, const char *const *argv, size_t argc,
                         const char *work_dir, int timeout_ms, int *exit_code, char *err,
                         size_t err_len)
{
    install_ctx *ctx = (install_ctx *)ud;
    (void)argc;   /* opts.args 是 NULL 结尾的，条数用不上 */
    if (exit_code) {
        *exit_code = -1;
    }
    sxcl_process_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.program = program;
    opts.args = argv;
    opts.work_dir = work_dir;
    opts.timeout_ms = timeout_ms;
    opts.on_line = processor_on_line;
    opts.userdata = ctx;

    ctx->last_line_count = 0;
    ctx->last_lines[2][0] = '\0';
    sxcl_process_result pres;
    memset(&pres, 0, sizeof(pres));
    const int rc = sxcl_process_run(&opts, &pres);
    if (rc != 0) {
        if (err && err_len) {
            (void)snprintf(err, err_len, "%s", pres.error[0] ? pres.error : "进程没启动起来");
        }
        return rc;
    }
    if (exit_code) {
        *exit_code = pres.exit_code;
    }
    if (pres.exit_code != 0 && err && err_len) {
        if (pres.timed_out) {
            (void)snprintf(err, err_len, "跑超时了（%d 秒）", timeout_ms / 1000);
        } else if (ctx->last_lines[2][0]) {
            (void)snprintf(err, err_len, "%s", ctx->last_lines[2]);
        } else {
            (void)snprintf(err, err_len, "退出码 %d", pres.exit_code);
        }
    }
    return 0;
}

static int processor_is_cancelled(void *ud)
{
    return ctx_cancelled((const install_ctx *)ud);
}

static void processor_report(void *ud, const char *text)
{
    install_ctx *ctx = (install_ctx *)ud;
    ctx_report(ctx, ctx->percent, "%s", text ? text : "");
}

/** 找原版客户端 jar：优先 versions/<原版>/<原版>.jar（原版自己装过），
 *  否则 versions/<实例>/<实例>.jar —— 引擎这次安装就是把它下在那儿的（install.c 的 client_jar 阶段）。 */
static int find_minecraft_jar(const sxcl_loader_install_request *req, const char *instance_dir,
                              char *out, size_t cap)
{
    out[0] = '\0';
    char leaf[SXCL_LOADER_CMD_ARG_MAX];
    char dir[SXCL_LOADER_CMD_ARG_MAX];
    char cand[SXCL_LOADER_CMD_ARG_MAX];
    if (req->game_dir && req->game_dir[0] && req->base_version && req->base_version[0] &&
        snprintf(leaf, sizeof(leaf), "%s.jar", req->base_version) > 0 &&
        join_path(dir, sizeof(dir), req->game_dir, "versions") == 0 &&
        join_path(dir, sizeof(dir), dir, req->base_version) == 0 &&
        join_path(cand, sizeof(cand), dir, leaf) == 0 && sxcl_fs_exists(cand)) {
        (void)snprintf(out, cap, "%s", cand);
        return 0;
    }
    if (instance_dir && instance_dir[0] && req->instance_name && req->instance_name[0] &&
        snprintf(leaf, sizeof(leaf), "%s.jar", req->instance_name) > 0 &&
        join_path(cand, sizeof(cand), instance_dir, leaf) == 0 && sxcl_fs_exists(cand)) {
        (void)snprintf(out, cap, "%s", cand);
        return 0;
    }
    return -1;
}

/** 重放 processors[]。返回 1 = 全跑完（或本来就没有），0 = 失败/取消（原因已进 ctx）。 */
static int run_processors(install_ctx *ctx, const sxcl_json *profile, const char *instance_dir)
{
    const sxcl_loader_install_request *req = ctx->req;
    const size_t total = sxcl_processor_count(profile);
    if (total == 0) {
        return 1;   /* 1.12 及以前没有 processors：这条路本来就是空的 */
    }

    if (make_temp_root(ctx->temp_dir, sizeof(ctx->temp_dir), "processors") != 0) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "建不了处理器的临时目录");
        return 0;
    }

    char mc_jar[SXCL_LOADER_CMD_ARG_MAX];
    if (find_minecraft_jar(req, instance_dir, mc_jar, sizeof(mc_jar)) != 0) {
        /* 没有原版 jar 就别硬跑（jarsplitter 第一步就要它）——如实报，别让用户看一句
         * "处理器失败"去猜。 */
        ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT,
                 "找不到原版客户端 jar（versions/%s/%s.jar），处理器没法跑",
                 req->base_version ? req->base_version : "?", req->base_version ? req->base_version : "?");
        return 0;
    }

    sxcl_processor_ctx pctx;
    memset(&pctx, 0, sizeof(pctx));
    pctx.game_dir = req->game_dir;
    pctx.installer_jar = req->installer_jar;
    pctx.minecraft_jar = mc_jar;
    pctx.minecraft_version = req->base_version;
    pctx.temp_dir = ctx->temp_dir;
    pctx.java_path = req->java_path;
    pctx.timeout_ms = effective_timeout_ms(req);
    pctx.take_file = processor_take_file;
    pctx.run = processor_run;
    pctx.is_cancelled = processor_is_cancelled;
    pctx.report = processor_report;
    pctx.ud = ctx;

    /* 处理器要用的 jar/classpath 是上一步 on_libraries 下的；实测它们就躺在 libraries/ 里。 */
    ctx_report(ctx, ctx->percent, "重放安装器处理器（%d 条，client 侧）…", (int)total);
    char perr[SXCL_PROCESSOR_ERR_MAX];
    perr[0] = '\0';
    sxcl_processor_stats pstats;
    const int rc = sxcl_processors_run(profile, "client", &pctx, &pstats, perr, sizeof(perr));
    if (rc == 2 || ctx->cancelled) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_CANCELLED, "用户取消了安装（处理器重放中途）");
        return 0;
    }
    if (rc != 0) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "安装器处理器没跑成：%s",
                 perr[0] ? perr : "原因不明");
        return 0;
    }
    ctx_report(ctx, ctx->percent, "处理器重放完成（跑了 %d 条，产物已就绪跳过 %d 条）",
               pstats.ran, pstats.skipped);
    return 1;
}

static int extract_install(install_ctx *ctx, const char *instance_dir)
{
    const sxcl_loader_install_request *req = ctx->req;
    if (req->kind != SXCL_LOADER_FORGE && req->kind != SXCL_LOADER_NEOFORGE) {
        return 0;   /* Fabric/OptiFine 没有这条路 */
    }
    sxcl_zip *zip = sxcl_zip_open(req->installer_jar);
    if (!zip) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "打不开安装器 jar：%s", req->installer_jar);
        return 0;
    }

    int ok = 0;
    char *profile_text = read_zip_text(zip, "install_profile.json");
    sxcl_json *profile = NULL;
    if (profile_text) {
        char perr[192];
        perr[0] = '\0';
        profile = sxcl_json_parse(profile_text, strlen(profile_text), perr, sizeof(perr));
    }
    const sxcl_json_value *profile_root = profile ? sxcl_json_root(profile) : NULL;
    const sxcl_json_value *install = profile_root ? sxcl_json_get(profile_root, "install") : NULL;
    const sxcl_json_value *version_root = NULL;
    sxcl_json *version_doc = NULL;

    /* 版本信息三个来源,顺序与 Python 一致:
     *   1) install_profile.json 的 versionInfo(Forge <= 1.12 的老格式)
     *   2) jar 根目录的 version.json(Forge 1.13+ / NeoForge)
     *   3) install_profile.json 的 json 字段指向的条目 */
    const sxcl_json_value *embedded = profile_root ? sxcl_json_get(profile_root, "versionInfo") : NULL;
    if (embedded && sxcl_json_type_of(embedded) == SXCL_JSON_OBJECT && sxcl_json_member_count(embedded) > 0) {
        version_root = embedded;
    } else {
        char *text = read_zip_text(zip, "version.json");
        if (!text && profile_root) {
            const char *entry = sxcl_json_get_string(profile_root, "json", "");
            if (entry[0]) {
                char clean[SXCL_LOADER_CMD_ARG_MAX];
                strip_leading_slashes(entry, clean, sizeof(clean));
                text = read_zip_text(zip, clean);
            }
        }
        if (text) {
            char verr[192];
            verr[0] = '\0';
            version_doc = sxcl_json_parse(text, strlen(text), verr, sizeof(verr));
            free(text);
            version_root = version_doc ? sxcl_json_root(version_doc) : NULL;
        }
    }
    if (!version_root || sxcl_json_type_of(version_root) != SXCL_JSON_OBJECT) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "安装器里没有 version.json / versionInfo，解包安装做不了");
        goto done;
    }

    /* 老格式:通用 jar 在 install.filePath 里,要落到 install.path 指的位置。 */
    if (install) {
        const char *rel = sxcl_json_get_string(install, "path", "");
        const char *entry = sxcl_json_get_string(install, "filePath", "");
        if (rel[0] && entry[0]) {
            char clean[SXCL_LOADER_CMD_ARG_MAX];
            strip_leading_slashes(rel, clean, sizeof(clean));
            if (clean[0]) {
                (void)extract_to_libraries(zip, entry, clean, req->game_dir);
            }
        }
    }

    /* 安装器自带 maven/ 目录的一并解包进 libraries/。 */
    {
        const size_t total = sxcl_zip_count(zip);
        for (size_t i = 0; i < total; ++i) {
            const char *name = sxcl_zip_name_at(zip, i);
            if (!name || strncmp(name, "maven/", 6) != 0) {
                continue;
            }
            const size_t len = strlen(name);
            if (len == 6 || name[len - 1] == '/') {
                continue;
            }
            (void)extract_to_libraries(zip, name, name + 6, req->game_dir);
        }
    }

    /* 依赖库清单交给调用方(启动器的下载引擎,见 sxcl/engine.h 的用法说明)。
     *
     * **两份清单都要收**(FCL 也是两个来源:GameLibrariesTask(profile.getLibraries()) 与
     * checkLibraryCompletion(forgeVersion)):
     *   * 版本 JSON(version.json)—— 实例**运行**要用的库(fmlloader / JarJar / mixin …);
     *   * install_profile.json —— **安装期**要用的库(installertools / jarsplitter /
     *     binarypatcher / ForgeAutoRenamingTool / mcp_config …),processors 重放就靠它们,
     *     少一件就是"处理器起不来"。以前只收 version_doc,于是 1.13+ 的工具库一件都没下 ——
     *     真机上就是这么撞出来的(处理器 jar 不在)。
     * 老格式(1.12-,版本信息嵌在 install_profile.json 里)只有 profile 那份,
     * libraries 在 install 下面(collect_libraries 里的 _section 语义)。 */
    if (req->on_libraries) {
        sxcl_loader_library *libs =
            (sxcl_loader_library *)malloc(sizeof(sxcl_loader_library) * SXCL_LOADER_MAX_LIBRARIES);
        if (!libs) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "内存不足，列不出依赖库");
            goto done;
        }
        const char *maven = maven_for_kind(req->kind);
        size_t total = 0;
        if (version_doc) {
            total = sxcl_loader_collect_libraries(version_doc, maven, libs, SXCL_LOADER_MAX_LIBRARIES);
            if (total > SXCL_LOADER_MAX_LIBRARIES) {
                total = SXCL_LOADER_MAX_LIBRARIES;
            }
        }
        if (profile) {
            sxcl_loader_library *extra =
                (sxcl_loader_library *)malloc(sizeof(sxcl_loader_library) * SXCL_LOADER_MAX_LIBRARIES);
            if (!extra) {
                free(libs);
                ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "内存不足，列不出依赖库");
                goto done;
            }
            size_t extra_count = sxcl_loader_collect_libraries(profile, maven, extra,
                                                               SXCL_LOADER_MAX_LIBRARIES);
            if (extra_count > SXCL_LOADER_MAX_LIBRARIES) {
                extra_count = SXCL_LOADER_MAX_LIBRARIES;
            }
            for (size_t i = 0; i < extra_count && total < SXCL_LOADER_MAX_LIBRARIES; ++i) {
                int seen = 0;
                for (size_t k = 0; k < total; ++k) {
                    if (strcmp(libs[k].name, extra[i].name) == 0) {
                        seen = 1;
                        break;
                    }
                }
                if (!seen) {
                    libs[total++] = extra[i];   /* 整条拷:名字/路径/URL 三样都带着 */
                }
            }
            free(extra);
        }
        /* Python: 老格式里 install.libraries 那条 net.minecraftforge:forge:<版本> 没有分类器,
         * 拼出来在 maven 上不存在(实测 404),PCL 也是直接用 install.path 那份 —— 这里把它剔掉,
         * 因为通用 jar 已经在上面按 install.path 解包好了。 */
        if (install && sxcl_json_get_string(install, "path", "")[0]) {
            size_t kept = 0;
            for (size_t i = 0; i < total; ++i) {
                const char *coord = libs[i].name;
                const int legacy_loader =
                    count_colons(coord) == 2 &&
                    (strncmp(coord, "net.minecraftforge:forge:", 25) == 0 ||
                     strncmp(coord, "net.minecraftforge:fmlloader:", 29) == 0);
                if (!legacy_loader) {
                    libs[kept++] = libs[i];
                }
            }
            total = kept;
        }
        const int rc = total > 0 ? req->on_libraries(req->userdata, libs, total) : 0;
        free(libs);
        if (rc != 0) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "依赖库没有下齐（方式 B）");
            goto done;
        }
    }

    /* ── processors 重放（1.13+ Forge / NeoForge 的安装真身） ──
     * 位置就是 FCL 的位置：库下齐之后、写版本 JSON 之前。两条理由都不能挪：
     *   * 必须在 on_libraries 之后 —— 处理器的 jar 与 classpath 是上一步才落地的；
     *   * 必须在写 JSON 之前 —— 写 JSON 是"装完了"的标记（docs/22 的 B3/C4），
     *     处理器没跑成就是安装失败，绝不能留下一份"看着装好、其实没打补丁"的实例。 */
    ctx->zip = zip;
    const int proc_ok = run_processors(ctx, profile, instance_dir);
    ctx->zip = NULL;
    if (!proc_ok) {
        goto done;
    }

    copy_vanilla_client(req->game_dir, req->base_version, instance_dir, req->instance_name);
    /* 版本 JSON:id = 实例名,**并且拍平**(把原版合并进来)——
     * 用户点名:「PCL 与 HMCL 装出来的都是能独立启动的版本 JSON,我们肯定要学」。
     * 只写 inheritsFrom 的话,启动层不解析继承就会缺原版的库、assetIndex 还会退化成 legacy。
     *
     * **顺序(2026-09-22 改,见 docs/22 的 B3/C4)**:这一块是**最后一步**。
     * 以前是先写 JSON 再解包/下库 —— 于是"目录里有可解析的 JSON"在**装完之前**就成立了:
     * 中途失败(库没下齐、解包出错)会留下一份"看着装好了、其实缺库"的版本,而装前预检
     * (sxcl_install_target_probe)正是按"有没有可解析的 JSON"判重的 —— 用户会被自己上次的
     * 失败挡住,想重装还得先手动删目录。现在:解包 -> 下库 -> 复制原版 jar 全部成功之后才写
     * JSON,"有 JSON"真正等于"装完了"。 */
    {
        char leaf[SXCL_LOADER_CMD_ARG_MAX];
        char target[SXCL_LOADER_CMD_ARG_MAX];
        target[0] = '\0';
        const int wrote = snprintf(leaf, sizeof(leaf), "%s.json", req->instance_name);
        if (wrote <= 0 || (size_t)wrote >= sizeof(leaf) ||
            join_path(target, sizeof(target), instance_dir, leaf) != 0) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "版本 JSON 的落盘路径拼不出来（实例名 %s）",
                     req->instance_name);
            goto done;
        }

        const char *base_id =
            ctx->base_id[0] ? ctx->base_id : pick_base_id(version_root, req->base_version);
        char ferr[SXCL_LOADER_ERROR_MAX];
        ferr[0] = '\0';
        if (json_has_inherits(version_root)) {
            /* 靠继承的(1.13+ Forge / NeoForge):必须拍平,不然装出来的东西启动不了。
             * 合并基准优先用开工前读下的那份(那时候版本目录里还是原版 JSON);
             * 没有就现找一次(原版自己单独装过的机器上有)。 */
            sxcl_json *base_doc = ctx->base_doc;
            int owns_base = 0;
            if (!base_doc) {
                base_doc = load_base_doc(req->game_dir, base_id, instance_dir, req->instance_name);
                owns_base = 1;
            }
            const int frc = flatten_write_json(base_doc ? sxcl_json_root(base_doc) : NULL, base_id,
                                               req->instance_name, version_root, target, ferr,
                                               sizeof(ferr));
            if (owns_base) {
                sxcl_json_free(base_doc);
            }
            if (frc == SXCL_LOADER_OK) {
                ctx_report(ctx, ctx->percent, "版本 JSON 已拍平(合并原版 %s)", base_id);
            } else {
                /* 原版 JSON 不在(手动删过 / base_version 没给):退回继承式,**但如实说一声**。 */
                ctx_report(ctx, ctx->percent, "拍平不了(%s),先按继承式写",
                           ferr[0] ? ferr : "原因不明");
                if (write_inheriting_json(ctx, version_root, target, NULL) != 0) {
                    goto done;
                }
            }
        } else {
            /* 本来就是独立版本(老 Forge 的 versionInfo 是完整 JSON):只改 id 并补上
             * clientVersion(PCL 的拍平标记,实例扫描最优先看它)。 */
            if (write_inheriting_json(ctx, version_root, target, base_id) != 0) {
                goto done;
            }
        }
    }

    ok = dir_has_json(instance_dir);
    if (!ok) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "解包安装没有生成版本 JSON：%s", instance_dir);
    }

done:
    if (ctx->temp_dir[0]) {
        (void)remove_tree(ctx->temp_dir);   /* 里面是给处理器看的中间物（解出来的 lzma 等），不留 */
        ctx->temp_dir[0] = '\0';
    }
    free(profile_text);
    sxcl_json_free(profile);
    sxcl_json_free(version_doc);
    sxcl_zip_close(zip);
    return ok;
}

/* ── 方式 A ── */

static int run_installer_phase(install_ctx *ctx, const char *versions_dir, const char *instance_dir)
{
    const sxcl_loader_install_request *req = ctx->req;
    if (req->force_extract_install) {
        /* 用户/排查点名要"解包安装":别去试安装器 CLI,直接进方式 B。
         * (也是方式 B + processors 重放的端到端验收入口 —— CLI 的 --extract-install。) */
        ctx_report(ctx, ctx->percent, "按请求跳过安装器 CLI，直接解包安装");
        return 0;
    }
    sxcl_loader_cmd_env env;
    memset(&env, 0, sizeof(env));
    env.java_path = req->java_path;
    env.installer_jar = req->installer_jar;
    env.game_dir = req->game_dir;
    env.base_version = req->base_version;
    env.loader_version = req->loader_version;
    env.instance_name = req->instance_name;
    env.mirror_maven = req->mirror_maven;

    sxcl_loader_cmd cmds[4];
    const size_t count = sxcl_loader_build_commands(req->kind, &env, cmds, 4);
    if (count == 0) {
        return 0;   /* 这种加载器没有静默 CLI -> 直接走方式 B */
    }

    const int timeout = effective_timeout_ms(req);
    for (size_t i = 0; i < count; ++i) {
        if (ctx_cancelled(ctx)) {
            ctx->cancelled = 1;
            ctx_fail(ctx, SXCL_LOADER_FAIL_CANCELLED, "用户取消了安装");
            return 0;
        }
        ctx_report(ctx, ctx->percent, cmds[i].desc);
        sxcl_process_result pres;
        memset(&pres, 0, sizeof(pres));
        if (run_variant(ctx, &cmds[i], &pres) != 0) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_RUN_INSTALLER, "启动安装器失败：%s",
                     pres.error[0] ? pres.error : cmds[i].program);
            continue;
        }
        ctx->res->exit_code = pres.exit_code;
        if (ctx->cancelled) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_CANCELLED, "用户取消了安装");
            return 0;
        }
        if (pres.timed_out) {
            ctx->timed_out = 1;
            ctx_fail(ctx, SXCL_LOADER_FAIL_TIMEOUT, "安装器超过 %d 分钟没有完成", timeout / 60000);
            continue;
        }
        if (variant_succeeded(ctx, &pres, versions_dir, instance_dir)) {
            return 1;
        }
        const char *tail = ctx_last_line(ctx);
        if (ctx->finished) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_RUN_INSTALLER, "安装器说装完了，但 %s 里没有版本 JSON",
                     instance_dir);
        } else if (tail[0]) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_RUN_INSTALLER, "安装器没有成功（退出码 %d）：%.140s",
                     pres.exit_code, tail);
        } else {
            ctx_fail(ctx, SXCL_LOADER_FAIL_RUN_INSTALLER, "安装器没有成功（退出码 %d）", pres.exit_code);
        }
    }
    return 0;
}

/* ── OptiFine:临时 APPDATA 沙箱 ── */

typedef struct find_dir_state {
    char name[192];
    int found;
} find_dir_state;

static int find_optifine_cb(void *ud, const char *full, const char *name, int is_dir)
{
    (void)full;
    find_dir_state *state = (find_dir_state *)ud;
    if (is_dir && contains_ci(name, "optifine")) {
        (void)snprintf(state->name, sizeof(state->name), "%s", name);
        state->found = 1;
        return 1;
    }
    return 0;
}

static int install_optifine(install_ctx *ctx)
{
    const sxcl_loader_install_request *req = ctx->req;
    sxcl_loader_install_result *res = ctx->res;
    const char *mc = req->base_version;
    int ok = 0;
    int sandbox_ready = 0;

    char sandbox[SXCL_LOADER_CMD_ARG_MAX];
    char fake_appdata[SXCL_LOADER_CMD_ARG_MAX];
    char fake_game[SXCL_LOADER_CMD_ARG_MAX];
    char fake_versions[SXCL_LOADER_CMD_ARG_MAX];
    char fake_mc_dir[SXCL_LOADER_CMD_ARG_MAX];
    char real_versions[SXCL_LOADER_CMD_ARG_MAX];
    char instance_dir[SXCL_LOADER_CMD_ARG_MAX];
    char real_libraries[SXCL_LOADER_CMD_ARG_MAX];

    res->used_sandbox = 1;
    if (make_temp_root(sandbox, sizeof(sandbox), "optifine") != 0) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_PREPARE, "建不了临时沙箱目录（%s）", sandbox);
        return 0;
    }
    sandbox_ready = 1;
    if (join_path(fake_appdata, sizeof(fake_appdata), sandbox, "appdata") != 0 ||
        join_path(fake_game, sizeof(fake_game), fake_appdata, ".minecraft") != 0 ||
        join_path(fake_versions, sizeof(fake_versions), fake_game, "versions") != 0 ||
        join_path(fake_mc_dir, sizeof(fake_mc_dir), fake_versions, mc) != 0 ||
        join_path(real_versions, sizeof(real_versions), req->game_dir, "versions") != 0 ||
        join_path(instance_dir, sizeof(instance_dir), real_versions, req->instance_name) != 0 ||
        join_path(real_libraries, sizeof(real_libraries), req->game_dir, "libraries") != 0) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_PREPARE, "路径太长，拼不出沙箱目录");
        goto done;
    }
    if (sxcl_fs_mkdirs(fake_mc_dir) != 0) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_PREPARE, "建不了沙箱游戏目录：%s", fake_mc_dir);
        goto done;
    }

    /* 摆好原版版本文件:OptiFine 要基于原版的版本 JSON 生成自己的版本。
     * Python: 缺原版 <mc>.json 直接判失败,别让它装出一份不完整的版本。 */
    {
        char leaf[SXCL_LOADER_CMD_ARG_MAX];
        char src[SXCL_LOADER_CMD_ARG_MAX];
        char target[SXCL_LOADER_CMD_ARG_MAX];
        char real_mc_dir[SXCL_LOADER_CMD_ARG_MAX];
        if (join_path(real_mc_dir, sizeof(real_mc_dir), real_versions, mc) != 0) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_PREPARE, "路径太长，拼不出原版目录");
            goto done;
        }
        (void)snprintf(leaf, sizeof(leaf), "%s.json", mc);
        if (join_path(src, sizeof(src), real_mc_dir, leaf) != 0 ||
            join_path(target, sizeof(target), fake_mc_dir, leaf) != 0) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_PREPARE, "路径太长，拼不出原版版本 JSON");
            goto done;
        }
        if (!sxcl_fs_exists(src)) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_PREPARE, "缺少原版 %s 的版本 JSON，先装原版再装 OptiFine", mc);
            goto done;
        }
        (void)copy_file(src, target, 0);
        (void)snprintf(leaf, sizeof(leaf), "%s.jar", mc);
        if (join_path(src, sizeof(src), real_mc_dir, leaf) == 0 &&
            join_path(target, sizeof(target), fake_mc_dir, leaf) == 0 && sxcl_fs_exists(src)) {
            (void)copy_file(src, target, 0);
        }
    }

    /* 沙箱里也得有 launcher_profiles.json(安装器的硬性前置)。 */
    {
        char perr[192];
        perr[0] = '\0';
        int changed = 0;
        if (sxcl_loader_ensure_launcher_profiles(fake_game, NULL, NULL, NULL, NULL, &changed, perr,
                                                 sizeof(perr)) != SXCL_LOADER_OK) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_PREPARE, "沙箱里补不了 launcher_profiles.json：%s", perr);
            goto done;
        }
    }

    ctx_report(ctx, 20, "OptiFine 正在安装（静默，不打图形界面）");
    {
        sxcl_loader_cmd_env env;
        memset(&env, 0, sizeof(env));
        env.java_path = req->java_path;
        env.installer_jar = req->installer_jar;
        env.game_dir = fake_game;
        env.fake_appdata = fake_appdata;
        env.base_version = mc;
        sxcl_loader_cmd cmds[1];
        if (sxcl_loader_build_commands(SXCL_LOADER_OPTIFINE, &env, cmds, 1) != 1) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_RUN_INSTALLER, "拼不出 OptiFine 安装器命令行");
            goto done;
        }
        /* 给它两次机会:
         *   第 1 次:APPDATA 只走 process.h 的 env(首选通道,不动全局状态);
         *   第 2 次:沙箱里什么都没生成 -> 这个进程后端八成不认 env,才退回"临时改本进程
         *           APPDATA"的兜底方式重试一次。
         * 两次都不行就如实失败,不做无谓的第三次。 */
        find_dir_state found;
        memset(&found, 0, sizeof(found));
        for (int attempt = 0; attempt < 2 && !found.found; ++attempt) {
            if (attempt > 0) {
                ctx->process_env_fallback = 1;
                ctx_report(ctx, ctx->percent, "OptiFine 没在沙箱里生成版本，改用进程环境变量兜底重试");
            }
            sxcl_process_result pres;
            memset(&pres, 0, sizeof(pres));
            if (run_variant(ctx, &cmds[0], &pres) != 0) {
                ctx_fail(ctx, SXCL_LOADER_FAIL_RUN_INSTALLER, "启动 OptiFine 安装器失败：%s",
                         pres.error[0] ? pres.error : req->java_path);
                goto done;
            }
            res->exit_code = pres.exit_code;
            if (ctx->cancelled) {
                ctx_fail(ctx, SXCL_LOADER_FAIL_CANCELLED, "用户取消了安装");
                goto done;
            }
            if (pres.timed_out) {
                ctx->timed_out = 1;
                ctx_fail(ctx, SXCL_LOADER_FAIL_TIMEOUT, "OptiFine 安装器超时");
                goto done;
            }
            if (pres.exit_code != 0 && !ctx->finished) {
                const char *tail = ctx_last_line(ctx);
                if (tail[0]) {
                    ctx_fail(ctx, SXCL_LOADER_FAIL_RUN_INSTALLER, "OptiFine 安装器退出码 %d：%.140s",
                             pres.exit_code, tail);
                } else {
                    ctx_fail(ctx, SXCL_LOADER_FAIL_RUN_INSTALLER, "OptiFine 安装器退出码 %d",
                             pres.exit_code);
                }
            }
            /* 找它装出来的版本目录(沙箱里只可能有原版和我们刚给的那份,所以名字里带 optifine 的就是它)。
             * 找不到就说明它把东西装到别处去了(典型:没认我们给的 APPDATA)。 */
            (void)list_dir(fake_versions, find_optifine_cb, &found);
        }
        if (!found.found) {
            if (ctx->fail_stage == SXCL_LOADER_FAIL_NONE) {
                ctx_fail(ctx, SXCL_LOADER_FAIL_RUN_INSTALLER,
                         "OptiFine 没有生成版本目录（它可能没认我们给的 APPDATA）");
            }
            goto done;
        }
        char source_dir[SXCL_LOADER_CMD_ARG_MAX];
        if (join_path(source_dir, sizeof(source_dir), fake_versions, found.name) != 0) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_RUN_INSTALLER, "OptiFine 版本目录名太长");
            goto done;
        }
        (void)copy_dir_files(source_dir, instance_dir);
        /* 它写的库(optifine/OptiFine、launchwrapper-of…)也要搬,已有的不覆盖。 */
        char fake_libraries[SXCL_LOADER_CMD_ARG_MAX];
        if (join_path(fake_libraries, sizeof(fake_libraries), fake_game, "libraries") == 0) {
            (void)copy_tree_jars(fake_libraries, real_libraries);
        }
    }

    (void)normalize_instance(instance_dir, req->instance_name);
    ok = dir_has_json(instance_dir);
    if (!ok) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_VERIFY, "OptiFine 的版本目录里没有版本 JSON：%s", instance_dir);
    }

done:
    if (sandbox_ready && !req->keep_sandbox) {
        (void)remove_tree(sandbox);
    }
    return ok;
}

/* ── 主流程 ── */

const char *sxcl_loader_fail_stage_name(sxcl_loader_fail_stage stage)
{
    switch (stage) {
    case SXCL_LOADER_FAIL_NONE:       return "none";
    case SXCL_LOADER_FAIL_PREPARE:    return "prepare";
    case SXCL_LOADER_FAIL_RUN_INSTALLER: return "run_installer";
    case SXCL_LOADER_FAIL_EXTRACT:    return "extract";
    case SXCL_LOADER_FAIL_VERIFY:     return "verify";
    case SXCL_LOADER_FAIL_CANCELLED:  return "cancelled";
    case SXCL_LOADER_FAIL_TIMEOUT:    return "timeout";
    default:                          return "unknown";
    }
}

static int path_is_absolute(const char *path)
{
    if (!path || !path[0]) {
        return 0;
    }
    if (path[0] == '/' || path[0] == '\\') {
        return 1;
    }
#if defined(_WIN32)
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
        return 1;
    }
#endif
    return 0;
}

/* 相对路径 -> 绝对路径(按本进程当前工作目录)。已经是绝对路径、或者取不到 cwd 时原样拷贝。
 *
 * 为什么非做不可:子进程的工作目录被我们设成了游戏目录,而 Java 解析 "-jar <路径>" 是相对
 * **它自己的**工作目录 —— 真跑 Forge 1.20.1 时就是这条把方式 A 的四组命令行全打挂了:
 * 它去找 <游戏目录>/build/e2e/mc/loaders/forge-....jar,直接 "Unable to access jarfile" 退出 1。 */
static void make_absolute(char *out, size_t cap, const char *path)
{
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!path || !path[0] || path_is_absolute(path)) {
        (void)snprintf(out, cap, "%s", path ? path : "");
        return;
    }
    char cwd[SXCL_LOADER_CMD_ARG_MAX];
#if defined(_WIN32)
    if (!_getcwd(cwd, (int)sizeof(cwd))) {
        (void)snprintf(out, cap, "%s", path);
        return;
    }
#else
    if (!getcwd(cwd, sizeof(cwd))) {
        (void)snprintf(out, cap, "%s", path);
        return;
    }
#endif
    if (join_path(out, cap, cwd, path) != 0) {
        (void)snprintf(out, cap, "%s", path);
    }
}

static int install_arg_error(sxcl_loader_install_result *out, const char *message)
{
    if (out) {
        out->ok = 0;
        (void)snprintf(out->error, sizeof(out->error), "%s", message);
    }
    return SXCL_LOADER_ERR_ARG;
}

int sxcl_loader_install(const sxcl_loader_install_request *req, sxcl_loader_install_result *out)
{
    if (!out) {
        return SXCL_LOADER_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;
    out->fail_stage = SXCL_LOADER_FAIL_NONE;
    if (!req) {
        return install_arg_error(out, "缺少安装请求");
    }
    if (!req->game_dir || !req->game_dir[0]) {
        return install_arg_error(out, "缺少游戏目录");
    }
    if (!req->instance_name || !req->instance_name[0]) {
        return install_arg_error(out, "缺少实例名（版本目录名）");
    }
    if (!req->base_version || !req->base_version[0]) {
        return install_arg_error(out, "缺少原版版本号");
    }
    if (!req->java_path || !req->java_path[0]) {
        return install_arg_error(out, "未找到 Java 运行时");
    }
    if (!req->installer_jar || !req->installer_jar[0]) {
        return install_arg_error(out, "缺少安装器 jar");
    }
    if (req->kind == SXCL_LOADER_VANILLA) {
        return install_arg_error(out, "原版不需要装加载器");
    }
    if (!sxcl_loader_kind_implemented(req->kind)) {
        return install_arg_error(out, "还不支持安装该加载器（只有 Forge / NeoForge / Fabric / OptiFine 有静默实现）");
    }

    /* 游戏目录 / 安装器 jar / 带分隔符的 java 路径一律转绝对路径 —— 子进程的工作目录是游戏目录,
     * 相对路径会被它按自己的工作目录重新解析(见 make_absolute 的注释)。 */
    sxcl_loader_install_request local;
    char game_abs[SXCL_LOADER_CMD_ARG_MAX];
    char installer_abs[SXCL_LOADER_CMD_ARG_MAX];
    char java_abs[SXCL_LOADER_CMD_ARG_MAX];
    memcpy(&local, req, sizeof(local));
    make_absolute(game_abs, sizeof(game_abs), req->game_dir);
    make_absolute(installer_abs, sizeof(installer_abs), req->installer_jar);
    if (strchr(req->java_path, '/') || strchr(req->java_path, '\\')) {
        make_absolute(java_abs, sizeof(java_abs), req->java_path);
    } else {
        (void)snprintf(java_abs, sizeof(java_abs), "%s", req->java_path);   /* 裸名字走 PATH,别动它 */
    }
    local.game_dir = game_abs;
    local.installer_jar = installer_abs;
    local.java_path = java_abs;
    req = &local;

    install_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.req = req;
    ctx.res = out;
    ctx.fail_stage = SXCL_LOADER_FAIL_NONE;

    const char *name = sxcl_loader_kind_name(req->kind);
    char versions_dir[SXCL_LOADER_CMD_ARG_MAX];
    char instance_dir[SXCL_LOADER_CMD_ARG_MAX];
    if (join_path(versions_dir, sizeof(versions_dir), req->game_dir, "versions") != 0 ||
        join_path(instance_dir, sizeof(instance_dir), versions_dir, req->instance_name) != 0) {
        return install_arg_error(out, "路径太长，拼不出版本目录");
    }
    (void)snprintf(out->version_dir, sizeof(out->version_dir), "%s", instance_dir);

    /* 拍平的合并基准要**在安装器开工前**读:引擎刚从清单把原版 JSON 落到
     * versions/<实例>/<实例>.json(install.c 的 version_json 阶段),安装器随后会覆盖它。
     * 读到了就顺带把原版版本号取出来(JSON 里的 id 比调用方填的 base_version 更准)。 */
    ctx.base_doc = load_base_doc(req->game_dir, req->base_version, instance_dir, req->instance_name);
    (void)snprintf(ctx.base_id, sizeof(ctx.base_id), "%s", req->base_version ? req->base_version : "");
    if (ctx.base_doc) {
        const char *own = sxcl_json_get_string(sxcl_json_root(ctx.base_doc), "id", NULL);
        if (own && own[0]) {
            (void)snprintf(ctx.base_id, sizeof(ctx.base_id), "%s", own);
        }
        /* 把"引擎刚放下的原版 JSON"从版本目录里**挪走**(内容已经在上面的 ctx.base_doc 里):
         *  1) 否则 variant_succeeded 的 dir_has_json 会被它骗到 —— 安装器什么都不干也算"安装成功",
         *     实测就是这样:装完版本 JSON 还是原版那份,mainClass 还是 net.minecraft.client.main.Main;
         *  2) 否则安装器写出来的 JSON 会和它抢同一个文件名(normalize 把每份 JSON 都改成 <实例名>.json),
         *     谁赢要看目录顺序 —— 那是不确定的;
         *  3) 装失败时目录里不留"看着像装好了"的东西。
         * 原版 JSON 本来就在游戏目录里随用随下(下次装会重新落一份),删掉不影响任何人。 */
        drop_stale_vanilla(instance_dir, req->instance_name);
    }

    int ok = 0;
    if (!sxcl_fs_exists(req->installer_jar)) {
        ctx_fail(&ctx, SXCL_LOADER_FAIL_PREPARE, "安装器 jar 不在：%s", req->installer_jar);
        goto finish;
    }

    ctx_report(&ctx, 0, "执行 %s 安装器", name);

    if (req->kind == SXCL_LOADER_OPTIFINE) {
        ok = install_optifine(&ctx);
    } else {
        /* 1) 安装器的硬性前置:launcher_profiles.json 必须在(只合并不覆盖)。 */
        char perr[192];
        perr[0] = '\0';
        int changed = 0;
        if (sxcl_loader_ensure_launcher_profiles(req->game_dir, NULL, NULL, NULL, NULL, &changed,
                                                 perr, sizeof(perr)) != SXCL_LOADER_OK) {
            ctx_fail(&ctx, SXCL_LOADER_FAIL_PREPARE, "补不了 launcher_profiles.json：%s",
                     perr[0] ? perr : "写不进去");
            goto finish;
        }
        if (perr[0]) {
            ctx_report(&ctx, ctx.percent, perr);   /* 非致命:文件还在,继续装 */
        }
        /* 2) 方式 A。 */
        ok = run_installer_phase(&ctx, versions_dir, instance_dir);
        /* 3) 方式 B(回退)。 */
        if (!ok && !ctx.cancelled) {
            if (!req->no_fallback && (req->kind == SXCL_LOADER_FORGE || req->kind == SXCL_LOADER_NEOFORGE)) {
                ctx_report(&ctx, FALLBACK_PERCENT, "改用解包安装 %s", name);
                out->used_fallback = 1;
                ok = extract_install(&ctx, instance_dir);
                if (!ok && ctx.fail_stage == SXCL_LOADER_FAIL_NONE) {
                    ctx.fail_stage = SXCL_LOADER_FAIL_EXTRACT;
                }
            } else if (ctx.fail_stage == SXCL_LOADER_FAIL_NONE) {
                ctx.fail_stage = SXCL_LOADER_FAIL_RUN_INSTALLER;
            }
        }
    }

    if (ok) {
        ctx_report(&ctx, POST_PERCENT, "整理 %s 文件", name);
        (void)normalize_instance(instance_dir, req->instance_name);
        /* 方式 A 的产物是**安装器自己写的版本 JSON**(Fabric / Forge 1.13+ 带 inheritsFrom):
         * 同样拍平一次,让两条路产出同一个形态 —— 能独立启动。已经是独立版本的会被跳过。 */
        {
            char note[SXCL_LOADER_TEXT_MAX];
            note[0] = '\0';
            if (flatten_instance_on_disk(&ctx, instance_dir, req->instance_name, note,
                                         sizeof(note)) == SXCL_LOADER_OK) {
                if (note[0]) {
                    ctx_report(&ctx, ctx.percent, "版本 JSON：%s", note);
                }
            } else {
                ctx_report(&ctx, ctx.percent, "版本 JSON 没能拍平：%s", note[0] ? note : "原因不明");
            }
        }
        if (!dir_has_json(instance_dir)) {
            ok = 0;
            ctx_fail(&ctx, SXCL_LOADER_FAIL_VERIFY, "版本目录里没有版本 JSON：%s", instance_dir);
        } else {
            /* 装完核对依赖库(docs/22 的 B9):"有版本 JSON"不等于"库都下齐了"。
             * **不判失败**(启动前的"补全文件"会补上),但必须报出来 —— 以前要到启动崩了才知道。 */
            char first_missing[SXCL_LOADER_CMD_ARG_MAX];
            const int missing = sxcl_loader_count_missing_libraries(
                req->game_dir, instance_dir, req->instance_name, first_missing,
                sizeof(first_missing));
            out->missing_libraries = missing;
            if (missing > 0) {
                ctx_report(&ctx, ctx.percent,
                           "依赖库没下齐：缺 %d 件（首个 %s）—— 启动前会自动补上",
                           missing, first_missing);
            } else {
                ctx_report(&ctx, ctx.percent, "依赖库已核对：版本 JSON 里的库都在磁盘上");
            }
        }
    }

    /* 装完把实例登记进 launcher_profiles.json:启动器的版本列表才看得到它。
     * 用同一个只合并不覆盖的入口(key = 实例名),原有档案与未知字段一个都不动。
     * 这一步失败不算安装失败(版本本身已经就位),只在进度里说一声。 */
    if (ok) {
        char perr[192];
        perr[0] = '\0';
        int changed = 0;
        const int prc = sxcl_loader_ensure_launcher_profiles(req->game_dir, req->instance_name,
                                                            req->instance_name, req->instance_name,
                                                            NULL, &changed, perr, sizeof(perr));
        if (prc != SXCL_LOADER_OK) {
            ctx_report(&ctx, ctx.percent, "实例没能写进 launcher_profiles.json：%s",
                       perr[0] ? perr : "写不进去");
        } else if (perr[0]) {
            ctx_report(&ctx, ctx.percent, perr);
        }
    }

finish:
    sxcl_json_free(ctx.base_doc);
    ctx.base_doc = NULL;
    if (ok) {
        out->ok = 1;
        out->fail_stage = SXCL_LOADER_FAIL_NONE;
        ctx_report(&ctx, 100, "%s 安装完成", name);
        return SXCL_LOADER_INSTALL_OK;
    }

    if (ctx.cancelled) {
        ctx.fail_stage = SXCL_LOADER_FAIL_CANCELLED;
        if (!ctx.last_error[0]) {
            (void)snprintf(ctx.last_error, sizeof(ctx.last_error), "用户取消了安装");
        }
    } else if (ctx.fail_stage == SXCL_LOADER_FAIL_NONE) {
        ctx.fail_stage = ctx.timed_out ? SXCL_LOADER_FAIL_TIMEOUT : SXCL_LOADER_FAIL_RUN_INSTALLER;
    }
    if (!ctx.last_error[0]) {
        (void)snprintf(ctx.last_error, sizeof(ctx.last_error), "安装器没有给出可用信息");
    }
    out->ok = 0;
    out->fail_stage = ctx.fail_stage;
    (void)snprintf(out->error, sizeof(out->error), "%s", ctx.last_error);
    ctx_report(&ctx, 100, "%s 安装失败：%s", name, out->error);
    return SXCL_LOADER_INSTALL_FAILED;
}
