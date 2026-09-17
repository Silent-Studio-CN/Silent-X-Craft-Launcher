/* SXCL-C 模组加载器安装驱动 —— 方式 A(静默 CLI)+ 方式 B(解包)+ OptiFine 沙箱。
 *
 * 逐条对应 Python 版 src/services/mod_loader/installer.py 的裁决(注释里标了函数名):
 *
 *   1) 方式 A 只跑安装器自己声明的静默入口,参数名是 --installClient(见 sxcl_loader_build_commands);
 *      **绝不**构造"不带参数"的命令行 —— 那会弹图形安装器,用户关掉窗口同样返回 0,
 *      会被误判成安装成功(_build_command_variants 的注释)。
 *   2) 跑之前必须先有 <游戏目录>/launcher_profiles.json(_ensure_launcher_profiles)。
 *   3) 结束判据不是"退出码 0"也不是"jar 有没有生成",而是**产物**:versions/<实例名>/ 里
 *      真的有 .json(_version_ready);安装器写到别的目录名时按候选名找回来(_handle_generated_files)。
 *   4) 进度按安装器自己打印的标记推进(Extracting json / Downloading libraries /
 *      Building Processors / Task: xxx),看到完成标记就主动终止进程(process.h 的 on_line 回调
 *      返回非 0),别死等;另有总超时(默认 30 分钟)与取消。
 *   5) 方式 A 失败 -> 方式 B:只从 jar 里取 install_profile.json / version.json 拼版本 JSON,
 *      把安装器自带的 maven/ 与通用 jar 解包进 libraries/(_extract_install)。
 *   6) OptiFine 的安装器忽略我们传的目录,只认 %APPDATA%\.minecraft,所以给它一个临时
 *      APPDATA 沙箱、摆好原版版本文件,装完再把产物与库搬回实例目录(_install_optifine)。
 *
 * 关于 APPDATA 沙箱:首选通道就是 process.h 的 opts->env(地基两条腿都实现了:POSIX 用 putenv,
 * Windows 用"父环境 + 覆盖项 → 环境块 + CREATE_UNICODE_ENVIRONMENT")。只有当子进程明显没认环境块
 * (沙箱里什么都没生成)时,才退回"临时把本进程 APPDATA 指过去"的兜底方式重试一次 —— 这也是本模块
 * "同一时刻只许一个安装在跑"的原因。
 *
 * 另一条实测踩出来的坑(真跑 Forge 1.20.1 时抓到):子进程的工作目录是游戏目录,而命令行里的
 * -jar / --installClient 是相对路径时,会被**子进程**按它自己的工作目录去解析 ——
 * "java -jar build/x.jar" 会被它找成 <游戏目录>/build/x.jar,直接 "Unable to access jarfile" 退出 1。
 * 所以驱动在开跑前把游戏目录/安装器 jar/带分隔符的 java 路径一律转成绝对路径(见 make_absolute)。
 */
#define _CRT_SECURE_NO_WARNINGS 1

#include "sxcl/loader.h"

#include "sxcl/fs.h"
#include "sxcl/process.h"
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
        /* Python: _build_command_variants 的 Fabric 分支
         *   [client, --mcversion, <mc>, --loader, <lv>, --dir, <目录>, --name, <实例名>] */
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
            if (cmd_begin(&cmd, env, game_dir, variant == 0 ? "Fabric client --name" : "Fabric client") != 0 ||
                cmd_add_arg(&cmd, "client") != 0) {
                continue;
            }
            int ok = 1;
            if (base[0]) {
                ok = ok && cmd_add_arg(&cmd, "--mcversion") == 0 && cmd_add_arg(&cmd, base) == 0;
            }
            if (ok && loader[0]) {
                ok = ok && cmd_add_arg(&cmd, "--loader") == 0 && cmd_add_arg(&cmd, loader) == 0;
            }
            if (ok) {
                ok = ok && cmd_add_arg(&cmd, "--dir") == 0 && cmd_add_arg(&cmd, game_dir) == 0;
            }
            if (ok && variant == 0) {
                ok = ok && cmd_add_arg(&cmd, "--name") == 0 && cmd_add_arg(&cmd, instance) == 0;
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

static int make_sandbox_root(char *out, size_t cap)
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
    written = snprintf(out, cap, "%s/sxcl-optifine-%lu-%lu", base,
                       (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    free(base);
#else
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) {
        base = "/tmp";
    }
    written = snprintf(out, cap, "%s/sxcl-optifine-%ld-%ld", base, (long)getpid(), (long)time(NULL));
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
        /* 原始行:先给调用方(CLI 的 --verbose / 启动器的日志),再自己留一份尾巴。 */
        if (ctx->req->on_line) {
            ctx->req->on_line(ctx->req->userdata, is_stderr, line);
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

/* Python: _extract_install —— 不启动安装器进程,直接从 jar 里拼出版本 JSON(老 Forge 只能这么装)。 */
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

    /* 版本 JSON:id = 实例名;inheritsFrom 缺省 = 原版(Python 的 setdefault 语义)。 */
    {
        const sxcl_loader_json_override overrides[2] = {
            {"id", req->instance_name},
            {"inheritsFrom", req->base_version ? req->base_version : ""},
        };
        char derr[192];
        derr[0] = '\0';
        char *version_text = NULL;
        if (sxcl_loader_json_dump(version_root, overrides, 2, &version_text, derr, sizeof(derr)) !=
                SXCL_LOADER_OK || !version_text) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "版本 JSON 写不出来（%s）", derr[0] ? derr : "格式不对");
            free(version_text);
            goto done;
        }
        char leaf[SXCL_LOADER_CMD_ARG_MAX];
        char target[SXCL_LOADER_CMD_ARG_MAX];
        if (snprintf(leaf, sizeof(leaf), "%s.json", req->instance_name) <= 0 ||
            join_path(target, sizeof(target), instance_dir, leaf) != 0 ||
            write_text_atomic(target, version_text) != 0) {
            free(version_text);
            ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "版本 JSON 落盘失败：%s", target);
            goto done;
        }
        free(version_text);
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
     * 老格式(版本信息嵌在 install_profile.json 里)要从整份 profile 里找 ——
     * libraries 在 install 下面(collect_libraries 里的 _section 语义)。 */
    if (req->on_libraries) {
        sxcl_loader_library *libs =
            (sxcl_loader_library *)malloc(sizeof(sxcl_loader_library) * SXCL_LOADER_MAX_LIBRARIES);
        if (!libs) {
            ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "内存不足，列不出依赖库");
            goto done;
        }
        sxcl_json *collect_doc = version_doc ? version_doc : profile;
        size_t total = sxcl_loader_collect_libraries(collect_doc, maven_for_kind(req->kind), libs,
                                                     SXCL_LOADER_MAX_LIBRARIES);
        if (total > SXCL_LOADER_MAX_LIBRARIES) {
            total = SXCL_LOADER_MAX_LIBRARIES;
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

    copy_vanilla_client(req->game_dir, req->base_version, instance_dir, req->instance_name);
    ok = dir_has_json(instance_dir);
    if (!ok) {
        ctx_fail(ctx, SXCL_LOADER_FAIL_EXTRACT, "解包安装没有生成版本 JSON：%s", instance_dir);
    }

done:
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
    if (make_sandbox_root(sandbox, sizeof(sandbox)) != 0) {
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
        if (!dir_has_json(instance_dir)) {
            ok = 0;
            ctx_fail(&ctx, SXCL_LOADER_FAIL_VERIFY, "版本目录里没有版本 JSON：%s", instance_dir);
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
