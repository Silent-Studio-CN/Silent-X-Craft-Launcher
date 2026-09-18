/* Android 适配层测试:挂载表解析 / 路径体检 / 安卓候选表 / 安卓诊断。
 *
 * 夹具是**设备原文**:下面的 kMounts 是 192.168.220.33(Android 16 / arm64-v8a,平板)
 * 上 adb shell cat /proc/self/mounts 的筛选摘录,一个字没改 —— 这样"共享存储是 noexec"
 * 这条结论在单测里也是可复现的,而不是只写在文档里。
 *
 * 目录夹具建在构建目录下(ctest 的 WORKING_DIRECTORY 就是本模块的构建目录,已被 .gitignore
 * 忽略:仓库里不留二进制/临时夹具)。
 */
#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdio.h>
#include <string.h>

#include "io_internal.h"
#include "sxcl/android.h"
#include "sxcl/fs.h"
#include "sxcl/paths.h"

/* ── 设备原文(192.168.220.33,Android 16,摘录)──
 * /storage/emulated 那一行是 fuse 且**带 noexec** —— 这就是"sdcard 上的程序起不来"的根因;
 * /data 与 /data/user/0 是 f2fs,**没有** noexec —— 所以应用私有目录里的 Java 能跑。
 * /mnt/pass_through/0/emulated 故意留着:它挂在 /mnt 之下却是另一个挂载点(且不带 noexec),
 * 用来验证"取最长前缀"是对的(不长眼的实现会误判成 noexec)。
 */
static const char *const kMounts =
    "/dev/block/dm-8 / erofs ro,seclabel,relatime,user_xattr,acl,cache_strategy=readaround 0 0\n"
    "tmpfs /dev tmpfs rw,seclabel,nosuid,relatime,size=3946684k,nr_inodes=986671,mode=755 0 0\n"
    "tmpfs /mnt tmpfs rw,seclabel,nosuid,nodev,noexec,relatime,size=3946684k,mode=755,gid=1000 0 0\n"
    "tmpfs /mnt/installer tmpfs rw,seclabel,nosuid,nodev,noexec,relatime,size=3946684k 0 0\n"
    "/dev/block/sdc15 /metadata f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,user_xattr,acl 0 0\n"
    "/dev/block/dm-54 /data f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,user_xattr,acl,inline_data 0 0\n"
    "/dev/block/dm-54 /data/user/0 f2fs rw,lazytime,seclabel,nosuid,nodev,noatime,user_xattr,acl 0 0\n"
    "/dev/fuse /mnt/user/0/emulated fuse rw,lazytime,nosuid,nodev,noexec,noatime,user_id=0,group_id=0,allow_other 0 0\n"
    "/dev/fuse /storage/emulated fuse rw,lazytime,nosuid,nodev,noexec,noatime,user_id=0,group_id=0,allow_other 0 0\n"
    "/dev/block/dm-54 /mnt/pass_through/0/emulated f2fs rw,lazytime,seclabel,nosuid,nodev,noatime 0 0\n";

#define FIXTURE_ROOT "sxcl_android_fixture"
#define FILES_DIR FIXTURE_ROOT "/files"
#define SHARED_DIR FIXTURE_ROOT "/shared"

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
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)");
    }
}

static void check_contains(const char *hay, const char *needle, const char *what)
{
    if (hay != NULL && needle != NULL && strstr(hay, needle) != NULL) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: '%s' 里找不到 '%s'\n", what, hay ? hay : "(null)", needle ? needle : "(null)");
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
    /* 本应用私有目录里的 .minecraft(空壳:能读,但不是游戏目录) */
    (void)sxcl_fs_mkdirs(FILES_DIR "/.minecraft");
    /* FCL 在共享存储上的游戏目录:3 个版本(其中一个是只有 JSON 的加载器版本) */
    put(SHARED_DIR "/FCL/.minecraft/versions/1.20.1/1.20.1.json", "{}");
    put(SHARED_DIR "/FCL/.minecraft/versions/1.20.1/1.20.1.jar", "jar");
    put(SHARED_DIR "/FCL/.minecraft/versions/1.21.11-NeoForge/1.21.11-NeoForge.json", "{}");
    put(SHARED_DIR "/FCL/.minecraft/versions/26.2/26.2.json", "{}");
    put(SHARED_DIR "/FCL/.minecraft/launcher_profiles.json", "{}");
    /* HMCL 在共享存储上的游戏目录:1 个版本 */
    put(SHARED_DIR "/HMCL/.minecraft/versions/1.21.1/1.21.1.json", "{}");
    /* 一个普通文件(没有执行位),用来验证 want_exec 两种口径 */
    put(FIXTURE_ROOT "/plain.txt", "x");
}

/* ══════════════════════ 1) 挂载表解析(设备原文) ══════════════════════ */

static void test_mounts(void)
{
    /* 共享存储是 noexec —— 这两条就是"共享存储上的程序起不来"的硬依据 */
    check_int(sxcl_android_noexec_in_mounts(kMounts, "/storage/emulated/0/FCL/.minecraft"), 1,
              "挂载表:/storage/emulated 带 noexec");
    check_int(sxcl_android_noexec_in_mounts(
                  kMounts, "/storage/emulated/0/Android/data/com.tungsten.fcl/files/exec_probe"),
              1, "挂载表:共享存储上的 Android/data 也是 noexec");
    check_int(sxcl_android_noexec_in_mounts(kMounts, "/mnt/anywhere"), 1,
              "挂载表:/mnt 这个 tmpfs 也带 noexec");

    /* 应用私有目录在 /data 上,**没有** noexec —— 所以自家 Java 能跑 */
    check_int(sxcl_android_noexec_in_mounts(
                  kMounts, "/data/data/com.tungsten.fcl/app_runtime/java/jre25/bin/java"),
              0, "挂载表:/data 不带 noexec");
    check_int(sxcl_android_noexec_in_mounts(
                  kMounts, "/data/user/0/com.silentstudio.sxcl/files/runtime/jre/bin/java"),
              0, "挂载表:本应用私有目录不带 noexec");

    /* 最长前缀:/mnt/pass_through/0/emulated 比 /mnt 更长,且它不带 noexec */
    check_int(sxcl_android_noexec_in_mounts(kMounts, "/mnt/pass_through/0/emulated/x"), 0,
              "挂载表:取最长前缀(/mnt/pass_through/0/emulated 盖过 /mnt)");
    /* 按路径分量匹配:/database 不能被 /data 命中(命中就说明前缀比较写错了) */
    check_int(sxcl_android_noexec_in_mounts(kMounts, "/database/x"), 0,
              "挂载表:/database 不匹配 /data");

    /* 找不到挂载点 / 空输入 */
    check_int(sxcl_android_noexec_in_mounts("", "/anything"), -1, "挂载表:空文本给 -1");
    check_int(sxcl_android_noexec_in_mounts(NULL, "/anything"), -1, "挂载表:NULL 文本给 -1");
    check_int(sxcl_android_noexec_in_mounts(kMounts, "relative/path"), -1, "挂载表:相对路径给 -1");
    check_int(sxcl_android_noexec_in_mounts(kMounts, ""), -1, "挂载表:空路径给 -1");
}

/* ══════════════════════ 2) 路径体检 ══════════════════════ */

static void test_probe(void)
{
    char reason[256];
    sxcl_android_access rc;

    reason[0] = '\0';
    rc = sxcl_android_probe_path_with_mounts(kMounts, FIXTURE_ROOT, 0, reason, sizeof(reason));
    check_int(rc, SXCL_ANDROID_OK, "体检:存在的目录 = OK");
    check_contains(reason, "可以正常使用", "体检:OK 有人话原因");

    rc = sxcl_android_probe_path_with_mounts(kMounts, FIXTURE_ROOT "/missing-xyz", 0, reason,
                                             sizeof(reason));
    check_int(rc, SXCL_ANDROID_MISSING, "体检:不存在的目录 = MISSING");
    check_contains(reason, "不存在", "体检:MISSING 有人话原因");

    rc = sxcl_android_probe_path_with_mounts(kMounts, NULL, 0, reason, sizeof(reason));
    check_int(rc, SXCL_ANDROID_MISSING, "体检:路径为空 = MISSING(不是崩溃)");

    /* 真文件:读得到 —— want_exec=0 时必须 OK;want_exec=1 时看平台执行位 */
    rc = sxcl_android_probe_path_with_mounts(kMounts, FIXTURE_ROOT "/plain.txt", 1, reason,
                                             sizeof(reason));
    check(rc == SXCL_ANDROID_NOT_EXECUTABLE || rc == SXCL_ANDROID_OK,
          "体检:普通文件要么没执行位、要么本来就能执行(Windows 没有执行位概念)");
    rc = sxcl_android_probe_path_with_mounts(kMounts, FIXTURE_ROOT "/plain.txt", 0, reason,
                                             sizeof(reason));
    check_int(rc, SXCL_ANDROID_OK, "体检:同一个文件在 want_exec=0 时是 OK");

    /* 结论表:名字/键/建议都不能是空的,键要稳定(日志与测试靠它) */
    check_str(sxcl_android_access_key(SXCL_ANDROID_DENIED), "denied", "结论键:denied");
    check_str(sxcl_android_access_key(SXCL_ANDROID_NOEXEC), "noexec", "结论键:noexec");
    check_str(sxcl_android_access_key(SXCL_ANDROID_OK), "ok", "结论键:ok");
    check_str(sxcl_android_access_name(SXCL_ANDROID_DENIED), "沙箱拒绝", "结论名:沙箱拒绝");
    check_str(sxcl_android_access_name(SXCL_ANDROID_NOEXEC), "共享存储不能执行", "结论名:noexec");
    check_contains(sxcl_android_access_hint(SXCL_ANDROID_DENIED), "私有目录",
                   "建议:沙箱拒绝要说清是私有目录");
    check_contains(sxcl_android_access_hint(SXCL_ANDROID_DENIED), "沙箱",
                   "建议:沙箱拒绝要说清是沙箱");
    check_contains(sxcl_android_access_hint(SXCL_ANDROID_NOEXEC), "noexec",
                   "建议:noexec 要说出根因");
    check(sxcl_android_access_name(SXCL_ANDROID_ACCESS_COUNT)[0] != '\0',
          "结论表:计数项也有名字(不会返回 NULL)");
}

/* ══════════════════════ 3) 安卓候选表 ══════════════════════ */

static void test_roots(void)
{
    sxcl_android_root roots[20];
    size_t n = 0;
    size_t i = 0;
    int saw_fcl_shared = 0;
    int saw_fcl_appdata = 0;
    int saw_hmcl = 0;

    n = sxcl_paths_android_roots(FILES_DIR, SHARED_DIR, roots, 20);
    check_int((long long)n, 10, "候选表:1 个私有 + 9 个共享存储 = 10 条");
    check_contains(roots[0].path, "files/.minecraft", "候选表:私有目录排第一");
    check_str(roots[0].owner, "本应用", "候选表:私有目录的归属");

    for (i = 0; i < n; ++i) {
        if (strstr(roots[i].path, "shared/FCL/.minecraft") != NULL) {
            saw_fcl_shared = 1;
            check_str(roots[i].owner, "FCL", "候选表:FCL 归属");
            check_contains(roots[i].label, "FCL", "候选表:FCL 标签");
        }
        if (strstr(roots[i].path, "Android/data/com.tungsten.fcl/files/.minecraft") != NULL) {
            saw_fcl_appdata = 1;
        }
        if (strstr(roots[i].path, "hmcl") != NULL || strstr(roots[i].path, "HMCL") != NULL) {
            saw_hmcl = 1;
        }
    }
    check(saw_fcl_shared == 1, "候选表:有 FCL 共享存储目录(设备实测就在这)");
    check(saw_fcl_appdata == 1, "候选表:有 FCL 应用数据目录");
    check(saw_hmcl == 1, "候选表:有 HMCL 目录");

    /* 不给根时用默认共享存储根 —— 纯字符串,不碰文件系统 */
    n = sxcl_paths_android_roots(NULL, NULL, roots, 20);
    check_int((long long)n, 9, "候选表:不给 files_dir 时只剩共享存储的 9 条");
    check_contains(roots[0].path, "/storage/emulated/0", "候选表:默认共享存储根");

    check_int((long long)sxcl_paths_android_roots(FILES_DIR, SHARED_DIR, NULL, 0), 0,
              "候选表:cap=0 返回 0(不炸)");
}

/* ══════════════════════ 4) 安卓自动扫描 + 诊断 ══════════════════════ */

static void test_detect_android(void)
{
    sxcl_game_folders folders;
    sxcl_game_probes probes;
    char err[SXCL_PATHS_ERROR_MAX];
    size_t i = 0;
    int saw_config = 0;
    int saw_hmcl = 0;
    int saw_private = 0;
    int saw_missing = 0;
    int saw_found_reason = 0;

    err[0] = '\0';
    /* 用户手动配置优先:哪怕它同时也是平台候选,标签也必须是"当前配置" */
    check_int(sxcl_paths_detect_android(FILES_DIR, SHARED_DIR, &folders,
                                        SHARED_DIR "/FCL/.minecraft", err, sizeof(err)),
              SXCL_PATHS_OK, "安卓探测:返回成功");
    check_int((long long)folders.count, 3, "安卓探测:3 个真实存在的候选");
    check_contains(folders.items[0].path, "FCL/.minecraft", "安卓探测:版本最多的排第一");
    check_int(folders.items[0].versions, 3, "安卓探测:FCL 目录 3 个版本");
    for (i = 0; i < folders.count; ++i) {
        if (strstr(folders.items[i].path, "FCL/.minecraft") != NULL) {
            saw_config = 1;
            check_str(folders.items[i].label, "当前配置", "安卓探测:手动配置的标签");
            check(folders.items[i].has_launcher_profiles == 1, "安卓探测:认得出 launcher_profiles");
        }
        if (strstr(folders.items[i].path, "HMCL") != NULL) {
            saw_hmcl = 1;
            check_str(folders.items[i].owner, "HMCL", "安卓探测:HMCL 的归属");
            check_int(folders.items[i].versions, 1, "安卓探测:HMCL 1 个版本");
        }
        if (strstr(folders.items[i].path, "files/.minecraft") != NULL) {
            saw_private = 1;
            check_int(folders.items[i].versions, 0, "安卓探测:空目录 0 个版本");
        }
    }
    check(saw_config == 1, "安卓探测:当前配置出现在列表里");
    check(saw_hmcl == 1, "安卓探测:HMCL 目录被自动发现(用户反馈的缺口)");
    check(saw_private == 1, "安卓探测:本应用私有目录也在候选里");

    /* 择优:有版本的一定排在空目录前面 */
    check_contains(sxcl_paths_best(&folders)->path, "FCL/.minecraft",
                   "安卓探测:择优取版本最多的");

    /* 只剩私有目录那一条时不算失败;连它都没有才 count=0 + 人话说明 */
    err[0] = '\0';
    check_int(sxcl_paths_detect_android(FILES_DIR, FIXTURE_ROOT "/nothing-here", &folders, NULL,
                                        err, sizeof(err)),
              SXCL_PATHS_OK, "安卓探测:空的共享存储根也返回成功");
    check_int((long long)folders.count, 1, "安卓探测:只剩本应用私有目录那一条");
    err[0] = '\0';
    (void)sxcl_paths_detect_android(NULL, FIXTURE_ROOT "/nothing-here", &folders, NULL, err,
                                    sizeof(err));
    check_int((long long)folders.count, 0, "安卓探测:连私有目录都没有时 count=0");
    check_contains(err, "没找到游戏目录", "安卓探测:没找到时有人话说明");

    /* ── 诊断:每条候选都要有一句人话(设置页要显示这个)── */
    (void)memset(&probes, 0, sizeof(probes));
    check_int((long long)sxcl_paths_probe_android(FILES_DIR, SHARED_DIR,
                                                  SHARED_DIR "/FCL/.minecraft", &probes),
              10, "安卓诊断:1 条配置 + 10 条候选(配置与 FCL 候选同路径,去重后 10)");
    check_str(probes.items[0].label, "当前配置", "安卓诊断:配置排第一");
    check(probes.usable >= 2, "安卓诊断:usable 至少 2(FCL 与 HMCL)");
    for (i = 0; i < probes.count; ++i) {
        check(probes.items[i].reason[0] != '\0', "安卓诊断:每条都有人话原因");
        if (probes.items[i].versions > 0) {
            saw_found_reason = 1;
            check(probes.items[i].exists == 1, "安卓诊断:有版本的必须 exists=1");
            check_contains(probes.items[i].reason, "找到游戏目录", "安卓诊断:找到时说明版本数");
            check_int(probes.items[i].access, SXCL_ANDROID_OK, "安卓诊断:找到的就是可读的");
        } else if (probes.items[i].exists) {
            check_int(probes.items[i].access, SXCL_ANDROID_OK, "安卓诊断:空目录也是可读的");
            check_contains(probes.items[i].reason, "没有 versions/",
                           "安卓诊断:空目录要说清为什么不算游戏目录");
        } else {
            saw_missing = 1;
            check_int(probes.items[i].access, SXCL_ANDROID_MISSING,
                      "安卓诊断:夹具里没建的那些如实报不存在");
            check_contains(probes.items[i].reason, "不存在", "安卓诊断:不存在也有人话");
        }
        check(probes.items[i].hint[0] != '\0', "安卓诊断:每条都有下一步建议(不是只报错)");
    }
    check(saw_found_reason == 1, "安卓诊断:存在 找到游戏目录 这类结论");
    check(saw_missing == 1, "安卓诊断:存在 不存在 这类结论");

    check_int((long long)sxcl_paths_probe_android(NULL, NULL, NULL, NULL), 0,
              "安卓诊断:out=NULL 返回 0(不炸)");
}

int main(void)
{
    build_fixture();

    printf("[android] 1) 挂载表解析(设备 /proc/self/mounts 原文)\n");
    test_mounts();
    printf("[android] 2) 路径体检 + 结论文案\n");
    test_probe();
    printf("[android] 3) 安卓候选表\n");
    test_roots();
    printf("[android] 4) 安卓自动扫描 + 诊断\n");
    test_detect_android();

    printf("\n[android] 通过 %d,失败 %d\n", g_pass, g_fail);
    (void)sxcl_dir_remove_tree(FIXTURE_ROOT);
    return (g_fail == 0) ? 0 : 1;
}
