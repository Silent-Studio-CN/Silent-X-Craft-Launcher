/* 游戏目录候选表 / 判定 测试 —— 不联网,夹具全在构建目录下。
 *
 * 覆盖三件事(对应用户点名的 ①):
 *   1) **候选表**:三个桌面平台 + 安卓,候选**逐条**覆盖官方启动器、HMCL、MultiMC/Prism、
 *      CurseForge、ATLauncher、FCL/PojavLauncher、共享存储 —— 纯字符串,注入 env 就能在
 *      任何一台机器上验(Windows 上也能验 Linux/macOS 的规则,反过来也一样);
 *   2) **判定**:sxcl_paths_marks 的"像不像 MC 目录"判据(versions/libraries/assets/
 *      launcher_profiles/logs),以及 count_versions 对"只有 JSON 的加载器版本"的口径;
 *   3) **排序**:已配置目录最高优先;同优先级才比 score(有版本 > 有资源 > 有 profiles)。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdio.h>
#include <string.h>

#include "io_internal.h"
#include "sxcl/fs.h"
#include "sxcl/paths.h"

#define FIXTURE_ROOT "sxcl_paths_roots_fixture"

#if defined(_WIN32)
#  define HOST_OS SXCL_PATHS_OS_WINDOWS
#else
#  define HOST_OS SXCL_PATHS_OS_LINUX
#endif

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what)
{
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

static void check_int(long long got, long long want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %lld want %lld\n", what, got, want);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)",
               want ? want : "(null)");
    }
}

static void check_contains(const char *hay, const char *needle, const char *what)
{
    if (hay != NULL && needle != NULL && strstr(hay, needle) != NULL) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: '%s' 里找不到 '%s'\n", what, hay ? hay : "(null)",
               needle ? needle : "(null)");
    }
}

/* ── 夹具 ── */

static void put(const char *path, const char *text)
{
    sxcl_fs_mkdirs_for_file(path);
    FILE *fh = sxcl_fs_fopen(path, "wb");
    if (fh == NULL) {
        printf("  [!!] 夹具写不进去: %s\n", path);
        ++g_fail;
        return;
    }
    (void)fputs(text, fh);
    (void)fclose(fh);
}

static void build_fixture(void)
{
    (void)sxcl_dir_remove_tree(FIXTURE_ROOT);
    /* 便携目录:2 个版本 */
    put(FIXTURE_ROOT "/prog/.minecraft/versions/1.20.1/1.20.1.json", "{}");
    put(FIXTURE_ROOT "/prog/.minecraft/versions/1.20.1/1.20.1.jar", "jar");
    put(FIXTURE_ROOT "/prog/.minecraft/versions/1.21.1/1.21.1.json", "{}");
    /* 官方启动器(APPDATA):1 个版本 + assets + launcher_profiles */
    put(FIXTURE_ROOT "/appdata/.minecraft/versions/1.19.2/1.19.2.json", "{}");
    put(FIXTURE_ROOT "/appdata/.minecraft/assets/indexes/1.19.json", "{}");
    put(FIXTURE_ROOT "/appdata/.minecraft/launcher_profiles.json", "{}");
    /* HMCL 数据目录(只有 JSON 的加载器版本也算一个) */
    put(FIXTURE_ROOT "/appdata/HMCL/.minecraft/versions/1.21.11-NeoForge/1.21.11-NeoForge.json", "{}");
    /* Prism:instances/<名字>/minecraft —— 容器根必须被展开 */
    put(FIXTURE_ROOT "/appdata/PrismLauncher/instances/MyPack/minecraft/versions/1.21.4/"
                     "1.21.4.json", "{}");
    put(FIXTURE_ROOT "/appdata/PrismLauncher/instances/MyPack/minecraft/libraries/a.jar", "j");
    /* CurseForge:Instances(首字母大写) */
    put(FIXTURE_ROOT "/home/curseforge/minecraft/Instances/Pack/minecraft/versions/1.18.2/"
                     "1.18.2.json", "{}");
    /* 用户目录下的 .minecraft:空壳(存在但没有 versions/) */
    (void)sxcl_fs_mkdirs(FIXTURE_ROOT "/home/.minecraft");
    /* 已配置目录:3 个版本 */
    put(FIXTURE_ROOT "/configured/versions/1.20.4/1.20.4.json", "{}");
    put(FIXTURE_ROOT "/configured/versions/1.20.4/1.20.4.jar", "jar");
    put(FIXTURE_ROOT "/configured/versions/1.20.2/1.20.2.json", "{}");
    put(FIXTURE_ROOT "/configured/versions/1.20.2/1.20.2.jar", "jar");
    put(FIXTURE_ROOT "/configured/versions/1.16.5/1.16.5.json", "{}");
    put(FIXTURE_ROOT "/configured/versions/1.16.5/1.16.5.jar", "jar");
    put(FIXTURE_ROOT "/configured/logs/latest.log", "log");
}

static const sxcl_paths_root *find_source(const sxcl_paths_root *roots, size_t n, const char *src)
{
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(roots[i].source, src) == 0) {
            return &roots[i];
        }
    }
    return NULL;
}

/* 取某个来源键的路径;来源不存在时给空串 —— 断言失败也**不会**解引用空指针。 */
static const char *src_path(const sxcl_paths_root *roots, size_t n, const char *src)
{
    const sxcl_paths_root *found = find_source(roots, n, src);
    return (found != NULL) ? found->path : "";
}

static int count_source(const sxcl_paths_root *roots, size_t n, const char *src)
{
    int c = 0;
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(roots[i].source, src) == 0) {
            ++c;
        }
    }
    return c;
}

/* ══════════════════════ 1) 候选表:三个桌面平台 ══════════════════════ */

static void test_roots_windows(void)
{
    sxcl_paths_env env;
    sxcl_paths_root roots[SXCL_PATHS_MAX_ROOTS];
    size_t n = 0;
    (void)memset(&env, 0, sizeof(env));
    env.app_data = "C:/Users/x/AppData/Roaming";
    env.local_app_data = "C:/Users/x/AppData/Local";
    env.user_home = "C:/Users/x";
    env.program_dir = "C:/Apps/SXCL";

    n = sxcl_paths_roots(&env, SXCL_PATHS_OS_WINDOWS, roots, SXCL_PATHS_MAX_ROOTS);
    check(n >= 12, "win:候选根至少 12 条(便携+官方+五家第三方+用户目录+桌面)");
    check_str(roots[0].source, "Portable", "win:便携目录排第一");
    check_contains(roots[0].path, "Apps/SXCL/.minecraft", "win:便携候选拼对");
    check_int(roots[0].priority, SXCL_PATHS_PRIORITY_PORTABLE, "win:便携优先级");

    check(find_source(roots, n, "APPDATA") != NULL, "win:官方启动器(%APPDATA%\\.minecraft)在候选里");
    check_contains(src_path(roots, n, "APPDATA"), "Roaming/.minecraft", "win:官方候选路径");
    check(find_source(roots, n, "HMCL") != NULL, "win:HMCL 在候选里");
    check(find_source(roots, n, "MultiMC") != NULL, "win:MultiMC 在候选里");
    check(find_source(roots, n, "Prism") != NULL, "win:Prism 在候选里");
    check(find_source(roots, n, "CurseForge") != NULL, "win:CurseForge 在候选里");
    check(find_source(roots, n, "ATLauncher") != NULL, "win:ATLauncher 在候选里");
    check(find_source(roots, n, "Desktop") != NULL, "win:桌面在候选里");
    check(find_source(roots, n, "UserHome") != NULL, "win:用户目录在候选里");

    check_int(find_source(roots, n, "MultiMC")->expand, 1, "win:MultiMC 是容器根(要展开实例)");
    check_int(find_source(roots, n, "Prism")->expand, 1, "win:Prism 是容器根");
    check_int(find_source(roots, n, "CurseForge")->expand, 1, "win:CurseForge 是容器根");
    check_int(find_source(roots, n, "ATLauncher")->expand, 1, "win:ATLauncher 是容器根");
    check_int(find_source(roots, n, "HMCL")->expand, 0, "win:HMCL 的 .minecraft 不是容器根");

    check(find_source(roots, n, "APPDATA")->priority > find_source(roots, n, "HMCL")->priority,
          "win:官方启动器优先级高于第三方");
    check_str(find_source(roots, n, "HMCL")->owner, "HMCL", "win:HMCL 的归属");
    check_str(find_source(roots, n, "Prism")->owner, "Prism", "win:Prism 的归属");

    check_int((long long)sxcl_paths_roots(&env, SXCL_PATHS_OS_WINDOWS, NULL, 0), 0,
              "win:out=NULL 返回 0(不炸)");
}

static void test_roots_linux_macos(void)
{
    sxcl_paths_env env;
    sxcl_paths_root roots[SXCL_PATHS_MAX_ROOTS];
    size_t n = 0;
    (void)memset(&env, 0, sizeof(env));
    env.user_home = "/home/x";
    env.program_dir = "/opt/sxcl";
    env.xdg_data_home = "/home/x/.local/share";

    n = sxcl_paths_roots(&env, SXCL_PATHS_OS_LINUX, roots, SXCL_PATHS_MAX_ROOTS);
    check(find_source(roots, n, "UserHome") != NULL, "linux:~/.minecraft 在候选里");
    check_contains(src_path(roots, n, "UserHome"), "/home/x/.minecraft",
                   "linux:用户目录候选拼对");
    check(find_source(roots, n, "Flatpak") != NULL, "linux:Flatpak 沙箱里的官方启动器在候选里");
    check(find_source(roots, n, "MultiMC") != NULL, "linux:~/.local/share/multimc 在候选里");
    check(find_source(roots, n, "Prism") != NULL, "linux:Prism 在候选里");
    check(find_source(roots, n, "ATLauncher") != NULL, "linux:ATLauncher 在候选里");
    check(find_source(roots, n, "CurseForge") != NULL, "linux:CurseForge 在候选里");
    check_contains(src_path(roots, n, "Prism"), "/home/x/.local/share/PrismLauncher",
                   "linux:Prism 用 XDG 数据目录");
    check_contains(src_path(roots, n, "MultiMC"), "/multimc", "linux:MultiMC 数据目录候选");
    check(find_source(roots, n, "Flatpak")->priority >= SXCL_PATHS_PRIORITY_USER_HOME,
          "linux:Flatpak 候选不是最低优先级");

    /* XDG_DATA_HOME 没给时必须回落到 ~/.local/share */
    env.xdg_data_home = NULL;
    n = sxcl_paths_roots(&env, SXCL_PATHS_OS_LINUX, roots, SXCL_PATHS_MAX_ROOTS);
    check_contains(src_path(roots, n, "Prism"), "/home/x/.local/share/PrismLauncher",
                   "linux:没给 XDG_DATA_HOME 时回落到 ~/.local/share");

    n = sxcl_paths_roots(&env, SXCL_PATHS_OS_MACOS, roots, SXCL_PATHS_MAX_ROOTS);
    check(find_source(roots, n, "AppSupport") != NULL, "macos:Application Support 在候选里");
    check_contains(src_path(roots, n, "AppSupport"),
                   "Library/Application Support/minecraft", "macos:官方候选路径");
    check(find_source(roots, n, "HMCL") != NULL, "macos:HMCL 在候选里");
    check(find_source(roots, n, "MultiMC") != NULL, "macos:MultiMC 在候选里");
    check(find_source(roots, n, "Prism") != NULL, "macos:Prism 在候选里");
    check(find_source(roots, n, "CurseForge") != NULL, "macos:CurseForge 在候选里");
    check(find_source(roots, n, "ATLauncher") != NULL, "macos:ATLauncher 在候选里");
    check(find_source(roots, n, "Desktop") != NULL, "macos:桌面在候选里");
}

/* ══════════════════════ 2) 候选表:安卓(共享存储 + 私有目录) ══════════ */

static void test_roots_android(void)
{
    sxcl_paths_env env;
    sxcl_paths_root roots[SXCL_PATHS_MAX_ROOTS];
    size_t n = 0;
    int saw_sdcard = 0;
    int saw_pkg = 0;
    (void)memset(&env, 0, sizeof(env));
    env.android_files = "/data/user/0/com.silentstudio.sxcl/files";
    env.android_shared = "/storage/emulated/0";

    n = sxcl_paths_roots(&env, SXCL_PATHS_OS_ANDROID, roots, SXCL_PATHS_MAX_ROOTS);
    check_int((long long)n, 10, "android:1 个私有目录 + 9 个共享存储候选");
    check_contains(roots[0].path, "/files/.minecraft", "android:私有目录排第一");
    check_str(roots[0].owner, "本应用", "android:私有目录归属");
    for (size_t i = 0; i < n; ++i) {
        if (strstr(roots[i].path, "/.minecraft") != NULL &&
            strstr(roots[i].path, "/storage/emulated/0/.minecraft") != NULL) {
            saw_sdcard = 1;
        }
        if (strstr(roots[i].path, "/Android/data/") != NULL) {
            saw_pkg = 1;
            check(roots[i].owner[0] != '\0', "android:Android/data 候选带归属");
        }
    }
    check(saw_sdcard == 1, "android:/storage/emulated/0/.minecraft 在候选里");
    check(saw_pkg == 1, "android:Android/data/<pkg>/files 在候选里");
    check(find_source(roots, n, "AndroidPrivate") != NULL, "android:私有目录来源键存在");
    check_str(src_path(roots, n, "AndroidPrivate"), roots[0].path, "android:私有目录路径一致");
    check(find_source(roots, n, "AndroidShared") != NULL, "android:共享存储来源键存在");

    /* 与设备实测的那份老口子必须逐条一致(顺序也一致) */
    {
        sxcl_android_root legacy[20];
        size_t ln = sxcl_paths_android_roots(env.android_files, env.android_shared, legacy, 20);
        check_int((long long)ln, (long long)n, "android:老口子条数一致");
        for (size_t i = 0; i < ln && i < n; ++i) {
            check_str(legacy[i].path, roots[i].path, "android:老口子路径与统一候选表一致");
            check_str(legacy[i].label, roots[i].label, "android:老口子标签与统一候选表一致");
        }
    }

    /* 不给私有目录时只剩共享存储 */
    env.android_files = NULL;
    n = sxcl_paths_roots(&env, SXCL_PATHS_OS_ANDROID, roots, SXCL_PATHS_MAX_ROOTS);
    check_int((long long)n, 9, "android:没有私有目录时只剩 9 条共享存储");
}

/* ══════════════════════ 3) 容器根展开(实例家族) ══════════════════════ */

static void test_expand(void)
{
    sxcl_paths_root in[3];
    sxcl_paths_root out[16];
    size_t n = 0;
    (void)memset(in, 0, sizeof(in));
    /* 用夹具里的 Prism 数据目录当"容器根"(host 分隔符,任意平台都能跑) */
    strcpy(in[0].path, FIXTURE_ROOT "/appdata/PrismLauncher");
    strcpy(in[0].label, "Prism Launcher 数据目录");
    strcpy(in[0].owner, "Prism");
    strcpy(in[0].source, "Prism");
    in[0].priority = SXCL_PATHS_PRIORITY_THIRD_PARTY;
    in[0].expand = 1;
    /* CurseForge 用 Instances(首字母大写) */
    strcpy(in[1].path, FIXTURE_ROOT "/home/curseforge/minecraft");
    strcpy(in[1].label, "CurseForge 数据目录");
    strcpy(in[1].source, "CurseForge");
    in[1].priority = SXCL_PATHS_PRIORITY_THIRD_PARTY;
    in[1].expand = 1;
    /* 不是容器:必须原样保留 */
    strcpy(in[2].path, FIXTURE_ROOT "/appdata/.minecraft");
    strcpy(in[2].label, "官方启动器");
    strcpy(in[2].source, "APPDATA");
    in[2].priority = SXCL_PATHS_PRIORITY_OFFICIAL;
    in[2].expand = 0;

    n = sxcl_paths_expand_roots(in, 3, out, 16);
    check_int((long long)n, 3, "expand:1 个 Prism 实例 + 1 个 CF 实例 + 1 条普通根");
    check_contains(out[0].path, "instances/MyPack/minecraft", "expand:Prism 实例展开到 minecraft/");
    check_contains(out[0].label, "MyPack", "expand:实例标签带名字");
    check_str(out[0].owner, "Prism", "expand:展开后归属不变");
    check_int(out[0].priority, SXCL_PATHS_PRIORITY_THIRD_PARTY, "expand:展开后优先级不变");
    check_int(out[0].expand, 0, "expand:展开后不再是容器根");
    /* CurseForge 用的是 Instances/(首字母大写)。Windows 的文件系统大小写不敏感,
     * 所以拼出来的字符串可能是小写那个 —— 只断言"展开到了这个实例",不锁死大小写。 */
    check_contains(out[1].path, "Pack/minecraft", "expand:CurseForge 的 Instances 也认");
    check_str(out[1].source, "CurseForge", "expand:CurseForge 实例的来源键");
    check_str(out[2].source, "APPDATA", "expand:非容器根原样保留");

    /* 空容器根:展开不出东西也要留着(它自己可能就是游戏目录) */
    (void)sxcl_fs_mkdirs(FIXTURE_ROOT "/empty-launcher/instances");
    (void)memset(in, 0, sizeof(in));
    strcpy(in[0].path, FIXTURE_ROOT "/empty-launcher");
    strcpy(in[0].source, "Prism");
    in[0].expand = 1;
    n = sxcl_paths_expand_roots(in, 1, out, 16);
    check_int((long long)n, 1, "expand:空容器根保留 1 条");
    check_contains(out[0].path, "empty-launcher", "expand:保留的是容器根本身");

    check_int((long long)sxcl_paths_expand_roots(NULL, 3, out, 16), 0, "expand:roots=NULL 返回 0");
    check_int((long long)sxcl_paths_expand_roots(in, 1, NULL, 0), 0, "expand:out=NULL 返回 0");
}

/* ══════════════════════ 4) 判定:像不像 MC 目录 ══════════════════════ */

static void test_marks(void)
{
    char text[SXCL_PATHS_DESC_MAX];
    const int marks_cfg = sxcl_paths_marks(FIXTURE_ROOT "/configured");
    check((marks_cfg & SXCL_PATHS_MARK_VERSIONS) != 0, "marks:有 versions/");
    check((marks_cfg & SXCL_PATHS_MARK_LOGS) != 0, "marks:有 logs/");
    check((marks_cfg & SXCL_PATHS_MARK_ASSETS) == 0, "marks:没有 assets/");

    const int marks_off = sxcl_paths_marks(FIXTURE_ROOT "/appdata/.minecraft");
    check((marks_off & SXCL_PATHS_MARK_ASSETS) != 0, "marks:官方目录有 assets/");
    check((marks_off & SXCL_PATHS_MARK_PROFILES) != 0, "marks:官方目录有 launcher_profiles.json");

    const int marks_priv = sxcl_paths_marks(FIXTURE_ROOT "/appdata/PrismLauncher/instances/"
                                                          "MyPack/minecraft");
    check((marks_priv & SXCL_PATHS_MARK_LIBRARIES) != 0, "marks:Prism 实例有 libraries/");

    check_int(sxcl_paths_marks(FIXTURE_ROOT "/home/.minecraft"), 0, "marks:空壳目录没有任何判据");
    check_int(sxcl_paths_marks(NULL), 0, "marks:NULL 返回 0");

    check_int(sxcl_paths_marks_text(marks_cfg, 3, text, sizeof(text)), SXCL_PATHS_OK,
              "marks_text:成功");
    check_contains(text, "3 个版本", "marks_text:带版本数");
    check_contains(text, "logs", "marks_text:列出 logs/");
    check_int(sxcl_paths_marks_text(0, 0, text, sizeof(text)), SXCL_PATHS_OK, "marks_text:空目录");
    check_contains(text, "空目录", "marks_text:空目录给人话");
    check_int(sxcl_paths_marks_text(marks_cfg, 3, NULL, 0), SXCL_PATHS_ERR_ARG,
              "marks_text:out=NULL 报参数错");
    check_int(sxcl_paths_marks_text(marks_cfg, 3, text, 4), SXCL_PATHS_ERR_SPACE,
              "marks_text:缓冲不够报 SPACE");
}

/* ══════════════════════ 5) 探测 + 排序(已配置最高优先) ══════════════ */

static void test_detect_ex(void)
{
    sxcl_paths_env env;
    sxcl_game_folders folders;
    char err[SXCL_PATHS_ERROR_MAX];
    (void)memset(&env, 0, sizeof(env));
    env.app_data = FIXTURE_ROOT "/appdata";
    env.user_home = FIXTURE_ROOT "/home";
    env.program_dir = FIXTURE_ROOT "/prog";

    err[0] = '\0';
    check_int(sxcl_paths_detect_ex(&env, HOST_OS, &folders, FIXTURE_ROOT "/configured", err,
                                   sizeof(err)),
              SXCL_PATHS_OK, "detect_ex:返回成功");
    check(folders.count >= 5, "detect_ex:至少 5 条真实存在的候选");
    check_str(folders.items[0].label, "当前配置", "detect_ex:已配置目录排在第一位");
    check_int(folders.items[0].priority, SXCL_PATHS_PRIORITY_CONFIGURED,
              "detect_ex:已配置目录的优先级");
    check_int(folders.items[0].versions, 3, "detect_ex:已配置目录 3 个版本");
    check_contains(folders.items[0].describe, "3 个版本", "detect_ex:人话描述");
    check_int(folders.items[0].has_launcher_profiles, 0, "detect_ex:没有 launcher_profiles");
    check_contains(folders.items[0].path, "configured", "detect_ex:路径就是配置的那个");

    /* 便携 > 官方 > 第三方;同优先级再比 score */
    {
        int idx_portable = -1;
        int idx_official = -1;
        int idx_third = -1;
        int idx_home = -1;
        for (size_t i = 0; i < folders.count; ++i) {
            if (strcmp(folders.items[i].source, "Portable") == 0 && idx_portable < 0) {
                idx_portable = (int)i;
            }
            if (strcmp(folders.items[i].source, "APPDATA") == 0 && idx_official < 0) {
                idx_official = (int)i;
            }
            if ((strcmp(folders.items[i].source, "Prism") == 0 ||
                 strcmp(folders.items[i].source, "CurseForge") == 0) && idx_third < 0) {
                idx_third = (int)i;
            }
            if (strcmp(folders.items[i].source, "UserHome") == 0 && idx_home < 0) {
                idx_home = (int)i;
            }
        }
        check(idx_portable > 0, "detect_ex:便携目录在列表里");
        check(idx_official > 0, "detect_ex:官方目录在列表里");
        check(idx_third > 0, "detect_ex:容器根展开出来的实例在列表里");
        check(idx_home > 0, "detect_ex:空壳的用户目录也在列表里(存在就列)");
        check(idx_portable < idx_official, "detect_ex:便携排在官方之前");
        check(idx_official < idx_third, "detect_ex:官方排在第三方之前");
        check(idx_third < idx_home, "detect_ex:第三方排在空壳用户目录之前");
        check_int(folders.items[idx_portable].versions, 2, "detect_ex:便携目录 2 个版本");
        check_int(folders.items[idx_official].has_assets, 1, "detect_ex:官方目录有 assets");
        check_int(folders.items[idx_official].score, 10 + 5 + 2, "detect_ex:官方目录的 score");
    }

    /* 择优:已配置永远是答案(哪怕它一个版本都没有) */
    check_contains(sxcl_paths_best(&folders)->path, "configured",
                   "detect_ex:best 取已配置目录");

    /* 没有已配置时:按 score 取第一个有版本的 */
    check_int(sxcl_paths_detect_ex(&env, HOST_OS, &folders, NULL, err, sizeof(err)),
              SXCL_PATHS_OK, "detect_ex:没有配置也返回成功");
    check(folders.count >= 4, "detect_ex:没有配置时仍有候选");
    check_contains(folders.items[0].path, "prog", "detect_ex:便携(2 个版本)排第一");
    check_contains(sxcl_paths_best(&folders)->path, "prog", "detect_ex:best 取版本最多的");

    /* 配置了一个不存在的目录:不算错误,但**不进列表**(口径与安卓那路一致:
     * 列表 = 真实存在的候选;配置目录在不在由 resolve_game_dir / inspect 如实回答)。 */
    check_int(sxcl_paths_detect_ex(&env, HOST_OS, &folders, FIXTURE_ROOT "/nope", err,
                                   sizeof(err)),
              SXCL_PATHS_OK, "detect_ex:配置目录不存在也返回成功");
    for (size_t i = 0; i < folders.count; ++i) {
        check_int(folders.items[i].exists, 1, "detect_ex:列表里只有真实存在的目录");
        check(strstr(folders.items[i].path, "/nope") == NULL,
              "detect_ex:不存在的配置目录不进列表");
    }
    /* 但它必须还能被单独体检出来(exists=0 / score=-1 / 人话说明),设置页靠这个提示用户 */
    {
        sxcl_game_folder probe;
        check_int(sxcl_paths_inspect_full(FIXTURE_ROOT "/nope", "当前配置", "", &probe),
                  SXCL_PATHS_OK, "detect_ex:配置目录能被单独体检");
        check_int(probe.exists, 0, "detect_ex:单独体检如实报不存在");
        check_int(probe.score, -1, "detect_ex:不存在的 score = -1");
        check_contains(probe.describe, "目录不存在", "detect_ex:单独体检给人话");
    }

    /* 环境全空:不能崩,count=0 + 人话 */
    (void)memset(&env, 0, sizeof(env));
    err[0] = '\0';
    check_int(sxcl_paths_detect_ex(&env, HOST_OS, &folders, NULL, err, sizeof(err)),
              SXCL_PATHS_OK, "detect_ex:环境全空也返回成功");
    check_int((long long)folders.count, 0, "detect_ex:环境全空时 0 条候选");
    check_contains(err, "没找到游戏目录", "detect_ex:环境全空时有人话说明");
    check_int(sxcl_paths_detect_ex(NULL, HOST_OS, &folders, NULL, err, sizeof(err)), SXCL_PATHS_OK,
              "detect_ex:env=NULL 用空环境(不炸)");
    check_int(sxcl_paths_detect_ex(&env, HOST_OS, NULL, NULL, err, sizeof(err)),
              SXCL_PATHS_ERR_ARG, "detect_ex:folders=NULL 报参数错");
}

int main(void)
{
    build_fixture();

    printf("[paths] 1) 候选表:Windows\n");
    test_roots_windows();
    printf("[paths] 2) 候选表:Linux / macOS\n");
    test_roots_linux_macos();
    printf("[paths] 3) 候选表:Android(共享存储 + 私有目录)\n");
    test_roots_android();
    printf("[paths] 4) 容器根展开(实例家族)\n");
    test_expand();
    printf("[paths] 5) 判定:像不像 MC 目录\n");
    test_marks();
    printf("[paths] 6) 探测 + 排序(已配置最高优先)\n");
    test_detect_ex();

    (void)sxcl_dir_remove_tree(FIXTURE_ROOT);
    printf("\n[paths] 通过 %d,失败 %d\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}