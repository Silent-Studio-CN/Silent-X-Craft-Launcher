/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#define _CRT_SECURE_NO_WARNINGS 1 /* 测试里用 fopen/fwrite 造夹具,MSVC 会标记弃用 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/launch.h"
#include "sxcl/manifest.h"
#include "sxcl/natives.h"

#define TMP_DIR  "build-b/_natives_tmp"
#define FIX_SRC  TMP_DIR "/src"
#define FIX_SRC2 TMP_DIR "/src2"
#define FIX_JAR  TMP_DIR "/natives-test.jar"
#define FIX_JAR2 TMP_DIR "/natives-test2.jar"
#define FIX_PS1  TMP_DIR "/fixture.ps1"
#define GAME_DIR TMP_DIR "/game"
#define OLD_VER  "1.12.2"
#define NEW_VER  "1.21.4"
#define OLD_VJSON GAME_DIR "/versions/" OLD_VER "/" OLD_VER ".json"
#define NEW_VJSON GAME_DIR "/versions/" NEW_VER "/" NEW_VER ".json"
#define OLD_NAT   GAME_DIR "/versions/" OLD_VER "/" OLD_VER "-natives"
#define NEW_NAT   GAME_DIR "/versions/" NEW_VER "/" NEW_VER "-natives"

static int g_pass = 0;
static int g_fail = 0;

static void check(int ok, const char *what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s\n", what);
    }
}

/** mtime 是 int64(纳秒),别用 long 比 —— Windows 上 long 只有 32 位,截断后两个不同的值也可能"相等"。 */
static void check_i64(long long got, long long want, const char *what) {
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %lld want %lld\n", what, got, want);
    }
}

static void check_int(long got, long want, const char *what) {
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %ld want %ld\n", what, got, want);
    }
}

/* ── 文件小工具(测试专用) ── */

static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(text, 1, strlen(text), f) != strlen(text)) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int read_all(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    size_t got = 0;
    if (!f) {
        buf[0] = '\0';
        return -1;
    }
    got = fread(buf, 1, cap - 1, f);
    buf[got] = '\0';
    fclose(f);
    return (int)got;
}

static int copy_file(const char *src, const char *dst) {
    char buf[8192];
    const int n = read_all(src, buf, sizeof(buf));
    if (n < 0) {
        return -1;
    }
    FILE *f = fopen(dst, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static long long mtime_of(const char *path) {
    int64_t size = 0;
    int64_t mtime = 0;
    if (sxcl_fs_stat(path, &size, &mtime) != 0) {
        return -1;
    }
    return (long long)mtime;
}

/* ── 夹具内容 ── */

static const char kFoo[] = "FOO-DLL-PAYLOAD-0123456789\n";
static const char kBar[] = "BAR-SO-PAYLOAD-abcdefghij\n";
static const char kManifest[] = "Manifest-Version: 1.0\r\nCreated-By: sxcl natives test\r\n";
static const char kExtra[] = "EXTRA-DLL-PAYLOAD-zzzzzzzzzz\n";

static char g_os[16];
static char g_classifier[32];
static char g_jar_rel[256];  /* com/example/demo/1.0/demo-1.0-natives-<os>.jar */
static char g_lib_jar[1024]; /* <game>/libraries/<g_jar_rel> */

static const char *classifier_for_os(const char *os) {
    if (strcmp(os, "windows") == 0) {
        return "natives-windows";
    }
    if (strcmp(os, "osx") == 0) {
        return "natives-macos";
    }
    return "natives-linux";
}

static int setup(void) {
    static const char ps1[] =
        "$ErrorActionPreference = 'Stop'\n"
        "$root = $PSScriptRoot\n"
        "$dst = Join-Path $root 'natives-test.jar'\n"
        "if (Test-Path $dst) { Remove-Item $dst -Force }\n"
        "Compress-Archive -Path (Join-Path (Join-Path $root 'src') '*') -DestinationPath $dst -Force\n"
        "$dst2 = Join-Path $root 'natives-test2.jar'\n"
        "if (Test-Path $dst2) { Remove-Item $dst2 -Force }\n"
        "Compress-Archive -Path (Join-Path (Join-Path $root 'src2') '*') -DestinationPath $dst2 -Force\n"
        "if (-not (Test-Path $dst)) { exit 3 }\n"
        "if (-not (Test-Path $dst2)) { exit 4 }\n"
        "exit 0\n";
    int rc_cmd = 0;

    if (sxcl_fs_mkdirs(FIX_SRC "/sub") != 0 || sxcl_fs_mkdirs(FIX_SRC "/META-INF") != 0) {
        return -1;
    }
    if (sxcl_fs_mkdirs(FIX_SRC2 "/sub") != 0 || sxcl_fs_mkdirs(FIX_SRC2 "/META-INF") != 0) {
        return -1;
    }
    if (write_text(FIX_SRC "/foo.dll", kFoo) != 0 || write_text(FIX_SRC "/sub/bar.so", kBar) != 0 ||
        write_text(FIX_SRC "/META-INF/MANIFEST.MF", kManifest) != 0) {
        return -1;
    }
    if (write_text(FIX_SRC2 "/foo.dll", kFoo) != 0 ||
        write_text(FIX_SRC2 "/sub/bar.so", kBar) != 0 ||
        write_text(FIX_SRC2 "/META-INF/MANIFEST.MF", kManifest) != 0 ||
        write_text(FIX_SRC2 "/extra.dll", kExtra) != 0) {
        return -1;
    }
    if (write_text(FIX_PS1, ps1) != 0) {
        return -1;
    }
    (void)sxcl_fs_remove(FIX_JAR);
    (void)sxcl_fs_remove(FIX_JAR2);
    rc_cmd = system("powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " FIX_PS1);
    if (rc_cmd != 0) {
        rc_cmd = system("pwsh -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " FIX_PS1);
    }
    if (rc_cmd != 0 || !sxcl_fs_exists(FIX_JAR) || !sxcl_fs_exists(FIX_JAR2)) {
#if defined(_WIN32)
        printf("  [!!] Compress-Archive 没生成夹具 jar(Windows 上必须有 PowerShell)\n");
        return -2;
#else
        printf("  [--] 本机没有可用的 PowerShell/Compress-Archive,跳过原生库抽取测试\n");
        return -3;
#endif
    }

    /* 库 jar 落到 <game>/libraries 下(natives.c 只认这个位置,不下载) */
    snprintf(g_lib_jar, sizeof(g_lib_jar), "%s/libraries/%s", GAME_DIR, g_jar_rel);
    if (sxcl_fs_mkdirs_for_file(g_lib_jar) != 0) {
        return -1;
    }
    if (copy_file(FIX_JAR, g_lib_jar) != 0) {
        return -1;
    }
    return 0;
}

static void write_old_json(void) {
    char json[2048];
    snprintf(json, sizeof(json),
             "{"
             "\"id\":\"%s\","
             "\"libraries\":[{"
             "  \"name\":\"com.example:demo:1.0\","
             "  \"downloads\":{\"classifiers\":{\"%s\":{\"path\":\"%s\"}}},"
             "  \"natives\":{\"%s\":\"%s\"},"
             "  \"extract\":{\"exclude\":[\"META-INF/\"]}"
             "}]}",
             OLD_VER, g_classifier, g_jar_rel, g_os, g_classifier);
    check(sxcl_fs_mkdirs(GAME_DIR "/versions/" OLD_VER) == 0, "建老版本目录");
    check(write_text(OLD_VJSON, json) == 0, "写老形态版本 JSON");
}

static void write_new_json(void) {
    char json[2048];
    snprintf(json, sizeof(json),
             "{"
             "\"id\":\"%s\","
             "\"libraries\":[{"
             "  \"name\":\"com.example:demo:1.0:%s\","
             "  \"downloads\":{\"artifact\":{\"path\":\"%s\"}},"
             "  \"extract\":{\"exclude\":[\"META-INF/\"]},"
             "  \"rules\":[{\"action\":\"allow\",\"os\":{\"name\":\"%s\"}}]"
             "}]}",
             NEW_VER, g_classifier, g_jar_rel, g_os);
    check(sxcl_fs_mkdirs(GAME_DIR "/versions/" NEW_VER) == 0, "建新版本目录");
    check(write_text(NEW_VJSON, json) == 0, "写新形态版本 JSON");
}

/* 检查一个解出来的文件内容对不对 */
static void check_file(const char *path, const char *want, const char *what) {
    char buf[512];
    const int n = read_all(path, buf, sizeof(buf));
    if (n == (int)strlen(want) && strcmp(buf, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: 内容不对(读到 %d 字节:'%s')\n", what, n, n >= 0 ? buf : "(打不开)");
    }
}

int main(void) {
    char err[512];
    char path[1024];
    int rc = 0;
    int first_count = 0;
    long long foo_before = 0;
    long long foo_after = 0;
    long long bar_before = 0;

    strcpy(g_os, sxcl_platform_os_name());
    strcpy(g_classifier, classifier_for_os(g_os));
    snprintf(g_jar_rel, sizeof(g_jar_rel), "com/example/demo/1.0/demo-1.0-%s.jar", g_classifier);

    printf("原生库抽取测试(本机 os.name=%s,分类器=%s)\n", g_os, g_classifier);
    {
        const int st = setup();
        if (st == -3) {
            printf("natives 测试: 跳过(没有 PowerShell)\n");
            return 0;
        }
        if (st != 0) {
            printf("夹具准备失败(%d)\n", st);
            return 1;
        }
    }

    /* ── 1) 老形态 ── */
    printf("[1] 老形态(classifiers + natives + extract.exclude)\n");
    write_old_json();
    err[0] = '\0';
    rc = sxcl_natives_prepare(OLD_VJSON, GAME_DIR, OLD_NAT, err, sizeof(err));
    check_int(rc, 0, "老形态抽取返回 0");
    if (rc != 0) {
        printf("      err: %s\n", err);
    }
    first_count = sxcl_natives_last_count();
    check_int(first_count, 2, "解出 2 个文件(foo.dll + sub/bar.so)");
    snprintf(path, sizeof(path), "%s/foo.dll", OLD_NAT);
    check(sxcl_fs_exists(path), "foo.dll 解出来了");
    check_file(path, kFoo, "foo.dll 内容逐字节一致");
    snprintf(path, sizeof(path), "%s/sub/bar.so", OLD_NAT);
    check(sxcl_fs_exists(path), "sub/bar.so 解出来了(相对路径保留)");
    check_file(path, kBar, "sub/bar.so 内容逐字节一致");
    snprintf(path, sizeof(path), "%s/META-INF/MANIFEST.MF", OLD_NAT);
    check(!sxcl_fs_exists(path), "META-INF/MANIFEST.MF 被 extract.exclude 挡住,没有落盘");
    snprintf(path, sizeof(path), "%s/META-INF", OLD_NAT);
    check(!sxcl_fs_is_dir(path), "连 META-INF 目录都没建");

    /* ── 2) 幂等 ── */
    printf("[2] 幂等:再跑一次不该碰任何文件\n");
    snprintf(path, sizeof(path), "%s/foo.dll", OLD_NAT);
    foo_before = mtime_of(path);
    snprintf(path, sizeof(path), "%s/sub/bar.so", OLD_NAT);
    bar_before = mtime_of(path);
    err[0] = '\0';
    rc = sxcl_natives_prepare(OLD_VJSON, GAME_DIR, OLD_NAT, err, sizeof(err));
    check_int(rc, 0, "第二次抽取返回 0");
    check_int(sxcl_natives_last_count(), 2, "第二次计数不变(还是 2)");
    snprintf(path, sizeof(path), "%s/foo.dll", OLD_NAT);
    foo_after = mtime_of(path);
    check(sxcl_fs_exists(path), "第二次之后 foo.dll 还在");
    check_i64(foo_after, foo_before, "foo.dll 的 mtime 没被改写(没白解一遍)");
    snprintf(path, sizeof(path), "%s/sub/bar.so", OLD_NAT);
    check_i64(mtime_of(path), bar_before, "sub/bar.so 的 mtime 没被改写");
    printf("      幂等证据: foo.dll mtime %lld -> %lld(相同)\n", foo_before, foo_after);

    /* ── 3) 新形态 ── */
    printf("[3] 新形态(独立库条目 + :%s 分类器 + extract)\n", g_classifier);
    write_new_json();
    err[0] = '\0';
    rc = sxcl_natives_prepare(NEW_VJSON, GAME_DIR, NEW_NAT, err, sizeof(err));
    check_int(rc, 0, "新形态抽取返回 0");
    if (rc != 0) {
        printf("      err: %s\n", err);
    }
    check_int(sxcl_natives_last_count(), 2, "新形态也解出 2 个文件");
    snprintf(path, sizeof(path), "%s/foo.dll", NEW_NAT);
    check_file(path, kFoo, "新形态 foo.dll 内容一致");
    snprintf(path, sizeof(path), "%s/sub/bar.so", NEW_NAT);
    check_file(path, kBar, "新形态 sub/bar.so 相对路径也保留");
    snprintf(path, sizeof(path), "%s/META-INF/MANIFEST.MF", NEW_NAT);
    check(!sxcl_fs_exists(path), "新形态 META-INF 同样被排除");

    /* ── 4) jar 变了必须重解 ── */
    printf("[4] jar 变了(模拟重新下载)-> 必须重解\n");
    check(copy_file(FIX_JAR2, g_lib_jar) == 0, "把库 jar 换成内容不同的一份(+extra.dll)");
    err[0] = '\0';
    rc = sxcl_natives_prepare(OLD_VJSON, GAME_DIR, OLD_NAT, err, sizeof(err));
    check_int(rc, 0, "换 jar 后抽取返回 0");
    check_int(sxcl_natives_last_count(), 3, "重解出 3 个文件(多了 extra.dll)");
    snprintf(path, sizeof(path), "%s/extra.dll", OLD_NAT);
    check_file(path, kExtra, "extra.dll 内容一致");

    /* ── 5) jar 不存在时的错误信息 ── */
    printf("[5] jar 不存在:错误信息必须指明是哪个库\n");
    {
        char json[1024];
        const char *ghost_json = TMP_DIR "/ghost.json";
        snprintf(json, sizeof(json),
                 "{\"id\":\"ghost\",\"libraries\":[{"
                 "\"name\":\"com.example:ghost:9.9\","
                 "\"downloads\":{\"classifiers\":{\"%s\":{\"path\":\"com/example/ghost/9.9/"
                 "ghost-%s.jar\"}}},"
                 "\"natives\":{\"%s\":\"%s\"}}]}",
                 g_classifier, g_classifier, g_os, g_classifier);
        check(write_text(ghost_json, json) == 0, "写缺 jar 的版本 JSON");
        err[0] = '\0';
        rc = sxcl_natives_prepare(ghost_json, GAME_DIR, TMP_DIR "/ghost-natives", err, sizeof(err));
        check(rc < 0, "缺 jar 时返回失败");
        check(strstr(err, "ghost-") != NULL, "错误里带上缺的 jar 文件名");
        check(strstr(err, "com.example:ghost:9.9") != NULL, "错误里带上库名(知道该补哪个库)");
        check_int(sxcl_natives_last_count(), 0, "失败时计数归 0");
        printf("      错误文本: %s\n", err);
    }

    /* ── 6) 接进启动驱动:dry_run 也要把 natives 准备好 ── */
    printf("[6] 接进启动驱动(dry_run)\n");
    {
        char json[2048];
        sxcl_launch_request req;
        sxcl_launch_result res;
        const char *drv_ver = NEW_VER;
        char drv_json[512];
        snprintf(drv_json, sizeof(drv_json), GAME_DIR "/versions/%s/%s.json", drv_ver, drv_ver);
        snprintf(json, sizeof(json),
                 "{\"id\":\"%s\",\"type\":\"release\","
                 "\"mainClass\":\"net.minecraft.client.main.Main\","
                 "\"javaVersion\":{\"majorVersion\":21},"
                 "\"assetIndex\":{\"id\":\"17\"},"
                 "\"libraries\":[{"
                 "  \"name\":\"com.example:demo:1.0:%s\","
                 "  \"downloads\":{\"artifact\":{\"path\":\"%s\"}},"
                 "  \"extract\":{\"exclude\":[\"META-INF/\"]},"
                 "  \"rules\":[{\"action\":\"allow\",\"os\":{\"name\":\"%s\"}}]"
                 "}]}",
                 drv_ver, g_classifier, g_jar_rel, g_os);
        check(write_text(drv_json, json) == 0, "写驱动用的版本 JSON");
        memset(&req, 0, sizeof(req));
        req.game_dir = GAME_DIR;
        req.version_name = drv_ver;
        req.java_path = TMP_DIR "/no-such-java.exe"; /* dry_run 不会真的执行它 */
        req.dry_run = 1;
        memset(&res, 0, sizeof(res));
        err[0] = '\0';
        rc = sxcl_launch_run(&req, &res, err, sizeof(err));
        check_int(rc, 0, "dry_run 启动驱动返回成功");
        if (rc != 0) {
            printf("      err: %s\n", err);
        }
        check(res.natives_count > 0, "结果里带上了 natives 文件数(count > 0)");
        check(strstr(res.natives_dir, "-natives") != NULL, "结果里带上了 natives 目录");
        snprintf(path, sizeof(path), "%s/foo.dll", res.natives_dir);
        check(sxcl_fs_exists(path), "dry_run 之后 natives 目录里真的有 foo.dll");
        printf("      驱动结果: natives_count=%d dir=%s\n", res.natives_count, res.natives_dir);
    }


    /* ── arch 占位符:1.8.x/1.9.x 的 natives 键(实测事故) ──
     * 启动 PCL 装的 1.8.9-Forge+OptiFine 实例时,库 tv.twitch:twitch-platform:6.5 的
     * natives.windows 是 "natives-windows-${arch}",而 classifiers 里的键是 -64 / -32。
     * 按原样查 → "没有下载路径" → 整个启动被拦下。 */
    {
        char perr[192];
        char key[160];
        perr[0] = '\0';
        const char *both =
            "{\"name\":\"tv.twitch:twitch-platform:6.5\","
            "\"natives\":{\"windows\":\"natives-windows-${arch}\"},"
            "\"downloads\":{\"classifiers\":{"
            "\"natives-windows-32\":{\"path\":\"tv/twitch/twitch-platform/6.5/tp-32.jar\",\"sha1\":\"aa\",\"size\":1},"
            "\"natives-windows-64\":{\"path\":\"tv/twitch/twitch-platform/6.5/tp-64.jar\",\"sha1\":\"bb\",\"size\":2}}}}";
        char buf[900];
        snprintf(buf, sizeof(buf), "%s", both);
        /* 上面那行只是为了让 ${arch} 替换后长度确定;真正解析的是替换后的文本 */
        sxcl_json *doc = sxcl_json_parse(buf, strlen(buf), perr, sizeof(perr));
        check(doc != NULL, "arch 夹具能解析");
        if (doc) {
            const sxcl_json_value *cls =
                sxcl_natives_classifier_of(sxcl_json_root(doc), "windows", key, sizeof(key));
            check(cls != NULL, "arch 占位符:认出了分类器(以前这里是 NULL -> 启动失败)");
            check(strcmp(key, "natives-windows-64") == 0, "  展开后优先 64 位");
            check(strcmp(sxcl_json_get_string(cls, "path", ""),
                         "tv/twitch/twitch-platform/6.5/tp-64.jar") == 0,
                  "  拿到的是 64 位那条");
            sxcl_json_free(doc);
        }
        const char *only32 =
            "{\"name\":\"tv.twitch:twitch-platform:6.5\","
            "\"natives\":{\"windows\":\"natives-windows-${arch}\"},"
            "\"downloads\":{\"classifiers\":{"
            "\"natives-windows-32\":{\"path\":\"tv/twitch/twitch-platform/6.5/tp-32.jar\",\"sha1\":\"aa\",\"size\":1}}}}";
        snprintf(buf, sizeof(buf), "%s", only32);
        doc = sxcl_json_parse(buf, strlen(buf), perr, sizeof(perr));
        check(doc != NULL, "只有 32 位的夹具能解析");
        if (doc) {
            const sxcl_json_value *cls =
                sxcl_natives_classifier_of(sxcl_json_root(doc), "windows", key, sizeof(key));
            check(cls != NULL, "只有 32 位时也能认出来");
            check(strcmp(key, "natives-windows-32") == 0, "  退到 32 位");
            sxcl_json_free(doc);
        }
        const char *plain =
            "{\"name\":\"org.lwjgl:lwjgl:2.9.4\","
            "\"natives\":{\"windows\":\"natives-windows\"},"
            "\"downloads\":{\"classifiers\":{"
            "\"natives-windows\":{\"path\":\"org/lwjgl/lwjgl/2.9.4/lwjgl-2.9.4-natives-windows.jar\",\"sha1\":\"cc\",\"size\":3}}}}";
        snprintf(buf, sizeof(buf), "%s", plain);
        doc = sxcl_json_parse(buf, strlen(buf), perr, sizeof(perr));
        check(doc != NULL, "无占位符的夹具能解析");
        if (doc) {
            const sxcl_json_value *cls =
                sxcl_natives_classifier_of(sxcl_json_root(doc), "windows", key, sizeof(key));
            check(cls != NULL, "原样能找到就用原样的(绝大多数版本)");
            check(strcmp(key, "natives-windows") == 0, "  键没被改动");
            sxcl_json_free(doc);
        }
        const char *none =
            "{\"name\":\"tv.twitch:twitch-platform:6.5\","
            "\"natives\":{\"windows\":\"natives-windows-${arch}\"},"
            "\"downloads\":{\"classifiers\":{"
            "\"natives-linux\":{\"path\":\"x.jar\",\"sha1\":\"dd\",\"size\":4}}}}";
        snprintf(buf, sizeof(buf), "%s", none);
        doc = sxcl_json_parse(buf, strlen(buf), perr, sizeof(perr));
        check(doc != NULL, "都对不上的夹具能解析");
        if (doc) {
            const sxcl_json_value *cls =
                sxcl_natives_classifier_of(sxcl_json_root(doc), "windows", key, sizeof(key));
            check(cls == NULL, "真的没有就返回 NULL(不硬编一个路径出来)");
            check(strcmp(key, "natives-windows-${arch}") == 0, "  回填的是**原始键**(报错里要看得见原样)");
            sxcl_json_free(doc);
        }
    }

    printf("natives 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
