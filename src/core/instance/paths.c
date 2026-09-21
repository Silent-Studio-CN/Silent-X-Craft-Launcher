/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1 /* getenv/snprintf 在 MSVC 下被标记不安全;C4996 在 /WX 下会打挂构建 */
#endif

#include "sxcl/paths.h"

#include "io_internal.h"
#include "sxcl/fs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#else
#  include <unistd.h>
#endif

#define SXCL_PATHS_TMP 2048

static void paths_copy(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    const size_t n = strlen(src);
    const size_t take = (n < cap - 1) ? n : cap - 1;
    (void)memcpy(dst, src, take);
    dst[take] = '\0';
}

static void paths_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (err == NULL || err_len == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* 拼候选路径;拼不下(超长)返回 0。 */
static int paths_join(char *out, size_t out_len, const char *base, const char *sub)
{
    if (base == NULL || base[0] == '\0') {
        return 0;
    }
    const int n = sxcl_dir_join(out, out_len, base, sub);
    return (n > 0 && (size_t)n < out_len) ? 1 : 0;
}

static const char *env_value(const char *name)
{
    const char *value = getenv(name);
    return (value != NULL && value[0] != '\0') ? value : NULL;
}

/* 用户主目录。拿到返回 0。 */
static int paths_home_dir(char *out, size_t out_len)
{
#if defined(_WIN32)
    const char *profile = env_value("USERPROFILE");
    if (profile != NULL) {
        paths_copy(out, out_len, profile);
        return 0;
    }
    const char *drive = env_value("HOMEDRIVE");
    const char *rest = env_value("HOMEPATH");
    if (drive != NULL && rest != NULL) {
        (void)snprintf(out, out_len, "%s%s", drive, rest);
        return 0;
    }
    return -1;
#else
    const char *home = env_value("HOME");
    if (home != NULL) {
        paths_copy(out, out_len, home);
        return 0;
    }
    return -1;
#endif
}

/* APPDATA(Windows 专有;其它平台没有). */
static int paths_appdata_dir(char *out, size_t out_len)
{
#if defined(_WIN32)
    const char *appdata = env_value("APPDATA");
    if (appdata != NULL) {
        paths_copy(out, out_len, appdata);
        return 0;
    }
    return -1;
#else
    (void)out;
    (void)out_len;
    return -1;
#endif
}

int sxcl_paths_program_dir(char *out, size_t out_len, char *err, size_t err_len)
{
    if (out == NULL || out_len == 0) {
        paths_err(err, err_len, "参数不合法");
        return SXCL_PATHS_ERR_ARG;
    }
#if defined(_WIN32)
    wchar_t wbuf[1024];
    const DWORD got = GetModuleFileNameW(NULL, wbuf, (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (got == 0 || got >= (DWORD)(sizeof(wbuf) / sizeof(wbuf[0]))) {
        paths_err(err, err_len, "拿不到启动器自己的路径(Windows API 失败)");
        return SXCL_PATHS_ERR_UNSUPPORTED;
    }
    char utf8[SXCL_PATHS_TMP];
    const int need = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int)got, utf8, (int)sizeof(utf8), NULL, NULL);
    if (need <= 0) {
        paths_err(err, err_len, "启动器路径转 UTF-8 失败");
        return SXCL_PATHS_ERR_UNSUPPORTED;
    }
    utf8[need] = '\0';
    for (int i = need - 1; i >= 0; --i) {
        if (utf8[i] == '\\' || utf8[i] == '/') {
            utf8[i] = '\0';
            break;
        }
    }
    if (utf8[0] == '\0') {
        paths_err(err, err_len, "启动器路径没有父目录");
        return SXCL_PATHS_ERR_UNSUPPORTED;
    }
    paths_copy(out, out_len, utf8);
    return SXCL_PATHS_OK;
#elif defined(__APPLE__)
    char buf[SXCL_PATHS_TMP];
    uint32_t size = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) {
        paths_err(err, err_len, "拿不到启动器自己的路径(_NSGetExecutablePath 失败)");
        return SXCL_PATHS_ERR_UNSUPPORTED;
    }
    for (size_t i = strlen(buf); i > 0; --i) {
        if (buf[i - 1] == '/') {
            buf[i - 1] = '\0';
            break;
        }
    }
    paths_copy(out, out_len, buf);
    return SXCL_PATHS_OK;
#else
    char buf[SXCL_PATHS_TMP];
    const ssize_t got = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (got <= 0) {
        paths_err(err, err_len, "拿不到启动器自己的路径(读 /proc/self/exe 失败)");
        return SXCL_PATHS_ERR_UNSUPPORTED;
    }
    buf[got] = '\0';
    for (ssize_t i = got - 1; i >= 0; --i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            break;
        }
    }
    paths_copy(out, out_len, buf);
    return SXCL_PATHS_OK;
#endif
}

int sxcl_paths_default_game_dir(char *out, size_t out_len, char *err, size_t err_len)
{
    if (out == NULL || out_len == 0) {
        paths_err(err, err_len, "参数不合法");
        return SXCL_PATHS_ERR_ARG;
    }
    out[0] = '\0';
#if defined(_WIN32)
    {
        char base[SXCL_PATHS_TMP];
        if (paths_appdata_dir(base, sizeof(base)) == 0) {
            if (!paths_join(out, out_len, base, ".minecraft")) {
                paths_err(err, err_len, "默认游戏目录路径太长");
                return SXCL_PATHS_ERR_SPACE;
            }
            return SXCL_PATHS_OK;
        }
        if (paths_home_dir(base, sizeof(base)) == 0) {
            if (!paths_join(out, out_len, base, ".minecraft")) {
                paths_err(err, err_len, "默认游戏目录路径太长");
                return SXCL_PATHS_ERR_SPACE;
            }
            return SXCL_PATHS_OK;
        }
        paths_err(err, err_len, "拿不到 APPDATA/USERPROFILE,拼不出默认游戏目录(请在设置里指定)");
        return SXCL_PATHS_ERR_UNSUPPORTED;
    }
#elif defined(__ANDROID__)
    {
        /* Android:APPDATA/USERPROFILE 都不存在,默认游戏目录只能落在应用自己的空间里。
         * 目录由安卓打包层通过环境变量 SXCL_ANDROID_FILES 传进来(Activity 的 getFilesDir(),
         * 即 /data/user/0/<pkg>/files)——**故意不用共享存储**:
         *   * 应用私有目录零权限即可读写,而 /sdcard 下的通用路径在 Android 11+ 需要
         *     MANAGE_EXTERNAL_STORAGE(要用户手动去系统设置里授权,不现实);
         *   * 卸载即清理,不会在别人机器上留垃圾。
         * getExternalFilesDir()(Android/data/<pkg>/files)同样免权限,只是卸载也会删;
         * 需要用户能用文件管理器看到时,打包层可以改成传它,核心层不用动。 */
        const char *base = env_value("SXCL_ANDROID_FILES");
        if (base != NULL && base[0] != '\0') {
            if (!paths_join(out, out_len, base, ".minecraft")) {
                paths_err(err, err_len, "默认游戏目录路径太长");
                return SXCL_PATHS_ERR_SPACE;
            }
            return SXCL_PATHS_OK;
        }
        paths_err(err, err_len,
                  "Android 上没拿到 SXCL_ANDROID_FILES(打包层入口应设为应用 files 目录)");
        return SXCL_PATHS_ERR_UNSUPPORTED;
    }
#elif defined(__APPLE__)
    {
        char home[SXCL_PATHS_TMP];
        if (paths_home_dir(home, sizeof(home)) != 0) {
            paths_err(err, err_len, "拿不到 HOME,拼不出默认游戏目录");
            return SXCL_PATHS_ERR_UNSUPPORTED;
        }
        if (!paths_join(out, out_len, home, "Library/Application Support/minecraft")) {
            paths_err(err, err_len, "默认游戏目录路径太长");
            return SXCL_PATHS_ERR_SPACE;
        }
        return SXCL_PATHS_OK;
    }
#else
    {
        char home[SXCL_PATHS_TMP];
        if (paths_home_dir(home, sizeof(home)) != 0) {
            paths_err(err, err_len, "拿不到 HOME,拼不出默认游戏目录");
            return SXCL_PATHS_ERR_UNSUPPORTED;
        }
        if (!paths_join(out, out_len, home, ".minecraft")) {
            paths_err(err, err_len, "默认游戏目录路径太长");
            return SXCL_PATHS_ERR_SPACE;
        }
        return SXCL_PATHS_OK;
    }
#endif
}

int sxcl_paths_describe(const sxcl_game_folder *folder, char *out, size_t out_len)
{
    if (folder == NULL || out == NULL || out_len == 0) {
        return SXCL_PATHS_ERR_ARG;
    }
    const char *label = (folder->label[0] != '\0') ? folder->label : "未知来源";
    int n;
    if (folder->exists) {
        n = snprintf(out, out_len, "%s（%s，%d 个版本）", folder->path, label, folder->versions);
    } else {
        n = snprintf(out, out_len, "%s（%s，目录不存在）", folder->path, label);
    }
    if (n < 0 || (size_t)n >= out_len) {
        return SXCL_PATHS_ERR_SPACE;
    }
    return SXCL_PATHS_OK;
}

int sxcl_paths_count_versions(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    char versions_dir[SXCL_PATHS_TMP];
    if (!paths_join(versions_dir, sizeof(versions_dir), path, "versions")) {
        return 0;
    }
    if (!sxcl_fs_is_dir(versions_dir)) {
        return 0;
    }
    sxcl_dir_list list;
    char err[SXCL_DIR_ERROR_MAX];
    if (sxcl_dir_list_open(versions_dir, &list, err, sizeof(err)) != 0) {
        return 0;
    }
    int count = 0;
    for (size_t i = 0; i < list.count; ++i) {
        if (!list.items[i].is_dir) {
            continue;
        }
        char sub[SXCL_PATHS_TMP];
        if (!paths_join(sub, sizeof(sub), versions_dir, list.items[i].name)) {
            continue;
        }
        char probe[SXCL_PATHS_TMP];
        (void)snprintf(probe, sizeof(probe), "%s.json", sub);
        if (sxcl_fs_exists(probe)) {
            ++count;
            continue;
        }
        (void)snprintf(probe, sizeof(probe), "%s.jar", sub);
        if (sxcl_fs_exists(probe)) {
            ++count;
            continue;
        }
        /* 加载器版本常常只有 JSON(jar 靠继承)—— 目录里只要有任意一个 *.json 就算一个 */
        sxcl_dir_list sub_list;
        if (sxcl_dir_list_open(sub, &sub_list, err, sizeof(err)) != 0) {
            continue;
        }
        for (size_t k = 0; k < sub_list.count; ++k) {
            if (sub_list.items[k].is_dir) {
                continue;
            }
            const size_t nlen = strlen(sub_list.items[k].name);
            if (nlen >= 5) {
                const char *suffix = sub_list.items[k].name + (nlen - 5);
                if (sxcl_dir_name_compare(suffix, ".json") == 0) {
                    ++count;
                    break;
                }
            }
        }
        sxcl_dir_list_free(&sub_list);
    }
    sxcl_dir_list_free(&list);
    return count;
}

int sxcl_paths_is_game_dir(const char *path)
{
    return (sxcl_paths_count_versions(path) > 0) ? 1 : 0;
}

int sxcl_paths_inspect_full(const char *path, const char *label, const char *owner,
                            sxcl_game_folder *out)
{
    if (path == NULL || path[0] == '\0' || out == NULL) {
        return SXCL_PATHS_ERR_ARG;
    }
    (void)memset(out, 0, sizeof(*out));
    paths_copy(out->path, sizeof(out->path), path);
    paths_copy(out->label, sizeof(out->label), (label != NULL && label[0] != '\0') ? label : "自定义目录");
    paths_copy(out->owner, sizeof(out->owner), (owner != NULL) ? owner : "");

    out->exists = sxcl_fs_is_dir(path) ? 1 : 0;
    if (out->exists) {
        out->versions = sxcl_paths_count_versions(path);
        char probe[SXCL_PATHS_TMP];
        /* Python: has_assets = (path/"assets").is_dir() */
        if (paths_join(probe, sizeof(probe), path, "assets") && sxcl_fs_is_dir(probe)) {
            out->has_assets = 1;
        }
        if (paths_join(probe, sizeof(probe), path, "launcher_profiles.json") && sxcl_fs_exists(probe)) {
            out->has_launcher_profiles = 1;
        }
    }
    /* Python 的 score:不存在 -1;有版本 > 有资源 > 有 launcher_profiles */
    if (!out->exists) {
        out->score = -1;
    } else {
        out->score = out->versions * 10 + (out->has_assets ? 5 : 0) + (out->has_launcher_profiles ? 2 : 0);
    }
    (void)sxcl_paths_describe(out, out->describe, sizeof(out->describe));
    return SXCL_PATHS_OK;
}

int sxcl_paths_inspect(const char *path, const char *label, sxcl_game_folder *out)
{
    return sxcl_paths_inspect_full(path, label, NULL, out);
}

int sxcl_paths_push(sxcl_game_folders *folders, const sxcl_game_folder *folder)
{
    if (folders == NULL || folder == NULL) {
        return SXCL_PATHS_ERR_ARG;
    }
    if (folders->count < SXCL_PATHS_MAX_CANDIDATES) {
        folders->items[folders->count] = *folder;
        ++folders->count;
        return SXCL_PATHS_OK;
    }
    /* 满了:丢最差的(分数最低);新来的更差就直接丢新的。两种都记 dropped。 */
    size_t worst = 0;
    for (size_t i = 1; i < folders->count; ++i) {
        if (folders->items[i].score < folders->items[worst].score) {
            worst = i;
        }
    }
    if (folder->score > folders->items[worst].score) {
        folders->items[worst] = *folder;
    }
    ++folders->dropped;
    return SXCL_PATHS_ERR_SPACE;
}

void sxcl_paths_sort(sxcl_game_folders *folders)
{
    if (folders == NULL) {
        return;
    }
    /* 稳定插入排序:**先比来源优先级**(已配置目录永远第一),再比分数;
     * 两者都相同就保持候选表原有先后(测试要能预期顺序)。 */
    for (size_t i = 1; i < folders->count; ++i) {
        sxcl_game_folder key = folders->items[i];
        size_t j = i;
        while (j > 0 && (folders->items[j - 1].priority < key.priority ||
                         (folders->items[j - 1].priority == key.priority &&
                          folders->items[j - 1].score < key.score))) {
            folders->items[j] = folders->items[j - 1];
            --j;
        }
        folders->items[j] = key;
    }
}

/* ══════════════════════ 候选根表(纯字符串,不碰文件系统) ══════════════════════
 *
 * 逐条来源(全部可注入 env,所以能在任意一台机器上验三个平台的规则):
 *   官方启动器   %APPDATA%\.minecraft / ~/Library/Application Support/minecraft / ~/.minecraft
 *   HMCL         HMCL 的数据目录与它默认的游戏目录(%APPDATA%\HMCL、~/Library/Application Support/HMCL、~/.minecraft)
 *                —— 依据 HMCL 自己的 GameRepositoryLayout(见 _ref/HMCL .../game/GameRepositoryLayout.java)
 *   MultiMC 家族 数据目录下的 instances/<名字>/{.minecraft,minecraft}
 *                (MultiMC / Prism Launcher;HMCL 也认这套 layout,所以单独列)
 *   CurseForge   数据目录下的 Instances/<名字>(首字母大写,实测 CurseForge App 就是这个)
 *   ATLauncher   数据目录下的 instances/<名字>
 *   FCL/Pojav    Android:FCL 默认 getExternalFilesDir()/.minecraft,也允许 /sdcard/FCL/.minecraft;
 *                PojavLauncher 在 games/PojavLauncher 与 Android/data/net.kdt.pojavlaunch/files 下
 *   共享存储     /sdcard/.minecraft 与 /storage/emulated/0/.minecraft(两者在多数机器上是同一处)
 *
 * 列表**不代表存在**:存在性/能不能读/像不像 MC 目录一律由探测层实测后如实标注。
 */

sxcl_paths_os sxcl_paths_current_os(void)
{
#if defined(__ANDROID__)
    return SXCL_PATHS_OS_ANDROID;
#elif defined(_WIN32)
    return SXCL_PATHS_OS_WINDOWS;
#elif defined(__APPLE__)
    return SXCL_PATHS_OS_MACOS;
#else
    return SXCL_PATHS_OS_LINUX;
#endif
}

const char *sxcl_paths_os_name(sxcl_paths_os os)
{
    switch (os) {
    case SXCL_PATHS_OS_WINDOWS: return "windows";
    case SXCL_PATHS_OS_MACOS:   return "macos";
    case SXCL_PATHS_OS_ANDROID: return "android";
    case SXCL_PATHS_OS_LINUX:
    default:                    return "linux";
    }
}

/** 拼候选路径:**一律用 '/'**(与 sxcl_dir_join 的工程约定一致 —— Windows 的文件 API 同时
 *  认 '/' 与 '\\',而混着写会让同一台机器上出现两种写法,去重就靠不住了)。 */
static int roots_join(char *out, size_t out_len, const char *base, const char *sub)
{
    if (out == NULL || out_len == 0 || base == NULL || base[0] == '\0') {
        return 0;
    }
    size_t n = strlen(base);
    while (n > 0 && (base[n - 1] == '/' || base[n - 1] == '\\')) {
        --n;
    }
    if (n + 1 >= out_len) {
        return 0;
    }
    (void)memcpy(out, base, n);
    out[n++] = '/';
    const size_t m = (sub != NULL) ? strlen(sub) : 0;
    if (n + m + 1 > out_len) {
        return 0;
    }
    if (m > 0) {
        (void)memcpy(out + n, sub, m);
        n += m;
    }
    out[n] = '\0';
    return 1;
}

typedef struct roots_builder {
    sxcl_paths_root *out;
    size_t cap;
    size_t count;
} roots_builder;

static void roots_add(roots_builder *rb, const char *path, const char *label, const char *owner,
                      const char *source, int priority, int expand)
{
    if (rb == NULL || rb->out == NULL || rb->count >= rb->cap) {
        return;
    }
    if (path == NULL || path[0] == '\0') {
        return;
    }
    sxcl_paths_root *slot = &rb->out[rb->count];
    (void)memset(slot, 0, sizeof(*slot));
    if (strlen(path) >= sizeof(slot->path)) {
        return; /* 太长:宁可不列,也不截断出一个假路径 */
    }
    paths_copy(slot->path, sizeof(slot->path), path);
    paths_copy(slot->label, sizeof(slot->label), label);
    paths_copy(slot->owner, sizeof(slot->owner), owner);
    paths_copy(slot->source, sizeof(slot->source), source);
    slot->priority = priority;
    slot->expand = expand;
    ++rb->count;
}

static void roots_add_sub(roots_builder *rb, const char *base, const char *sub, const char *label,
                          const char *owner, const char *source, int priority, int expand)
{
    char joined[SXCL_PATHS_PATH_MAX];
    if (base == NULL || base[0] == '\0') {
        return;
    }
    if (!roots_join(joined, sizeof(joined), base, sub)) {
        return;
    }
    roots_add(rb, joined, label, owner, source, priority, expand);
}

/** XDG 数据目录:显式给了就用它,否则 <home>/.local/share(Linux 桌面惯例)。 */
static void roots_linux_data_dir(const sxcl_paths_env *env, char *out, size_t out_len)
{
    if (env->xdg_data_home != NULL && env->xdg_data_home[0] != '\0') {
        paths_copy(out, out_len, env->xdg_data_home);
        return;
    }
    out[0] = '\0';
    if (env->user_home != NULL && env->user_home[0] != '\0') {
        (void)roots_join(out, out_len, env->user_home, ".local/share");
    }
}

size_t sxcl_paths_roots(const sxcl_paths_env *env, sxcl_paths_os os, sxcl_paths_root *out,
                        size_t cap)
{
    sxcl_paths_env empty;
    roots_builder rb;
    if (out == NULL || cap == 0) {
        return 0;
    }
    if (env == NULL) {
        (void)memset(&empty, 0, sizeof(empty));
        env = &empty;
    }
    rb.out = out;
    rb.cap = cap;
    rb.count = 0;

    /* 1) 启动器自己旁边(便携版习惯:SXCL.exe 旁边的 .minecraft,也认 minecraft / MC) */
    if (env->program_dir != NULL && env->program_dir[0] != '\0') {
        static const char *const portable[] = { ".minecraft", "minecraft", "MC" };
        size_t i = 0;
        for (i = 0; i < sizeof(portable) / sizeof(portable[0]); ++i) {
            roots_add_sub(&rb, env->program_dir, portable[i], "启动器目录（便携）", "本应用",
                          "Portable", SXCL_PATHS_PRIORITY_PORTABLE, 0);
        }
    }

    switch (os) {
    case SXCL_PATHS_OS_WINDOWS: {
        const char *appdata = env->app_data;
        static const char *const desk[] = { "Desktop", "桌面" };
        if (appdata != NULL && appdata[0] != '\0') {
            roots_add_sub(&rb, appdata, ".minecraft", "官方启动器（APPDATA）", "", "APPDATA",
                          SXCL_PATHS_PRIORITY_OFFICIAL, 0);
            /* HMCL:数据目录默认在 %APPDATA%\HMCL,里面还可能有自己的 .minecraft */
            roots_add_sub(&rb, appdata, "HMCL/.minecraft", "HMCL 游戏目录", "HMCL", "HMCL",
                          SXCL_PATHS_PRIORITY_THIRD_PARTY, 0);
            roots_add_sub(&rb, appdata, "HMCL", "HMCL 数据目录", "HMCL", "HMCL",
                          SXCL_PATHS_PRIORITY_THIRD_PARTY, 0);
            roots_add_sub(&rb, appdata, "MultiMC", "MultiMC 数据目录", "MultiMC", "MultiMC",
                          SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            roots_add_sub(&rb, appdata, "PrismLauncher", "Prism Launcher 数据目录", "Prism",
                          "Prism", SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            roots_add_sub(&rb, appdata, "ATLauncher", "ATLauncher 数据目录", "ATLauncher",
                          "ATLauncher", SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
        }
        if (env->user_home != NULL && env->user_home[0] != '\0') {
            /* 官方启动器在 Windows 上也认用户目录(~/.minecraft,老版本/手工装常见) */
            roots_add_sub(&rb, env->user_home, ".minecraft", "用户目录", "", "UserHome",
                          SXCL_PATHS_PRIORITY_USER_HOME, 0);
            /* CurseForge App:实例装在 <用户目录>\curseforge\minecraft\Instances\<名字> */
            roots_add_sub(&rb, env->user_home, "curseforge/minecraft", "CurseForge 数据目录",
                          "CurseForge", "CurseForge", SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            size_t d = 0;
            for (d = 0; d < sizeof(desk) / sizeof(desk[0]); ++d) {
                char desktop[SXCL_PATHS_TMP];
                if (roots_join(desktop, sizeof(desktop), env->user_home, desk[d])) {
                    roots_add_sub(&rb, desktop, ".minecraft", "桌面", "", "Desktop",
                                  SXCL_PATHS_PRIORITY_DESKTOP, 0);
                }
            }
        }
        break;
    }
    case SXCL_PATHS_OS_MACOS: {
        static const char *const desk_mac[] = { "Desktop", "桌面" };
        const char *home = env->user_home;
        if (home != NULL && home[0] != '\0') {
            roots_add_sub(&rb, home, "Library/Application Support/minecraft",
                          "官方启动器（Application Support）", "", "AppSupport",
                          SXCL_PATHS_PRIORITY_OFFICIAL, 0);
            roots_add_sub(&rb, home, "Library/Application Support/HMCL",
                          "HMCL 数据目录", "HMCL", "HMCL", SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            roots_add_sub(&rb, home, "Library/Application Support/multimc",
                          "MultiMC 数据目录", "MultiMC", "MultiMC",
                          SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            roots_add_sub(&rb, home, "Library/Application Support/PrismLauncher",
                          "Prism Launcher 数据目录", "Prism", "Prism",
                          SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            roots_add_sub(&rb, home, "Library/Application Support/ATLauncher",
                          "ATLauncher 数据目录", "ATLauncher", "ATLauncher",
                          SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            roots_add_sub(&rb, home, "Library/Application Support/curseforge/minecraft",
                          "CurseForge 数据目录", "CurseForge", "CurseForge",
                          SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            roots_add_sub(&rb, home, ".minecraft", "用户目录", "", "UserHome",
                          SXCL_PATHS_PRIORITY_USER_HOME, 0);
            size_t d = 0;
            for (d = 0; d < sizeof(desk_mac) / sizeof(desk_mac[0]); ++d) {
                char desktop[SXCL_PATHS_TMP];
                if (roots_join(desktop, sizeof(desktop), home, desk_mac[d])) {
                    roots_add_sub(&rb, desktop, ".minecraft", "桌面", "", "Desktop",
                                  SXCL_PATHS_PRIORITY_DESKTOP, 0);
                }
            }
        }
        break;
    }
    case SXCL_PATHS_OS_ANDROID: {
        /* 安卓:唯一免权限可写的是本应用私有 files 目录;共享存储要"所有文件访问"权限。
         * 顺序 = 私有目录 -> 我们自己/别的启动器在共享存储上的已知位置(能不能读由探测层实测)。 */
        char shared[SXCL_PATHS_PATH_MAX];
        const char *sh = (env->android_shared != NULL && env->android_shared[0] != '\0')
                             ? env->android_shared
                             : "/storage/emulated/0";
        paths_copy(shared, sizeof(shared), sh);
        if (env->android_files != NULL && env->android_files[0] != '\0') {
            roots_add_sub(&rb, env->android_files, ".minecraft", "本应用（私有目录）", "本应用",
                          "AndroidPrivate", SXCL_PATHS_PRIORITY_ANDROID, 0);
        }
        /* 顺序与 sxcl_paths_android_roots 完全一致(那份表是设备实测出来的,别调) */
        roots_add_sub(&rb, shared, "FCL/.minecraft", "FCL 游戏目录（共享存储）", "FCL",
                      "AndroidShared", SXCL_PATHS_PRIORITY_ANDROID, 0);
        roots_add_sub(&rb, shared, "games/FCL/.minecraft", "FCL 游戏目录（旧版位置）", "FCL",
                      "AndroidShared", SXCL_PATHS_PRIORITY_ANDROID, 0);
        roots_add_sub(&rb, shared, ".minecraft", "共享存储根目录", "共享存储", "AndroidShared",
                      SXCL_PATHS_PRIORITY_ANDROID, 0);
        roots_add_sub(&rb, shared, "HMCL/.minecraft", "HMCL 游戏目录（共享存储）", "HMCL",
                      "AndroidShared", SXCL_PATHS_PRIORITY_ANDROID, 0);
        roots_add_sub(&rb, shared, "games/PojavLauncher/.minecraft", "PojavLauncher 游戏目录",
                      "PojavLauncher", "AndroidShared", SXCL_PATHS_PRIORITY_ANDROID, 0);
        roots_add_sub(&rb, shared, "Android/data/com.tungsten.fcl/files/.minecraft",
                      "FCL 应用数据目录", "FCL", "AndroidShared",
                      SXCL_PATHS_PRIORITY_ANDROID, 0);
        roots_add_sub(&rb, shared, "Android/data/org.jackhuang.hmcl/files/.minecraft",
                      "HMCL 应用数据目录", "HMCL", "AndroidShared",
                      SXCL_PATHS_PRIORITY_ANDROID, 0);
        roots_add_sub(&rb, shared, "Android/data/net.kdt.pojavlaunch/files/.minecraft",
                      "PojavLauncher 应用数据目录", "PojavLauncher", "AndroidShared",
                      SXCL_PATHS_PRIORITY_ANDROID, 0);
        roots_add_sub(&rb, shared, "Android/data/com.silentstudio.sxcl/files/.minecraft",
                      "本应用（共享存储侧）", "本应用", "AndroidShared",
                      SXCL_PATHS_PRIORITY_ANDROID, 0);
        break;
    }
    case SXCL_PATHS_OS_LINUX:
    default: {
        static const char *const desk_lin[] = { "Desktop", "桌面" };
        char data[SXCL_PATHS_PATH_MAX];
        const char *home = env->user_home;
        roots_linux_data_dir(env, data, sizeof(data));
        if (home != NULL && home[0] != '\0') {
            roots_add_sub(&rb, home, ".minecraft", "官方启动器（用户目录）", "", "UserHome",
                          SXCL_PATHS_PRIORITY_OFFICIAL, 0);
            /* Flatpak 沙箱里的官方启动器:数据在 ~/.var/app 下,不探就永远找不到 */
            roots_add_sub(&rb, home, ".var/app/com.mojang.Minecraft/.minecraft",
                          "官方启动器（Flatpak 沙箱）", "", "Flatpak",
                          SXCL_PATHS_PRIORITY_USER_HOME, 0);
            roots_add_sub(&rb, home,
                          ".var/app/org.prismlauncher.PrismLauncher/data/PrismLauncher",
                          "Prism Launcher 数据目录（Flatpak 沙箱）", "Prism", "Flatpak",
                          SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            size_t d = 0;
            for (d = 0; d < sizeof(desk_lin) / sizeof(desk_lin[0]); ++d) {
                char desktop[SXCL_PATHS_TMP];
                if (roots_join(desktop, sizeof(desktop), home, desk_lin[d])) {
                    roots_add_sub(&rb, desktop, ".minecraft", "桌面", "", "Desktop",
                                  SXCL_PATHS_PRIORITY_DESKTOP, 0);
                }
            }
        }
        if (data[0] != '\0') {
            roots_add_sub(&rb, data, "multimc", "MultiMC 数据目录", "MultiMC", "MultiMC",
                          SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            roots_add_sub(&rb, data, "PrismLauncher", "Prism Launcher 数据目录", "Prism",
                          "Prism", SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            roots_add_sub(&rb, data, "ATLauncher", "ATLauncher 数据目录", "ATLauncher",
                          "ATLauncher", SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
            roots_add_sub(&rb, data, "curseforge/minecraft", "CurseForge 数据目录", "CurseForge",
                          "CurseForge", SXCL_PATHS_PRIORITY_THIRD_PARTY, 1);
        }
        break;
    }
    }
    return rb.count;
}

void sxcl_paths_env_capture(sxcl_paths_env_store *store)
{
    if (store == NULL) {
        return;
    }
    (void)memset(store, 0, sizeof(*store));
    paths_copy(store->app_data, sizeof(store->app_data), env_value("APPDATA"));
    paths_copy(store->local_app_data, sizeof(store->local_app_data), env_value("LOCALAPPDATA"));
    {
        char home[SXCL_PATHS_TMP];
        if (paths_home_dir(home, sizeof(home)) == 0) {
            paths_copy(store->user_home, sizeof(store->user_home), home);
        }
    }
    {
        char program[SXCL_PATHS_TMP];
        if (sxcl_paths_program_dir(program, sizeof(program), NULL, 0) == SXCL_PATHS_OK) {
            paths_copy(store->program_dir, sizeof(store->program_dir), program);
        }
    }
    paths_copy(store->xdg_data_home, sizeof(store->xdg_data_home), env_value("XDG_DATA_HOME"));
    paths_copy(store->android_files, sizeof(store->android_files), env_value("SXCL_ANDROID_FILES"));
    paths_copy(store->android_shared, sizeof(store->android_shared), env_value("SXCL_ANDROID_SHARED"));

    store->env.app_data = store->app_data;
    store->env.local_app_data = store->local_app_data;
    store->env.user_home = store->user_home;
    store->env.program_dir = store->program_dir;
    store->env.xdg_data_home = store->xdg_data_home;
    store->env.android_files = store->android_files;
    store->env.android_shared = store->android_shared;
}

/* ══════════════════════ "像不像 MC 目录"的判据 ══════════════════════ */

int sxcl_paths_marks(const char *path)
{
    int marks = 0;
    char probe[SXCL_PATHS_TMP];
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (sxcl_paths_count_versions(path) > 0) {
        marks |= SXCL_PATHS_MARK_VERSIONS;
    }
    if (paths_join(probe, sizeof(probe), path, "libraries") && sxcl_fs_is_dir(probe)) {
        marks |= SXCL_PATHS_MARK_LIBRARIES;
    }
    if (paths_join(probe, sizeof(probe), path, "assets") && sxcl_fs_is_dir(probe)) {
        marks |= SXCL_PATHS_MARK_ASSETS;
    }
    if (paths_join(probe, sizeof(probe), path, "launcher_profiles.json") && sxcl_fs_exists(probe)) {
        marks |= SXCL_PATHS_MARK_PROFILES;
    }
    if (paths_join(probe, sizeof(probe), path, "logs") && sxcl_fs_is_dir(probe)) {
        marks |= SXCL_PATHS_MARK_LOGS;
    }
    return marks;
}

int sxcl_paths_marks_text(int marks, int versions, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return SXCL_PATHS_ERR_ARG;
    }
    out[0] = '\0';
    if (marks == 0) {
        return (snprintf(out, out_len, "空目录（没有 versions/）") < 0) ? SXCL_PATHS_ERR_SPACE
                                                                       : SXCL_PATHS_OK;
    }
    char buf[SXCL_PATHS_DESC_MAX];
    size_t used = 0;
    buf[0] = '\0';
    struct {
        int bit;
        const char *text;
    } parts[5];
    size_t n = 0;
    char versions_text[64];
    (void)snprintf(versions_text, sizeof(versions_text), "有 versions/（%d 个版本）", versions);
    parts[0].bit = SXCL_PATHS_MARK_VERSIONS;
    parts[0].text = versions_text;
    parts[1].bit = SXCL_PATHS_MARK_LIBRARIES;
    parts[1].text = "有 libraries/";
    parts[2].bit = SXCL_PATHS_MARK_ASSETS;
    parts[2].text = "有 assets/";
    parts[3].bit = SXCL_PATHS_MARK_PROFILES;
    parts[3].text = "有 launcher_profiles.json";
    parts[4].bit = SXCL_PATHS_MARK_LOGS;
    parts[4].text = "有 logs/";
    for (n = 0; n < sizeof(parts) / sizeof(parts[0]); ++n) {
        if ((marks & parts[n].bit) == 0) {
            continue;
        }
        const int written = snprintf(buf + used, sizeof(buf) - used, "%s%s",
                                     (used == 0) ? "" : "、", parts[n].text);
        if (written < 0 || (size_t)written >= sizeof(buf) - used) {
            break;
        }
        used += (size_t)written;
    }
    if (used == 0) {
        return (snprintf(out, out_len, "不像 MC 目录") < 0) ? SXCL_PATHS_ERR_SPACE : SXCL_PATHS_OK;
    }
    if (snprintf(out, out_len, "%s", buf) < 0 || strlen(buf) >= out_len) {
        return SXCL_PATHS_ERR_SPACE;
    }
    return SXCL_PATHS_OK;
}

/** 已经写过这个路径了吗(大小写口径跟着平台走)。 */
static int expand_seen(const sxcl_paths_root *out, size_t written, const char *path)
{
    for (size_t i = 0; i < written; ++i) {
        if (sxcl_dir_name_compare(out[i].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 容器根(installer 家族的数据目录)展开一层:instances/<名字>/{.minecraft,minecraft,<名字>} */
size_t sxcl_paths_expand_roots(const sxcl_paths_root *roots, size_t count, sxcl_paths_root *out,
                               size_t cap)
{
    /* 大小写两套名字都要试:MultiMC/Prism 是 instances/,CurseForge 是 Instances/。
     * 但 Windows 的文件系统大小写不敏感,两趟会**枚举到同一个目录** ——
     * 所以写入前按 sxcl_dir_name_compare 去重(Windows 上忽略大小写,Linux 上不忽略)。 */
    static const char *const family_dirs[] = { "instances", "Instances" };
    size_t written = 0;
    if (roots == NULL || out == NULL || cap == 0) {
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (written >= cap) {
            break;
        }
        if (roots[i].expand == 0) {
            out[written++] = roots[i];
            continue;
        }
        int expanded = 0;
        for (size_t f = 0; f < sizeof(family_dirs) / sizeof(family_dirs[0]) && written < cap; ++f) {
            char list_dir[SXCL_PATHS_TMP];
            if (!paths_join(list_dir, sizeof(list_dir), roots[i].path, family_dirs[f])) {
                continue;
            }
            if (!sxcl_fs_is_dir(list_dir)) {
                continue;
            }
            sxcl_dir_list list;
            char err[SXCL_DIR_ERROR_MAX];
            if (sxcl_dir_list_open(list_dir, &list, err, sizeof(err)) != 0) {
                continue;
            }
            for (size_t k = 0; k < list.count && written < cap; ++k) {
                static const char *const inside[] = { ".minecraft", "minecraft" };
                char inst[SXCL_PATHS_TMP];
                if (!list.items[k].is_dir) {
                    continue;
                }
                if (!paths_join(inst, sizeof(inst), list_dir, list.items[k].name)) {
                    continue;
                }
                expanded = 1;
                for (size_t s = 0; s < sizeof(inside) / sizeof(inside[0]) && written < cap; ++s) {
                    char sub[SXCL_PATHS_TMP];
                    if (!paths_join(sub, sizeof(sub), inst, inside[s])) {
                        continue;
                    }
                    if (!sxcl_fs_is_dir(sub)) {
                        continue;
                    }
                    sxcl_paths_root item = roots[i];
                    item.expand = 0;
                    paths_copy(item.path, sizeof(item.path), sub);
                    (void)snprintf(item.label, sizeof(item.label), "%s 实例（%s）",
                                   (roots[i].owner[0] != '\0') ? roots[i].owner : "启动器",
                                   list.items[k].name);
                    if (!expand_seen(out, written, item.path)) {
                        out[written++] = item;
                    }
                }
                /* 实例目录本身就是游戏目录(MultiMC 现代布局偶见) */
                if (sxcl_paths_count_versions(inst) > 0) {
                    sxcl_paths_root item = roots[i];
                    item.expand = 0;
                    paths_copy(item.path, sizeof(item.path), inst);
                    (void)snprintf(item.label, sizeof(item.label), "%s 实例（%s）",
                                   (roots[i].owner[0] != '\0') ? roots[i].owner : "启动器",
                                   list.items[k].name);
                    if (!expand_seen(out, written, item.path)) {
                        out[written++] = item;
                    }
                }
            }
            sxcl_dir_list_free(&list);
        }
        /* 展开不出东西也把容器本身留着:它有可能是"直接放了 versions/ 的目录" */
        if (expanded == 0 && written < cap) {
            out[written++] = roots[i];
        }
    }
    return written;
}

/* ══════════════════════ 全平台探测(可注入 env) ══════════════════════ */

int sxcl_paths_detect_ex(const sxcl_paths_env *env, sxcl_paths_os os, sxcl_game_folders *folders,
                         const char *configured_dir, char *err, size_t err_len)
{
    sxcl_paths_root roots[SXCL_PATHS_MAX_ROOTS];
    sxcl_paths_root flats[SXCL_PATHS_MAX_ROOTS * 2];
    size_t root_count = 0;
    size_t flat_count = 0;
    if (folders == NULL) {
        paths_err(err, err_len, "参数不合法");
        return SXCL_PATHS_ERR_ARG;
    }
    (void)memset(folders, 0, sizeof(*folders));
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }

    root_count = sxcl_paths_roots(env, os, roots, SXCL_PATHS_MAX_ROOTS);
    flat_count = sxcl_paths_expand_roots(roots, root_count, flats,
                                         sizeof(flats) / sizeof(flats[0]));

    /* 已配置目录**最高优先**:先插它,后面的重复项会被去重丢掉,标签也就留在"当前配置"上。
     * 不存在的配置目录**不进列表**(与安卓那路 sxcl_paths_detect_android 完全一致的口径:
     * 列表 = 真实存在的候选)。它到底在不在,由 sxcl_paths_resolve_game_dir(配置优先,原样返回)
     * 与设置页的 sxcl_paths_inspect_full 负责如实告诉用户,不靠这张列表。 */
    if (configured_dir != NULL && configured_dir[0] != '\0') {
        sxcl_game_folder folder;
        if (sxcl_paths_inspect_full(configured_dir, "当前配置", "", &folder) == SXCL_PATHS_OK &&
            folder.exists) {
            folder.priority = SXCL_PATHS_PRIORITY_CONFIGURED;
            paths_copy(folder.source, sizeof(folder.source), "Configured");
            (void)sxcl_paths_push(folders, &folder);
        }
    }

    for (size_t i = 0; i < flat_count; ++i) {
        int duplicate = 0;
        sxcl_game_folder folder;
        if (flats[i].path[0] == '\0') {
            continue;
        }
        for (size_t k = 0; k < folders->count; ++k) {
            if (sxcl_dir_name_compare(folders->items[k].path, flats[i].path) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (sxcl_paths_inspect_full(flats[i].path, flats[i].label, flats[i].owner, &folder) !=
            SXCL_PATHS_OK) {
            continue;
        }
        folder.priority = flats[i].priority;
        paths_copy(folder.source, sizeof(folder.source), flats[i].source);
        if (!folder.exists) {
            continue; /* 不存在的候选不列出来(配置目录是例外,上面单独处理) */
        }
        (void)sxcl_paths_push(folders, &folder);
    }

    sxcl_paths_sort(folders);
    if (folders->count == 0) {
        paths_err(err, err_len,
                  "没找到游戏目录（官方启动器、HMCL、MultiMC/Prism、CurseForge、ATLauncher 与"
                  "用户目录/桌面都查过了，没有带 versions/ 的目录）：请在设置里指定 .minecraft 目录");
        return SXCL_PATHS_OK;
    }
    return SXCL_PATHS_OK;
}

int sxcl_paths_detect(sxcl_game_folders *folders, const char *configured_dir, char *err, size_t err_len)
{
    /* 平台候选表现已抽到 sxcl_paths_roots()(纯字符串、可注入 env),
     * 这里只负责"抓本机环境 + 按本机平台探测",语义与老版本完全一致:
     *   平台候选 + 最高优先的当前配置 -> 只留真实存在的目录 -> 去重 -> 按 priority/score 降序。 */
    sxcl_paths_env_store store;
    sxcl_paths_env_capture(&store);
    return sxcl_paths_detect_ex(&store.env, sxcl_paths_current_os(), folders, configured_dir, err,
                                err_len);
}

const sxcl_game_folder *sxcl_paths_best(const sxcl_game_folders *folders)
{
    if (folders == NULL || folders->count == 0) {
        return NULL;
    }
    /* 已配置目录最高优先:用户明确指定过的那个就是答案(哪怕它还是空的 ——
     * 那正是"你配的这个目录里还没有版本",界面要如实这么提示)。 */
    for (size_t i = 0; i < folders->count; ++i) {
        if (folders->items[i].priority >= SXCL_PATHS_PRIORITY_CONFIGURED) {
            return &folders->items[i];
        }
    }
    for (size_t i = 0; i < folders->count; ++i) {
        if (folders->items[i].versions > 0) {
            return &folders->items[i];
        }
    }
    return &folders->items[0];
}

int sxcl_paths_resolve_game_dir(const char *configured_dir, char *out, size_t out_len,
                                char *err, size_t err_len)
{
    if (out == NULL || out_len == 0) {
        paths_err(err, err_len, "参数不合法");
        return SXCL_PATHS_ERR_ARG;
    }
    out[0] = '\0';
    if (configured_dir != NULL && configured_dir[0] != '\0') {
        if (strlen(configured_dir) >= out_len) {
            paths_err(err, err_len, "配置里的游戏目录路径太长");
            return SXCL_PATHS_ERR_SPACE;
        }
        paths_copy(out, out_len, configured_dir);
        return SXCL_PATHS_OK;
    }
    sxcl_game_folders folders;
    char detect_err[SXCL_PATHS_ERROR_MAX];
#if defined(__ANDROID__)
    /* 安卓走**专用候选表**(共享存储 + 各家启动器),桌面那五条在安卓上几乎全是空的。
     * 两个环境变量都由安卓打包层设置;SXCL_ANDROID_SHARED 可空(默认 /storage/emulated/0)。 */
    (void)sxcl_paths_detect_android(env_value("SXCL_ANDROID_FILES"), env_value("SXCL_ANDROID_SHARED"),
                                    &folders, NULL, detect_err, sizeof(detect_err));
#else
    (void)sxcl_paths_detect(&folders, NULL, detect_err, sizeof(detect_err));
#endif
    const sxcl_game_folder *best = sxcl_paths_best(&folders);
    if (best != NULL && best->exists) {
        if (strlen(best->path) >= out_len) {
            paths_err(err, err_len, "探测到的游戏目录路径太长");
            return SXCL_PATHS_ERR_SPACE;
        }
        paths_copy(out, out_len, best->path);
        return SXCL_PATHS_OK;
    }
    char fallback_err[SXCL_PATHS_ERROR_MAX];
    const int rc = sxcl_paths_default_game_dir(out, out_len, fallback_err, sizeof(fallback_err));
    if (rc != SXCL_PATHS_OK) {
        out[0] = '\0';
        paths_err(err, err_len, "%s", fallback_err);
        return SXCL_PATHS_ERR_EMPTY;
    }
    if (detect_err[0] != '\0') {
        paths_err(err, err_len, "%s", detect_err);
    }
    return SXCL_PATHS_OK;
}

/* ══════════════════════ Android:候选表 / 探测 / 诊断 ══════════════════════ */

/* 安卓候选表已并入统一候选根表 sxcl_paths_roots(env, SXCL_PATHS_OS_ANDROID)(见上)。
 * 这里保留这个口子是因为它被单测与设置页直接调用,而且**顺序**与设备实测一致
 * (实测依据见 docs/08 第 14 节 / docs/11):
 *   FCL    默认把游戏数据放 getExternalFilesDir()/.minecraft,也允许用户改成
 *          /storage/emulated/0/FCL/.minecraft —— 这台平板用的就是后者;
 *   HMCL   安卓版的数据目录在 Android/data/org.jackhuang.hmcl/files 下;
 *   PojavLauncher 在 Android/data/net.kdt.pojavlaunch/files 与 games/PojavLauncher 下。
 * 列表只是"去哪儿找",**不代表能读**(共享存储要 MANAGE_EXTERNAL_STORAGE;
 * Android/data/<别人的包名> 在 Android 11+ 对别的应用完全不可见)。能不能读由
 * sxcl_paths_probe_android 如实告诉用户。 */

#define SXCL_ANDROID_DEFAULT_SHARED "/storage/emulated/0"

size_t sxcl_paths_android_roots(const char *files_dir, const char *shared_root,
                                sxcl_android_root *out, size_t cap)
{
    sxcl_paths_env env;
    sxcl_paths_root roots[SXCL_PATHS_MAX_ROOTS];
    size_t n = 0;
    size_t i = 0;
    char shared[SXCL_PATHS_PATH_MAX];
    if (out == NULL || cap == 0) {
        return 0;
    }
    paths_copy(shared, sizeof(shared),
               (shared_root != NULL && shared_root[0] != '\0') ? shared_root
                                                               : SXCL_ANDROID_DEFAULT_SHARED);
    (void)memset(&env, 0, sizeof(env));
    env.android_files = files_dir;
    env.android_shared = shared;
    n = sxcl_paths_roots(&env, SXCL_PATHS_OS_ANDROID, roots, SXCL_PATHS_MAX_ROOTS);
    if (n > cap) {
        n = cap;
    }
    for (i = 0; i < n; ++i) {
        (void)memset(&out[i], 0, sizeof(out[i]));
        paths_copy(out[i].path, sizeof(out[i].path), roots[i].path);
        paths_copy(out[i].label, sizeof(out[i].label), roots[i].label);
        paths_copy(out[i].owner, sizeof(out[i].owner), roots[i].owner);
    }
    return n;
}

static int android_folders_has(const sxcl_game_folders *folders, const char *path)
{
    size_t k = 0;
    for (k = 0; k < folders->count; ++k) {
        if (sxcl_dir_name_compare(folders->items[k].path, path) == 0) {
            return 1;
        }
    }
    return 0;
}

int sxcl_paths_detect_android(const char *files_dir, const char *shared_root,
                              sxcl_game_folders *folders, const char *configured_dir,
                              char *err, size_t err_len)
{
    sxcl_android_root roots[SXCL_PATHS_MAX_PROBES];
    size_t rn = 0;
    size_t i = 0;
    if (folders == NULL) {
        paths_err(err, err_len, "参数不合法");
        return SXCL_PATHS_ERR_ARG;
    }
    (void)memset(folders, 0, sizeof(*folders));

    /* 用户手动配置的目录排第一:他说的算(哪怕目录不存在也要如实列出来) */
    if (configured_dir != NULL && configured_dir[0] != '\0' &&
        strlen(configured_dir) < SXCL_PATHS_PATH_MAX) {
        sxcl_game_folder folder;
        if (sxcl_paths_inspect_full(configured_dir, "当前配置", NULL, &folder) == SXCL_PATHS_OK &&
            folder.exists) {
            (void)sxcl_paths_push(folders, &folder);
        }
    }

    rn = sxcl_paths_android_roots(files_dir, shared_root, roots, SXCL_PATHS_MAX_PROBES);
    for (i = 0; i < rn; ++i) {
        sxcl_game_folder folder;
        if (android_folders_has(folders, roots[i].path)) {
            continue;
        }
        if (sxcl_paths_inspect_full(roots[i].path, roots[i].label, roots[i].owner, &folder) !=
            SXCL_PATHS_OK) {
            continue;
        }
        if (!folder.exists) {
            continue; /* 不存在的候选不进列表(与桌面语义一致) */
        }
        (void)sxcl_paths_push(folders, &folder);
    }
    sxcl_paths_sort(folders);
    if (folders->count == 0) {
        paths_err(err, err_len,
                  "没找到游戏目录（安卓上找过共享存储 .minecraft 与 FCL/HMCL/PojavLauncher 的数据目录）："
                  "请在设置里指定 .minecraft 目录");
    }
    return SXCL_PATHS_OK;
}

/** 填一条诊断。返回 1 = 这条是真能用的游戏目录。 */
static int android_fill_probe(sxcl_game_probe *probe, const char *path, const char *label,
                              const char *owner)
{
    char detail[SXCL_PATHS_DESC_MAX];
    sxcl_game_folder folder;
    (void)memset(probe, 0, sizeof(*probe));
    paths_copy(probe->path, sizeof(probe->path), path);
    paths_copy(probe->label, sizeof(probe->label), (label != NULL) ? label : "");
    paths_copy(probe->owner, sizeof(probe->owner), (owner != NULL) ? owner : "");
    detail[0] = '\0';
    probe->access = sxcl_android_probe_path(path, 0, detail, sizeof(detail));
    if (probe->access == SXCL_ANDROID_OK &&
        sxcl_paths_inspect_full(path, label, owner, &folder) == SXCL_PATHS_OK) {
        probe->exists = folder.exists;
        probe->versions = folder.versions;
    }
    if (probe->exists) {
        if (probe->versions > 0) {
            (void)snprintf(probe->reason, sizeof(probe->reason), "找到游戏目录:%d 个版本",
                           probe->versions);
            (void)snprintf(probe->hint, sizeof(probe->hint), "可以直接选它启动游戏。");
            return 1;
        }
        (void)snprintf(probe->reason, sizeof(probe->reason),
                       "目录在,但里面没有 versions/,不像游戏目录");
        (void)snprintf(probe->hint, sizeof(probe->hint),
                       "如果游戏装在别处,请手动指定那个 .minecraft 目录。");
        return 0;
    }
    /* 怎么办:按**游戏目录**的场景写,别拿 Java 那套文案糊上去 */
    switch (probe->access) {
    case SXCL_ANDROID_DENIED:
        (void)snprintf(probe->hint, sizeof(probe->hint),
                       "安卓不让本应用读这里。共享存储上的游戏目录需要在 系统设置 - 应用 - 特殊应用权限 -"
                       " 所有文件访问权限 里给本应用授权;没有这个权限时,只认本应用私有目录里的"
                       " .minecraft。");
        break;
    case SXCL_ANDROID_NOT_READABLE:
        (void)snprintf(probe->hint, sizeof(probe->hint), "读这个位置时出错,可能是权限或存储已卸载。");
        break;
    case SXCL_ANDROID_NOEXEC:
        (void)snprintf(probe->hint, sizeof(probe->hint), "共享存储是 noexec,游戏数据放这里跑不起来。");
        break;
    case SXCL_ANDROID_MISSING:
    default:
        (void)snprintf(probe->hint, sizeof(probe->hint),
                       "这个位置没有游戏数据;在设置里手动指定你的 .minecraft 目录即可。");
        break;
    }
    /* 原样带出体检的**原始原因**(含路径与原话) —— "为什么没检测到"要能拿给用户看,
     * 不能只回一句"没找到";怎么办由 sxcl_android_access_hint 另外给。 */
    (void)snprintf(probe->reason, sizeof(probe->reason), "%s", detail);
    return 0;
}

size_t sxcl_paths_probe_android(const char *files_dir, const char *shared_root,
                                const char *configured_dir, sxcl_game_probes *out)
{
    sxcl_android_root roots[SXCL_PATHS_MAX_PROBES];
    size_t rn = 0;
    size_t i = 0;
    if (out == NULL) {
        return 0;
    }
    (void)memset(out, 0, sizeof(*out));

    if (configured_dir != NULL && configured_dir[0] != '\0' &&
        strlen(configured_dir) < SXCL_PATHS_PATH_MAX && out->count < SXCL_PATHS_MAX_PROBES) {
        if (android_fill_probe(&out->items[out->count], configured_dir, "当前配置", NULL)) {
            ++out->usable;
        }
        ++out->count;
    }

    rn = sxcl_paths_android_roots(files_dir, shared_root, roots, SXCL_PATHS_MAX_PROBES);
    for (i = 0; i < rn && out->count < SXCL_PATHS_MAX_PROBES; ++i) {
        int dup = 0;
        size_t k = 0;
        for (k = 0; k < out->count; ++k) {
            if (sxcl_dir_name_compare(out->items[k].path, roots[i].path) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (android_fill_probe(&out->items[out->count], roots[i].path, roots[i].label,
                               roots[i].owner)) {
            ++out->usable;
        }
        ++out->count;
    }
    return out->count;
}

