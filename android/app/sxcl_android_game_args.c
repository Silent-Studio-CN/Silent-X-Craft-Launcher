/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* sxcl_android_game_args.c - 见头文件。规则都在核心库,这里只有安卓布局与"把整条命令行传下去"。 */

#include "sxcl_android_game_args.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/instance.h"
#include "sxcl/natives.h"

static sxcl_android_game_log_fn g_log_fn = NULL;
static void *g_log_ud = NULL;

void sxcl_android_game_set_log(sxcl_android_game_log_fn fn, void *ud)
{
    g_log_fn = fn;
    g_log_ud = ud;
}

static void plan_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    if (g_log_fn == NULL)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log_fn(g_log_ud, buf);
}

static void set_note(sxcl_android_game_plan *plan, const char *fmt, ...)
{
    va_list ap;
    if (plan == NULL)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(plan->note, sizeof(plan->note), fmt, ap);
    va_end(ap);
}

static void set_err(char *err, size_t err_len, const char *fmt, ...)
{
    va_list ap;
    if (err == NULL || err_len == 0)
        return;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* 从核心库拼出来的命令行里把 -cp 的值 / 主类抽出来(只为取证与给 jvm 层对账;
 * 真正的启动用的是**整条**命令行)。 */
static void extract_evidence(sxcl_android_game_plan *plan)
{
    const char *const *av = plan->argv;
    int i = 0;
    plan->classpath[0] = '\0';
    plan->main_class[0] = '\0';
    if (av == NULL)
        return;
    for (i = 0; av[i] != NULL; ++i) {
        if ((strcmp(av[i], "-cp") == 0 || strcmp(av[i], "-classpath") == 0) && av[i + 1] != NULL) {
            (void)snprintf(plan->classpath, sizeof(plan->classpath), "%s", av[i + 1]);
            ++i;
            continue;
        }
        /* 主类:第一条**不以 '-' 开头**、且不是 -cp 取值、且后面还有游戏参数的
         * (版本 JSON 的 mainClass);找不到就留空。 */
        if (av[i][0] != '-' && plan->main_class[0] == '\0' && strstr(av[i], ".jar") == NULL &&
            av[i + 1] != NULL) {
            (void)snprintf(plan->main_class, sizeof(plan->main_class), "%s", av[i]);
        }
    }
}

int sxcl_android_game_plan_build(const sxcl_android_game_spec *spec, sxcl_android_game_plan *out,
                                 char *err, size_t err_len)
{
    sxcl_launch_ctx ctx;
    char berr[256];
    int rc = 0;

    if (out == NULL) {
        set_err(err, err_len, "缺少输出结构");
        return -1;
    }
    (void)memset(out, 0, sizeof(*out));
    out->natives_count = 0;
    if (err != NULL && err_len > 0)
        err[0] = '\0';

    if (spec == NULL || spec->files_dir == NULL || spec->files_dir[0] == '\0' ||
        spec->game_dir == NULL || spec->game_dir[0] == '\0' || spec->instance_id == NULL ||
        spec->instance_id[0] == '\0') {
        set_err(err, err_len, "缺少 files_dir / game_dir / instance_id(安卓上 natives 落在应用私有目录)");
        return -2;
    }

    /* ── 1. natives 目录:**安卓布局** <files>/natives/<instance> ──
     * 为什么不用桌面那套 <game>/versions/<v>-natives:/storage 是 noexec 挂载,
     * 原生库放那儿一定加载不了;应用私有目录才在允许 dlopen 的位置(与 APK 的 lib 同域)。 */
    if ((int)snprintf(out->natives_dir, sizeof(out->natives_dir), "%s/natives/%s", spec->files_dir,
                      spec->instance_id) >= (int)sizeof(out->natives_dir)) {
        set_err(err, err_len, "路径太长,拼不出 natives 目录");
        return -3;
    }
    (void)snprintf(out->java_library_path, sizeof(out->java_library_path), "%s", out->natives_dir);
    if (sxcl_fs_mkdirs(out->natives_dir) != 0) {
        set_err(err, err_len, "建不了 natives 目录:%s", out->natives_dir);
        return -4;
    }

    /* ── 2. 版本 JSON(核心库读:同名优先,其次任意含 mainClass+type+id 的 *.json) ── */
    berr[0] = '\0';
    out->doc = sxcl_instance_read_json(spec->game_dir, spec->instance_id, out->json_path,
                                       sizeof(out->json_path), berr, sizeof(berr));
    if (out->doc == NULL) {
        set_err(err, err_len, "读版本 JSON 失败:%s", berr[0] != '\0' ? berr : "(无原因)");
        return -5;
    }
    plan_log("版本 JSON = %s", out->json_path);

    /* ── 3. inheritsFrom:核心库**不合并**(如实报缺口,不静默) ── */
    {
        const char *inherit = sxcl_json_get_string(sxcl_json_root(out->doc), "inheritsFrom", NULL);
        if (inherit != NULL && inherit[0] != '\0') {
            (void)snprintf(out->inherits_from, sizeof(out->inherits_from), "%s", inherit);
            set_note(out,
                     "这个实例的版本 JSON 带 inheritsFrom=%s;核心库目前**不合并**父子 JSON"
                     "(instance.c/args.c 都只吃单份),拼出来的 classpath 会缺父版本的 libraries 与 jar。"
                     "补的地方在核心库一处(桌面 driver.c 走的是同一条路,同样缺)",
                     out->inherits_from);
            plan_log("警告:inheritsFrom=%s 未合并(核心库缺口,见 note)", out->inherits_from);
        }
    }

    /* ── 4. 原生库:尽力而为(安卓上通常由 LWJGL/JRE 资产链另外提供) ── */
    berr[0] = '\0';
    rc = sxcl_natives_prepare_json(out->doc, spec->game_dir, out->natives_dir, berr, sizeof(berr));
    if (rc != 0) {
        plan_log("准备原生库失败(不当作致命:安卓的原生库一般由 LWJGL/JRE 资产链提供):%s",
                 berr[0] != '\0' ? berr : "(无原因)");
    }
    out->natives_count = sxcl_natives_last_count();
    plan_log("natives = %s(%d 个可用文件)", out->natives_dir, out->natives_count);

    /* ── 5. 拼 argv:全部交给核心库(sxcl_launch_build_args) ── */
    (void)memset(&ctx, 0, sizeof(ctx));
    {
        const int online = (spec->access_token != NULL && spec->access_token[0] != '\0');
        ctx.player_name = spec->player_name;
        ctx.uuid = spec->uuid;
        ctx.access_token = spec->access_token;
        ctx.user_type = spec->user_type != NULL && spec->user_type[0] != '\0'
                            ? spec->user_type
                            : (online ? "msa" : "legacy");
        ctx.xuid = spec->xuid;
        ctx.client_id = spec->client_id;
    }
    ctx.version_name = spec->instance_id;
    ctx.game_directory = spec->game_dir;
    ctx.natives_directory = out->natives_dir;
    ctx.memory_mb = spec->memory_mb;
    ctx.java_major = spec->java_major;
    ctx.is_64bit = spec->is_64bit;
    /* 平台:安卓在 rules 里按 linux 匹配、classpath 用 ':'(核心库 args.c 里就这么映射) */
    ctx.os = SXCL_LAUNCH_OS_ANDROID;
    ctx.extra_jvm_args = spec->extra_jvm_args;
    ctx.extra_game_args = spec->extra_game_args;

    berr[0] = '\0';
    out->args = sxcl_launch_build_args(out->doc, &ctx, berr, sizeof(berr));
    if (out->args == NULL) {
        set_err(err, err_len, "拼启动参数失败:%s", berr[0] != '\0' ? berr : "(无原因)");
        sxcl_android_game_plan_free(out);
        return -6;
    }
    out->argv = sxcl_launch_argv(out->args);
    out->arg_count = (int)sxcl_launch_arg_count(out->args);
    extract_evidence(out);
    plan_log("核心库拼出 %d 个参数(mainClass=%s,classpath %d 字符)", out->arg_count,
             out->main_class[0] != '\0' ? out->main_class : "(没找到)",
             (int)strlen(out->classpath));
    if (spec->renderer != NULL && spec->renderer[0] != '\0')
        plan_log("渲染器 = %s(只记日志;options.txt 由启动器在启动前写好)", spec->renderer);
    return 0;
}

void sxcl_android_game_plan_free(sxcl_android_game_plan *plan)
{
    if (plan == NULL)
        return;
    if (plan->args != NULL) {
        sxcl_launch_args_free(plan->args);
        plan->args = NULL;
    }
    if (plan->doc != NULL) {
        sxcl_json_free(plan->doc);
        plan->doc = NULL;
    }
    plan->argv = NULL;
}
