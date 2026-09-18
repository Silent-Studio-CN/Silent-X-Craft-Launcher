/* SXCL-C 游戏目录探测实现 —— 逐条对齐 Python 版 src/services/minecraft/folders.py。
 * 注意:所有函数都"不抛异常",失败给人话 err;路径一律 UTF-8。 */
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
    /* 稳定插入排序:分数高的在前,同分保持原有先后(测试要能预期顺序) */
    for (size_t i = 1; i < folders->count; ++i) {
        sxcl_game_folder key = folders->items[i];
        size_t j = i;
        while (j > 0 && folders->items[j - 1].score < key.score) {
            folders->items[j] = folders->items[j - 1];
            --j;
        }
        folders->items[j] = key;
    }
}

int sxcl_paths_detect(sxcl_game_folders *folders, const char *configured_dir, char *err, size_t err_len)
{
    if (folders == NULL) {
        paths_err(err, err_len, "参数不合法");
        return SXCL_PATHS_ERR_ARG;
    }
    (void)memset(folders, 0, sizeof(*folders));

    char home[SXCL_PATHS_TMP];
    char appdata[SXCL_PATHS_TMP];
    char program[SXCL_PATHS_TMP];
    const int have_home = (paths_home_dir(home, sizeof(home)) == 0);
    const int have_appdata = (paths_appdata_dir(appdata, sizeof(appdata)) == 0);
    const int have_program = (sxcl_paths_program_dir(program, sizeof(program), NULL, 0) == SXCL_PATHS_OK);

    /* 候选顺序与 Python 一致:启动器目录(便携) -> APPDATA -> 用户目录 -> 桌面 -> 当前配置 */
    struct {
        char path[SXCL_PATHS_PATH_MAX];
        const char *label;
        int ok;
    } cands[12];
    size_t cand_count = 0;
    (void)memset(cands, 0, sizeof(cands));

    if (have_program) {
        static const char *const portable[] = { ".minecraft", "minecraft", "MC" };
        for (size_t i = 0; i < sizeof(portable) / sizeof(portable[0]); ++i) {
            cands[cand_count].ok = paths_join(cands[cand_count].path, sizeof(cands[cand_count].path), program, portable[i]);
            cands[cand_count].label = "启动器目录（便携）";
            ++cand_count;
        }
    }
    if (have_appdata) {
        cands[cand_count].ok = paths_join(cands[cand_count].path, sizeof(cands[cand_count].path), appdata, ".minecraft");
        cands[cand_count].label = "官方启动器（APPDATA）";
        ++cand_count;
    }
    if (have_home) {
        cands[cand_count].ok = paths_join(cands[cand_count].path, sizeof(cands[cand_count].path), home, ".minecraft");
        cands[cand_count].label = "用户目录";
        ++cand_count;
        static const char *const desktops[] = { "Desktop", "桌面" };
        for (size_t i = 0; i < sizeof(desktops) / sizeof(desktops[0]); ++i) {
            char desk[SXCL_PATHS_TMP];
            if (!paths_join(desk, sizeof(desk), home, desktops[i])) {
                cands[cand_count].ok = 0;
                cands[cand_count].label = "桌面";
                ++cand_count;
                continue;
            }
            cands[cand_count].ok = paths_join(cands[cand_count].path, sizeof(cands[cand_count].path), desk, ".minecraft");
            cands[cand_count].label = "桌面";
            ++cand_count;
        }
    }
    if (configured_dir != NULL && configured_dir[0] != '\0') {
        cands[cand_count].ok = (strlen(configured_dir) < sizeof(cands[cand_count].path));
        if (cands[cand_count].ok) {
            paths_copy(cands[cand_count].path, sizeof(cands[cand_count].path), configured_dir);
        }
        cands[cand_count].label = "当前配置";
        ++cand_count;
    }

    /* 去重(Windows 大小写不敏感)+ 只保留真实存在的目录(Python: folders = [x for x in seen if x.exists]) */
    for (size_t i = 0; i < cand_count; ++i) {
        if (!cands[i].ok) {
            continue;
        }
        int duplicate = 0;
        for (size_t k = 0; k < folders->count; ++k) {
            if (sxcl_dir_name_compare(folders->items[k].path, cands[i].path) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        sxcl_game_folder folder;
        if (sxcl_paths_inspect(cands[i].path, cands[i].label, &folder) != SXCL_PATHS_OK) {
            continue;
        }
        if (!folder.exists) {
            continue; /* 不存在的候选不列出来;当前配置不存在时由 resolve_game_dir 如实返回 */
        }
        (void)sxcl_paths_push(folders, &folder);
    }

    sxcl_paths_sort(folders);
    if (folders->count == 0) {
        paths_err(err, err_len, "没找到游戏目录（没有 versions/ 的候选）：请在设置里指定 .minecraft 目录");
        return SXCL_PATHS_OK;
    }
    return SXCL_PATHS_OK;
}

const sxcl_game_folder *sxcl_paths_best(const sxcl_game_folders *folders)
{
    if (folders == NULL || folders->count == 0) {
        return NULL;
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

/* 相对共享存储根的候选(实测依据见 docs/08 第 14 节):
 *   FCL    默认把游戏数据放 getExternalFilesDir()/.minecraft,也允许用户改成
 *          /storage/emulated/0/FCL/.minecraft —— 这台平板(.33)用的就是后者;
 *   HMCL   安卓版的数据目录在 Android/data/org.jackhuang.hmcl/files 下;
 *   PojavLauncher 在 Android/data/net.kdt.pojavlaunch/files 与 games/PojavLauncher 下。
 * 列表只是"去哪儿找",**不代表能读**(共享存储要 MANAGE_EXTERNAL_STORAGE;
 * Android/data/<别人的包名> 在 Android 11+ 对别的应用完全不可见)。能不能读由
 * sxcl_paths_probe_android 如实告诉用户。 */
typedef struct android_rel_root {
    const char *rel;
    const char *label;
    const char *owner;
} android_rel_root;

static const android_rel_root kAndroidRelRoots[] = {
    { "FCL/.minecraft", "FCL 游戏目录（共享存储）", "FCL" },
    { "games/FCL/.minecraft", "FCL 游戏目录（旧版位置）", "FCL" },
    { ".minecraft", "共享存储根目录", "共享存储" },
    { "HMCL/.minecraft", "HMCL 游戏目录（共享存储）", "HMCL" },
    { "games/PojavLauncher/.minecraft", "PojavLauncher 游戏目录", "PojavLauncher" },
    { "Android/data/com.tungsten.fcl/files/.minecraft", "FCL 应用数据目录", "FCL" },
    { "Android/data/org.jackhuang.hmcl/files/.minecraft", "HMCL 应用数据目录", "HMCL" },
    { "Android/data/net.kdt.pojavlaunch/files/.minecraft", "PojavLauncher 应用数据目录",
      "PojavLauncher" },
    { "Android/data/com.silentstudio.sxcl/files/.minecraft", "本应用（共享存储侧）", "本应用" },
};

#define SXCL_ANDROID_DEFAULT_SHARED "/storage/emulated/0"

static void android_push_root(sxcl_android_root *out, size_t cap, size_t *n, const char *path,
                              const char *label, const char *owner)
{
    if (*n >= cap || path == NULL || path[0] == '\0') {
        return;
    }
    (void)memset(&out[*n], 0, sizeof(out[*n]));
    paths_copy(out[*n].path, sizeof(out[*n].path), path);
    paths_copy(out[*n].label, sizeof(out[*n].label), (label != NULL) ? label : "");
    paths_copy(out[*n].owner, sizeof(out[*n].owner), (owner != NULL) ? owner : "");
    ++(*n);
}

size_t sxcl_paths_android_roots(const char *files_dir, const char *shared_root,
                                sxcl_android_root *out, size_t cap)
{
    size_t n = 0;
    size_t i = 0;
    char shared[SXCL_PATHS_PATH_MAX];
    if (out == NULL || cap == 0) {
        return 0;
    }
    /* 1) 本应用自己的私有目录 —— 唯一**一定能读**的那个 */
    if (files_dir != NULL && files_dir[0] != '\0') {
        char mine[SXCL_PATHS_PATH_MAX];
        if (paths_join(mine, sizeof(mine), files_dir, ".minecraft")) {
            android_push_root(out, cap, &n, mine, "本应用（私有目录）", "本应用");
        }
    }
    /* 2) 共享存储上的各家目录 */
    paths_copy(shared, sizeof(shared),
               (shared_root != NULL && shared_root[0] != '\0') ? shared_root
                                                                : SXCL_ANDROID_DEFAULT_SHARED);
    for (i = 0; i < sizeof(kAndroidRelRoots) / sizeof(kAndroidRelRoots[0]); ++i) {
        char path[SXCL_PATHS_PATH_MAX];
        if (!paths_join(path, sizeof(path), shared, kAndroidRelRoots[i].rel)) {
            continue;
        }
        android_push_root(out, cap, &n, path, kAndroidRelRoots[i].label, kAndroidRelRoots[i].owner);
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

