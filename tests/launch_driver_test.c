/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/launch.h"
#include "sxcl/options.h"
#include "sxcl/settings.h"

#if defined(_WIN32)
#  include <direct.h>
#  define FAKE_EXT ".bat"
#  define GETCWD _getcwd
#  define PATH_SEP '\\'
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define FAKE_EXT ".sh"
#  define GETCWD getcwd
#  define PATH_SEP '/'
#endif

#define TMP_DIR     "build-b/_launch_driver_tmp"
#define GAME_DIR    TMP_DIR "/game"
#define VERSIONS    GAME_DIR "/versions"
#define VERSION_DIR VERSIONS "/1.21.4"
#define VJSON       VERSION_DIR "/1.21.4.json"
#define OPTIONS     GAME_DIR "/options.txt"
#define SETTINGS    TMP_DIR "/settings.conf"
#define FAKE_OK     TMP_DIR "/fake_ok"     FAKE_EXT
#define FAKE_JAVA   TMP_DIR "/fake_java"   FAKE_EXT
#define FAKE_VK     TMP_DIR "/fake_vulkan" FAKE_EXT
#define FAKE_SLEEP  TMP_DIR "/fake_sleep"  FAKE_EXT

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

static void check_int(long got, long want, const char *what) {
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %ld want %ld\n", what, got, want);
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (got && want && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)");
    }
}

/* ── 落盘小工具 ── */

/** 相对路径 -> 绝对路径。
 *  为什么假 java 必须用绝对路径:CreateProcess 自己在**父进程**的当前目录下找到了 .bat,
 *  但它把路径原样交给 cmd.exe,而 cmd 是按**子进程**的工作目录(游戏目录)去找的 ——
 *  相对路径的 .bat 于是必然"系统找不到指定的路径"(实测)。真启动器给的 java 路径本来就是绝对的,
 *  这里照做即可。 */
static void abs_path(char *out, size_t cap, const char *rel) {
    char cwd[1024];
    if (GETCWD(cwd, sizeof(cwd)) != NULL) {
        (void)snprintf(out, cap, "%s%c%s", cwd, PATH_SEP, rel);
    } else {
        (void)snprintf(out, cap, "%s", rel);
    }
}

/* 绝对路径版的可执行文件(program 用它们) */
static char g_fake_ok[1200];
static char g_fake_java[1200];
static char g_fake_vk[1200];
static char g_fake_sleep[1200];

static int write_text(const char *path, const char *text) {
    /* 位置写(offset 0)**不截断**:内容一次比一次短时,旧文件的尾巴会留在后面,
     * 夹具就成了一份"合法 JSON + 多余内容"(实测:解析报"根值之后还有多余内容")。
     * 先删掉再写,夹具就永远是这次的这一份。 */
    (void)sxcl_fs_remove(path);
    sxcl_file *f = sxcl_file_open_write(path, -1);
    int64_t n = 0;
    if (!f) {
        return -1;
    }
    n = sxcl_file_write_at(f, text, strlen(text), 0);
    (void)sxcl_file_close(f);
    return n == (int64_t)strlen(text) ? 0 : -1;
}

static void catf(char *buf, size_t cap, size_t *len, const char *text) {
    const size_t n = strlen(text);
    if (*len + n + 1 < cap) {
        memcpy(buf + *len, text, n);
        *len += n;
        buf[*len] = '\0';
    }
}

/** 造一个"假 java":打印 lines 里的每一行,可选打印 options.txt,最后按 exit_code 退出。
 *  lines 里以 '@' 开头的条目按**原样命令**写进脚本(不套 echo)—— 用来塞 sleep/ping 这类等时命令。 */
static int write_fake_java(const char *path, const char *const *lines, int exit_code,
                           int dump_options) {
    char body[4096];
    char tmp[1024];
    size_t n = 0;
    size_t i = 0;
    body[0] = '\0';
#if defined(_WIN32)
    catf(body, sizeof(body), &n, "@echo off\r\n");
    for (i = 0; lines[i]; ++i) {
        if (lines[i][0] == '@') {
            (void)snprintf(tmp, sizeof(tmp), "%s\r\n", lines[i] + 1);
        } else {
            (void)snprintf(tmp, sizeof(tmp), "echo %s\r\n", lines[i]);
        }
        catf(body, sizeof(body), &n, tmp);
    }
    if (dump_options) {
        catf(body, sizeof(body), &n, "type options.txt\r\n");
    }
    (void)snprintf(tmp, sizeof(tmp), "exit /b %d\r\n", exit_code);
    catf(body, sizeof(body), &n, tmp);
#else
    catf(body, sizeof(body), &n, "#!/bin/sh\n");
    for (i = 0; lines[i]; ++i) {
        if (lines[i][0] == '@') {
            (void)snprintf(tmp, sizeof(tmp), "%s\n", lines[i] + 1);
        } else {
            (void)snprintf(tmp, sizeof(tmp), "echo '%s'\n", lines[i]);
        }
        catf(body, sizeof(body), &n, tmp);
    }
    if (dump_options) {
        catf(body, sizeof(body), &n, "cat options.txt 2>/dev/null\n");
    }
    (void)snprintf(tmp, sizeof(tmp), "exit %d\n", exit_code);
    catf(body, sizeof(body), &n, tmp);
#endif
    if (write_text(path, body) != 0) {
        return -1;
    }
#if !defined(_WIN32)
    (void)chmod(path, 0755);
#endif
    return 0;
}

/* ── 抓到子进程输出的每一行 ── */

typedef struct captured {
    char buf[16384];
    size_t len;
} captured;

static int capture_line(void *userdata, int is_stderr, const char *line) {
    captured *cap = (captured *)userdata;
    (void)is_stderr;
    if (cap->len + strlen(line) + 2 < sizeof(cap->buf)) {
        const size_t n = strlen(line);
        memcpy(cap->buf + cap->len, line, n);
        cap->len += n;
        cap->buf[cap->len++] = '\n';
        cap->buf[cap->len] = '\0';
    }
    return 0; /* 0 = 不终止;返回非 0 才是"请求终止" */
}

/* ── 仿真日志剧本 ── */

static const char *kLinesOk[] = {
    "[12:34:56] [main/INFO]: Loading Minecraft 1.21.4",
    "[12:34:58] [Render thread/INFO]: Backend library: LWJGL version 3.3.3+5",
    "[12:34:59] [Render thread/INFO]: OpenGL Renderer: Fake GPU 9000",
    "[12:34:59] [Render thread/INFO]: OpenGL Version: 4.6.0 FakeDriver 1.0",
    "[12:35:10] [main/INFO]: Stopping!",
    NULL
};

static const char *kLinesJava[] = {
    "[12:35:00] [main/ERROR]: java.lang.UnsupportedClassVersionError: "
    "net/fabricmc/loader/impl/launch/knot/Knot has been compiled by a more recent version of the "
    "Java Runtime (class file version 65.0), this version of the Java Runtime only recognizes class "
    "file versions up to 52.0",
    "[12:35:00] [main/ERROR]: java.lang.NoClassDefFoundError: net/minecraft/client/main/Main",
    NULL
};

static const char *kLinesVulkan[] = {
    "[12:34:58] [Render thread/INFO]: Backend library: LWJGL version 3.3.3+5",
    "[12:34:59] [Render thread/WARN]: Vulkan device not found (VK_ERROR_INCOMPATIBLE_DRIVER), "
    "falling back to OpenGL",
    "[12:35:00] [Render thread/INFO]: OpenGL Renderer: Fake GPU 9000",
    NULL
};

static const char *kLinesSleep[] = {
    "[12:40:00] [Render thread/INFO]: Backend library: LWJGL version 3.3.3+5",
    NULL
};

/* 版本 JSON:新格式,要求 Java 21,带最小可用的 arguments 与一条库 */
static const char *kVersionJson =
    "{"
    "\"id\":\"1.21.4\","
    "\"type\":\"release\","
    "\"mainClass\":\"net.minecraft.client.main.Main\","
    "\"javaVersion\":{\"component\":\"java-runtime-delta\",\"majorVersion\":21},"
    "\"assetIndex\":{\"id\":\"17\","
    "\"size\":200,\"url\":\"https://piston-meta.mojang.com/v1/packages/ff/17.json\"},"
    "\"arguments\":{"
    "  \"jvm\":[\"-Djava.library.path=${natives_directory}\",\"-cp\",\"${classpath}\"],"
    "  \"game\":[\"--username\",\"${auth_player_name}\",\"--version\",\"${version_name}\","
    "            \"--gameDir\",\"${game_directory}\"]"
    "},"
    /* downloads.client:启动前"补全文件"要拿它去补 <实例>.jar(少了它清单里就没有 jar 那一条) */
    "\"downloads\":{\"client\":{"
    "\"size\":64,\"url\":\"https://piston-data.mojang.com/v1/objects/aa/client.jar\"}},"
    "\"libraries\":[{\"name\":\"com.example:demo:1.0\","
    "\"downloads\":{\"artifact\":{\"path\":\"com/example/demo/1.0/demo-1.0.jar\","
    "\"size\":64,"
    "\"url\":\"https://libraries.minecraft.net/com/example/demo/1.0/demo-1.0.jar\"}}}]"
    "}";

static int setup(void) {
    if (sxcl_fs_mkdirs(VERSION_DIR) != 0) {
        return -1;
    }
    if (write_text(VJSON, kVersionJson) != 0) {
        return -1;
    }
    /* options.txt 里先放一个"用户/游戏自己写的"旧值,好看清是不是被改写 */
    if (write_text(OPTIONS, "fov:0.5\r\ngraphicsApi:default\r\n") != 0) {
        return -1;
    }
    /* 每实例设置:case-a 选的是 opengl(驱动要从设置里读出来) */
    if (write_text(SETTINGS, "instance.case-a.graphicsApi=opengl\r\n") != 0) {
        return -1;
    }
    if (write_fake_java(FAKE_OK, kLinesOk, 0, 1) != 0) {
        return -1;
    }
    if (write_fake_java(FAKE_JAVA, kLinesJava, 1, 1) != 0) {
        return -1;
    }
    if (write_fake_java(FAKE_VK, kLinesVulkan, 0, 1) != 0) {
        return -1;
    }
#if defined(_WIN32)
    {
        const char *sleep_lines[] = { kLinesSleep[0], "@ping -n 6 127.0.0.1 >nul", NULL };
        if (write_fake_java(FAKE_SLEEP, sleep_lines, 0, 0) != 0) {
            return -1;
        }
    }
#else
    {
        const char *sleep_lines[] = { kLinesSleep[0], "@sleep 5", NULL };
        if (write_fake_java(FAKE_SLEEP, sleep_lines, 0, 0) != 0) {
            return -1;
        }
    }
#endif
    return 0;
}

/* ── 用例 ── */

/** 把 options.txt / 设置文件当前的样子打出来 —— 报告里的"写入前后对照"就靠它。 */
static void print_state(const char *tag, const char *instance) {
    sxcl_options *op = sxcl_options_load(OPTIONS);
    sxcl_settings *st = sxcl_settings_open(SETTINGS);
    const char *api = op ? sxcl_options_get(op, "graphicsApi") : NULL;
    const char *fov = op ? sxcl_options_get(op, "fov") : NULL;
    const char *last = st ? sxcl_settings_instance_get(st, instance, "lastGraphicsApi", "(无)") : "(没设置文件)";
    printf("      [%s] options.txt: fov=%s graphicsApi=%s | instance.%s.lastGraphicsApi=%s\n", tag,
           fov ? fov : "(无)", api ? api : "(无)", instance, last ? last : "(无)");
    sxcl_options_free(op);
    sxcl_settings_free(st);
}

static void case_a_normal(void) {
    captured cap;
    sxcl_launch_request req;
    sxcl_launch_result res;
    char err[256];
    sxcl_options *op = NULL;
    sxcl_settings *st = NULL;
    int rc = 0;

    printf("[a] 正常启动(假 java:GL 行 + Stopping! + 退出码 0)\n");
    memset(&cap, 0, sizeof(cap));
    memset(&req, 0, sizeof(req));
    req.game_dir = GAME_DIR;
    req.version_name = "1.21.4";
    req.java_path = g_fake_ok;
    req.instance = "case-a";
    req.offline_name = "Steve";
    req.settings_path = SETTINGS;
    req.on_line = capture_line;
    req.userdata = &cap;

    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    rc = sxcl_launch_run(&req, &res, err, sizeof(err));
    check_int(rc, 0, "驱动返回成功");
    check_int(res.started, 1, "确实起了进程");
    check_int(res.exit_code, 0, "退出码 0");
    check_int(res.timed_out, 0, "没有超时");
    check_int(res.conclusion, SXCL_LOG_CONCLUSION_OK, "结论是正常退出");
    check(strstr(res.conclusion_text, "正常退出") != NULL, "人话结论里说了正常退出");
    check_str(res.requested_backend, "opengl", "后端从实例设置里读出来(case-a=opengl)");
    check_str(res.actual_backend, "opengl", "实际后端 = 请求的后端(没看到回退)");
    check_int(res.vulkan_fell_back, 0, "没有回退");
    check(strstr(res.java_path, "fake_ok") != NULL, "结果里带上了选中的 java 路径");
    check_int(res.java_major, 0, "假 java 没有 release,版本按未知处理(不是错误)");
    check_int((long)res.log.lines > 4, 1, "日志行被喂进了 logscan");
    check(strstr(cap.buf, "OpenGL Version: 4.6.0 FakeDriver 1.0") != NULL,
          "调用方回调收到了原始日志行");
    check(strstr(cap.buf, "graphicsApi:opengl") != NULL,
          "子进程启动时读到的 options.txt 已经是 opengl(说明是启动前写的)");

    op = sxcl_options_load(OPTIONS);
    check(op != NULL, "回读 options.txt");
    check_str(sxcl_options_get(op, "graphicsApi"), "opengl", "options.txt 里的 graphicsApi");
    check_str(sxcl_options_get(op, "fov"), "0.5", "文件里原有的键没被丢掉");
    sxcl_options_free(op);

    st = sxcl_settings_open(SETTINGS);
    check(st != NULL, "回读设置文件");
    check_str(sxcl_settings_instance_get(st, "case-a", "lastGraphicsApi", "(无)"), "opengl",
              "settings 落盘:lastGraphicsApi=opengl");
    sxcl_settings_free(st);
    printf("      结论文本: %s\n", res.conclusion_text);
    print_state("a 之后", "case-a");
}

static void case_b_java(void) {
    captured cap;
    sxcl_launch_request req;
    sxcl_launch_result res;
    char err[256];
    int rc = 0;

    printf("[b] Java 版本不符(次生 NoClassDefFoundError 不许把结论带偏)\n");
    memset(&cap, 0, sizeof(cap));
    memset(&req, 0, sizeof(req));
    req.game_dir = GAME_DIR;
    req.version_name = "1.21.4";
    req.java_path = g_fake_java;
    req.instance = "case-b";
    req.backend = "default";
    req.settings_path = SETTINGS;
    req.on_line = capture_line;
    req.userdata = &cap;

    memset(&res, 0, sizeof(res));
    rc = sxcl_launch_run(&req, &res, err, sizeof(err));
    check_int(rc, 0, "进程起来了(驱动本身成功)");
    check_int(res.exit_code, 1, "游戏以 1 退出");
    check_int(res.conclusion, SXCL_LOG_CONCLUSION_JAVA,
              "结论指向 Java 版本,而不是次生的缺类错误");
    check_int(res.log.voted_missing, 1, "次生的 NoClassDefFoundError 仍然记录在案");
    check(strstr(res.conclusion_text, "Java") != NULL, "人话结论里点名 Java");
    check_str(res.actual_backend, "default", "这局没提 Vulkan 回退");
    printf("      结论文本: %s\n", res.conclusion_text);
    print_state("b 之后", "case-b");
}

static void case_c_vulkan_fallback(void) {
    captured cap;
    sxcl_launch_request req;
    sxcl_launch_result res;
    char err[256];
    sxcl_options *op = NULL;
    sxcl_settings *st = NULL;
    int rc = 0;

    printf("[c] 切了 Vulkan 但设备回退到 OpenGL\n");
    memset(&cap, 0, sizeof(cap));
    memset(&req, 0, sizeof(req));
    req.game_dir = GAME_DIR;
    req.version_name = "1.21.4";
    req.java_path = g_fake_vk;
    req.instance = "case-c";
    req.backend = SXCL_LAUNCH_BACKEND_VULKAN;
    req.settings_path = SETTINGS;
    req.on_line = capture_line;
    req.userdata = &cap;

    memset(&res, 0, sizeof(res));
    rc = sxcl_launch_run(&req, &res, err, sizeof(err));
    check_int(rc, 0, "进程起来了");
    check_int(res.exit_code, 0, "游戏正常退出");
    check_int(res.vulkan_fell_back, 1, "识别出 Vulkan 回退");
    check_str(res.requested_backend, "vulkan", "用户要求的是 vulkan");
    check_str(res.actual_backend, "opengl", "实际生效的是 opengl");
    check_int(res.conclusion, SXCL_LOG_CONCLUSION_VULKAN_FALLBACK, "结论文本是 vulkan_fallback");
    check(strstr(res.conclusion_text, "Vulkan") != NULL &&
              strstr(res.conclusion_text, "OpenGL") != NULL,
          "人话结论同时点名 Vulkan 与 OpenGL");
    check(strstr(cap.buf, "graphicsApi:vulkan") != NULL,
          "子进程启动时 options.txt 里已经是用户选的 vulkan");

    op = sxcl_options_load(OPTIONS);
    check_str(sxcl_options_get(op, "graphicsApi"), "vulkan",
              "options.txt 写的是用户选的后端(启动前写的)");
    sxcl_options_free(op);

    st = sxcl_settings_open(SETTINGS);
    check_str(sxcl_settings_instance_get(st, "case-c", "lastGraphicsApi", "(无)"), "opengl",
              "settings 落盘:lastGraphicsApi 被改写成实际生效的 opengl");
    sxcl_settings_free(st);
    printf("      结论文本: %s\n", res.conclusion_text);
    print_state("c 之后", "case-c");
}

static void case_d_dry_run(void) {
    sxcl_launch_request req;
    sxcl_launch_result res;
    char err[256];
    sxcl_options *op = NULL;
    int rc = 0;

    printf("[d] 只准备不启动(dry_run):options.txt 仍然按实例设置写好\n");
    memset(&req, 0, sizeof(req));
    req.game_dir = GAME_DIR;
    req.version_name = "1.21.4";
    req.java_path = g_fake_ok;
    req.instance = "case-d";
    req.backend = SXCL_LAUNCH_BACKEND_OPENGL; /* 上一局留下的是 vulkan,这一局要看到它被改回 opengl */
    req.settings_path = SETTINGS;
    req.dry_run = 1;

    memset(&res, 0, sizeof(res));
    rc = sxcl_launch_run(&req, &res, err, sizeof(err));
    check_int(rc, 0, "dry_run 返回成功");
    check_int(res.started, 0, "dry_run 不起进程");
    check_int(res.exit_code, -1, "dry_run 没有退出码");
    op = sxcl_options_load(OPTIONS);
    check(op != NULL, "dry_run 后回读 options.txt");
    check_str(sxcl_options_get(op, "graphicsApi"), "opengl",
              "options.txt 已经改成这一局选的后端(证明写盘发生在起进程之前)");
    sxcl_options_free(op);
    printf("      结论文本: %s\n", res.conclusion_text);
    print_state("d 之后", "case-d");
}

static void case_e_timeout(void) {
    sxcl_launch_request req;
    sxcl_launch_result res;
    char err[256];
    int rc = 0;

    printf("[e] 超时透传(假 java 会睡 5 秒,驱动 1 秒就终止它)\n");
    memset(&req, 0, sizeof(req));
    req.game_dir = GAME_DIR;
    req.version_name = "1.21.4";
    req.java_path = g_fake_sleep;
    req.instance = "case-e";
    req.backend = "default";
    req.settings_path = SETTINGS;
    req.timeout_ms = 1000;

    memset(&res, 0, sizeof(res));
    rc = sxcl_launch_run(&req, &res, err, sizeof(err));
    check_int(rc, 0, "驱动本身没失败");
    check_int(res.started, 1, "进程起来了");
    check_int(res.timed_out, 1, "超时被识别");
    check(res.elapsed_ms >= 900 && res.elapsed_ms < 4500, "用时约等于超时时间(没有等满 5 秒)");
    check(strstr(res.conclusion_text, "超时") != NULL, "人话结论里说了超时");
    printf("      结论文本: %s\n", res.conclusion_text);
}

static void case_f_errors(void) {
    sxcl_launch_request req;
    sxcl_launch_result res;
    char err[256];
    int rc = 0;

    printf("[f] 错误路径:版本没装好 / 参数不全\n");
    memset(&req, 0, sizeof(req));
    req.game_dir = GAME_DIR;
    req.version_name = "9.9.9-not-installed";
    req.java_path = g_fake_ok;
    req.settings_path = SETTINGS;
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    rc = sxcl_launch_run(&req, &res, err, sizeof(err));
    check(rc < 0, "版本 JSON 不存在时驱动报错");
    check(strstr(res.error, "版本 JSON") != NULL, "错误里点名版本 JSON");
    check(res.error[0] != '\0' && err[0] != '\0', "错误同时写进 result 与 err");
    check_int(res.started, 0, "没有起进程");
    printf("      错误文本: %s\n", res.error);

    memset(&req, 0, sizeof(req));
    req.game_dir = GAME_DIR;
    rc = sxcl_launch_run(&req, &res, err, sizeof(err));
    check(rc < 0, "缺版本名时报错");
    check_str(res.error, "缺少版本名", "缺版本名的错误文本");
    check_int(sxcl_launch_run(NULL, &res, err, sizeof(err)), -1, "request=NULL 不炸");
}


/* ── 启动前补全文件(PCL 启动链第 3 步):清单 + "齐了一个字节都不下" + 补不齐不拦启动 ──
 *
 * 这里连**假传输后端**一起造 —— 补全最要紧的性质就是那两个计数:
 *   文件都在 → **一次 HTTP 都不该发**;缺文件 → 恰好把缺的下回来。
 * (校验本身是引擎的活,verify_test / hashcache_test 已经盯得很细。)
 * 夹具里那三个文件都是 64 字节、不带 sha1 → 校验等级是"只比大小",假后端回 64 字节就过。 */

typedef struct fake_http {
    int requests;      /* 收到几次请求(0 = 一个字节都没下) */
    int fail_all;      /* 1 = 一律 404(验"补不上也要照常启动") */
    /* 声明的长度照旧,但**只给这么多字节**(模拟服务端提前掐断:Content-Length 说 64,实际 16)。
     * 回归用例:引擎会把 .part 预分配到最终大小,只按大小校验就会把半截文件当成品。 */
    int short_by;
    char last_url[512];
    unsigned char payload[64];
    /* P0b:资源索引必须是**真 JSON** 才展得开 5000+ 个对象 —— URL 里带 "17.json" 时回这一份。 */
    const char *index_json;
} fake_http;

typedef struct fake_body {
    const unsigned char *data;
    size_t len;
    size_t pos;
} fake_body;

static int fake_request(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp,
                        sxcl_http_body **body) {
    fake_http *s = (fake_http *)ctx;
    fake_body *b = (fake_body *)calloc(1, sizeof(fake_body));
    if (!b) {
        return SXCL_NET_ERR_IO;
    }
    ++s->requests;
    if (req && req->url) {
        snprintf(s->last_url, sizeof(s->last_url), "%s", req->url);
    }
    memset(resp, 0, sizeof(*resp));
    if (!s->fail_all && s->index_json != NULL && req != NULL && req->url != NULL &&
        strstr(req->url, "17.json") != NULL) {
        /* 资源索引:长度要**正好等于**夹具里 assetIndex.size(尾部用空格补齐,JSON 允许尾随空白) */
        b->data = (const unsigned char *)s->index_json;
        b->len = strlen(s->index_json);
        resp->status = 200;
        resp->content_length = (int64_t)b->len;
        resp->total_length = (int64_t)b->len;
        resp->accept_ranges = 1;
        *body = (sxcl_http_body *)b;
        return SXCL_NET_OK;
    }
    if (s->fail_all) {
        resp->status = 404;
        b->data = (const unsigned char *)"";
        b->len = 0;
    } else {
        resp->status = 200;
        resp->content_length = (int64_t)sizeof(s->payload);
        resp->total_length = (int64_t)sizeof(s->payload);
        resp->accept_ranges = 1;
        b->data = s->payload;
        b->len = sizeof(s->payload);
        if (s->short_by > 0 && (size_t)s->short_by < b->len) {
            b->len -= (size_t)s->short_by; /* 提前 EOF:声明与实到不一致 */
        }
    }
    *body = (sxcl_http_body *)b;
    return SXCL_NET_OK;
}

static int64_t fake_read(void *ctx, sxcl_http_body *body, void *buf, size_t len) {
    (void)ctx;
    fake_body *b = (fake_body *)body;
    if (!b || b->pos >= b->len) {
        return 0;
    }
    size_t n = b->len - b->pos;
    if (n > len) {
        n = len;
    }
    memcpy(buf, b->data + b->pos, n);
    b->pos += n;
    return (int64_t)n;
}

static void fake_close_body(void *ctx, sxcl_http_body *body) {
    (void)ctx;
    free(body);
}

static void fake_cancel_all(void *ctx) { (void)ctx; }
static void fake_destroy(void *ctx) { (void)ctx; }

static sxcl_transport *fake_factory(void *ud) {
    sxcl_transport *t = (sxcl_transport *)calloc(1, sizeof(sxcl_transport));
    if (!t) {
        return NULL;
    }
    t->ctx = ud;
    t->request = fake_request;
    t->read = fake_read;
    t->close_body = fake_close_body;
    t->cancel_all = fake_cancel_all;
    t->destroy = fake_destroy;
    return t;
}

/* 三个文件的落盘位置:与版本 JSON 里的 downloads.client / 那条库 / assetIndex 一一对应。 */
#define CJAR   VERSION_DIR "/1.21.4.jar"
#define CLIB   GAME_DIR "/libraries/com/example/demo/1.0/demo-1.0.jar"
#define CINDEX GAME_DIR "/assets/indexes/17.json"

static int write_sized(const char *path, size_t size) {
    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        return -1;
    }
    memset(buf, 'x', size);
    buf[size] = '\0';
    (void)sxcl_fs_mkdirs_for_file(path); /* 库/资源那两条的父目录还不存在 */
    const int rc = write_text(path, buf);
    free(buf);
    return rc;
}

static void remove_if_there(const char *path) {
    if (sxcl_fs_exists(path)) {
        (void)sxcl_fs_remove(path);
    }
}

static int g_cancel_hits = 0;
static int cancel_right_away(void *ud) {
    (void)ud;
    ++g_cancel_hits;
    return 1; /* 立刻喊停 */
}

/* ── P0b:资源索引与它展开出来的两个对象 ──
 * 索引长度必须正好 200(夹具里 assetIndex.size = 200),尾部补空格(JSON 允许尾随空白)。 */
#define COBJ1_HASH "aabbccddeeff00112233445566778899aabbccdd"
#define COBJ2_HASH "00112233445566778899aabbccddeeff00112233"
#define COBJ1 GAME_DIR "/assets/objects/aa/" COBJ1_HASH
#define COBJ2 GAME_DIR "/assets/objects/00/" COBJ2_HASH
static const char *kAssetIndexBody =
    "{\"objects\":{\"probe/one.bin\":{\"hash\":\"" COBJ1_HASH "\",\"size\":64},"
    "\"probe/two.bin\":{\"hash\":\"" COBJ2_HASH "\",\"size\":64}}}";

static const char *assetIndexBody(void) {
    static char padded[201];
    if (padded[0] == '\0') {
        memset(padded, ' ', sizeof(padded) - 1);
        memcpy(padded, kAssetIndexBody, strlen(kAssetIndexBody));
        padded[200] = '\0';
    }
    return padded;
}

static void case_g_complete(void) {
    fake_http server;
    sxcl_engine_opts opts;
    sxcl_launch_request req;
    sxcl_launch_result res;
    char err[256];

    printf("[g] 启动前补全文件(清单 / dry-run 也补 / 齐了一字节不下 / 补不齐不拦启动)\n");
    memset(&server, 0, sizeof(server));
    memset(&opts, 0, sizeof(opts));
    opts.workers = 2;
    opts.transport_factory = fake_factory;
    opts.userdata = &server;
    memset(server.payload, 'y', sizeof(server.payload));
    server.index_json = assetIndexBody();   // P0b:资源索引那一路(必须是真 JSON)

    memset(&req, 0, sizeof(req));
    req.game_dir = GAME_DIR;
    req.version_name = "1.21.4";
    req.java_path = g_fake_ok;
    req.instance = "case-g";
    req.settings_path = SETTINGS;
    req.dry_run = 1;
    req.complete_files = 1;
    req.engine_opts = &opts;
    req.complete_assets = 1;   /* P0b:资源对象按"只比大小"补(PCL 启动前的口径) */

    /* g1) 五个文件都在(大小对得上)→ 一次 HTTP 都不该发。
     * 注意索引是 200 字节(与夹具的 assetIndex.size 一致),对象两个各 64 字节。 */
    check_int(write_sized(CJAR, 64), 0, "摆好客户端 jar(64 字节)");
    check_int(write_sized(CLIB, 64), 0, "摆好依赖库(64 字节)");
    (void)sxcl_fs_mkdirs_for_file(CINDEX);
    check(write_text(CINDEX, assetIndexBody()) == 0,
          "摆好资源索引(200 字节的**真 JSON** —— 展开对象要靠它的内容)");
    check_int(write_sized(COBJ1, 64), 0, "摆好资源对象 1(64 字节)");
    check_int(write_sized(COBJ2, 64), 0, "摆好资源对象 2(64 字节)");
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    check_int(sxcl_launch_run(&req, &res, err, sizeof(err)), 0, "dry-run 成功");
    check_int(res.complete_ran, 1, "补全这一步跑了");
    check_int(server.requests, 0, "**文件都在:一次 HTTP 都没发**");
    check_int(res.complete.files_total, 5, "清单五件:jar + 依赖库 + 索引 + 两个资源对象(P0b)");
    check_int(res.complete.files_skipped, 5, "五件全部命中已有(资源对象也没重下)");
    check_int(res.complete.files_downloaded, 0, "没有下载");
    check_int((long)res.complete.bytes_done, 0, "没有写字节");

    /* g2) 删掉库 + 索引 + 对象1 → 恰好把缺的下回来,另两件不重下。
     * 这一局同时验证**两遍的顺序**:索引必须在第一遍下回来,第二遍才展得开对象。 */
    remove_if_there(CLIB);
    remove_if_there(CINDEX);
    remove_if_there(COBJ1);
    server.requests = 0;
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    check_int(sxcl_launch_run(&req, &res, err, sizeof(err)), 0, "缺文件时启动照常");
    check_int(server.requests > 0, 1, "真的去下了");
    check_int(res.complete.files_downloaded, 3, "补回三件(依赖库 + 索引 + 对象1)");
    /* 第二遍跑的是**整张计划**(5 件),命中的都算 skipped —— 所以这里比"没下过的件数"多 */
    check_int(res.complete.files_skipped, 4, "第二遍里命中的 4 件都走了快路径");
    check_int((long)res.complete.bytes_done, 200 + 64 + 64, "字节 = 索引 200 + 库 64 + 对象 64");
    check(sxcl_fs_exists(CLIB), "依赖库补回来了");
    check(sxcl_fs_exists(CINDEX), "资源索引补回来了");
    check(sxcl_fs_exists(COBJ1), "资源对象补回来了(第二遍展开生效)");

    /* g3) 后端一律 404 → 补不上,但**不拦启动**(与 PCL 一致) */
    remove_if_there(CLIB);
    server.fail_all = 1;
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    check_int(sxcl_launch_run(&req, &res, err, sizeof(err)), 0, "补不齐也不拦启动");
    check_int(res.complete_ran, 1, "这一步跑了");
    check_int(res.complete.files_failed, 1, "如实记下失败件数(只有那一件库;资源对象都在)");
    check(res.complete.error[0] != '\0', "错误原因留下来了(不是空的)");
    server.fail_all = 0;

    /* g4) 没给 engine_opts → 保持旧行为:不补全 */
    memset(&res, 0, sizeof(res));
    req.engine_opts = NULL;
    err[0] = '\0';
    check_int(sxcl_launch_run(&req, &res, err, sizeof(err)), 0, "没有引擎配置也能启动");
    check_int(res.complete_ran, 0, "没给引擎配置就不补全(旧行为)");
    req.engine_opts = &opts;

    /* g5) 开关关掉 → 不补全 */
    memset(&res, 0, sizeof(res));
    req.complete_files = 0;
    err[0] = '\0';
    check_int(sxcl_launch_run(&req, &res, err, sizeof(err)), 0, "关掉补全也能启动");
    check_int(res.complete_ran, 0, "关掉就不补全");
    req.complete_files = 1;

    /* g6) 取消:回调立刻喊停 → 如实记"已取消",启动继续 */
    remove_if_there(CLIB);
    g_cancel_hits = 0;
    req.complete_is_cancelled = cancel_right_away;
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    check_int(sxcl_launch_run(&req, &res, err, sizeof(err)), 0, "取消补全也不拦启动");
    check_int(res.complete_ran, 1, "这一步跑了");
    check(g_cancel_hits > 0, "取消回调被问过");
    check(strstr(res.complete.error, "取消") != NULL, "结果里说了是取消");
    req.complete_is_cancelled = NULL;

    /* g7) 档位 0 = 不补资源:对象不补回来,清单也只有三件 */
    remove_if_there(COBJ1);
    req.complete_assets = 0;
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    check_int(sxcl_launch_run(&req, &res, err, sizeof(err)), 0, "关掉资源那一档也能启动");
    check_int(res.complete.files_total, 3, "档位 0:清单只有三件(不含资源对象)");
    check(!sxcl_fs_exists(COBJ1), "档位 0:对象不会被补回来");

    /* g8) 档位 2 = 强校验:假后端回的字节对不上索引里的 sha1 → 如实记失败
     * (这一条同时证明"这个档位真的在算哈希",而不是摆设) */
    req.complete_assets = 2;
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    check_int(sxcl_launch_run(&req, &res, err, sizeof(err)), 0, "强校验档也照样启动");
    check(res.complete.files_failed >= 1, "强校验:内容与 sha1 不符 → 记失败");
    req.complete_assets = 1;

    /* g9) **半截传输**:服务端声明 64 字节却只给 16 字节 —— 必须记失败,绝不能把半截文件
     * 落到正式路径上。引擎会把 .part **预分配到最终大小**,所以"文件大小 == 期望大小"永远成立,
     * 只按大小校验就会放过它 —— 真机上(2026-09-22 晚)8.3MB 的 Quilt 安装器只到了 2MB、
     * 后半段全是 0,却报了"成功",一直到 java 打不开那个 jar 才暴露。 */
    remove_if_there(CLIB);
    memset(&server, 0, sizeof(server));
    memset(server.payload, 'y', sizeof(server.payload));
    server.index_json = assetIndexBody();
    server.short_by = 48; /* 64 -> 只给 16 */
    memset(&res, 0, sizeof(res));
    err[0] = '\0';
    check_int(sxcl_launch_run(&req, &res, err, sizeof(err)), 0, "半截传输也不拦启动");
    check(res.complete.files_failed >= 1, "  半截传输要如实记失败（不能当成功）");
    check(!sxcl_fs_exists(CLIB), "  半截内容绝不能落到正式路径上");

    /* 复原:把对象补上,后面的用例看到的是"全都在" */
    (void)write_sized(COBJ1, 64);
    (void)write_sized(CLIB, 64);
}

/* ── [h] 版本隔离（docs/22 的 A2 / docs/24 的 P3 前置） ──
 *
 * 口径：开了以后 --gameDir 指 <游戏目录>/versions/<版本名>（这一份自己的 mods/saves/config/
 * options.txt），而 assets / libraries **仍从根目录取**；关着（默认）时一切照旧。
 * 断言直接读 dry-run 报出来的命令行（driver.c 在 dry-run 时会逐行报 argv），
 * 加上结果里的 isolated / isolated_dir / options_path 与磁盘上真建出来的子目录。 */

static char g_iso_lines[16384];
static size_t g_iso_len = 0;

static int iso_capture(void *ud, int is_stderr, const char *line) {
    (void)ud;
    (void)is_stderr;
    if (line != NULL) {
        catf(g_iso_lines, sizeof(g_iso_lines), &g_iso_len, line);
        catf(g_iso_lines, sizeof(g_iso_lines), &g_iso_len, "\n");
    }
    return 0;
}

/** 在捕获到的命令行里找某个开关**后面那一个参数**。
 *  driver.c 每行报成 "  arg[12] --gameDir" / "  arg[13] <值>"（同一个开关与它的值分两行）,
 *  所以做法是：找含 flag 的那一行 -> 取下一行 -> 剥掉 "  arg[N] " 前缀。 */
static int arg_after(const char *flag, char *out, size_t cap) {
    const char *p = g_iso_lines;
    while ((p = strstr(p, flag)) != NULL) {
        const char *eol = strchr(p, '\n');
        if (eol == NULL) {
            break;
        }
        const char *val = eol + 1;
        const char *vend = strchr(val, '\n');
        const size_t vlen = vend ? (size_t)(vend - val) : strlen(val);
        const char *rest = val;
        size_t rest_len = vlen;
        for (size_t i = 0; i < vlen; ++i) {
            if (val[i] == ']') {
                rest = val + i + 1;
                while (rest < val + vlen && *rest == ' ') {
                    ++rest;
                }
                rest_len = (size_t)(val + vlen - rest);
                break;
            }
        }
        if (rest_len + 1 <= cap) {
            memcpy(out, rest, rest_len);
            out[rest_len] = '\0';
            return 1;
        }
        p = eol + 1;
    }
    return 0;
}

/** 路径比较:Windows 上 join_path 用 '\\'、隔离模块用 '/'，同一份路径两种写法都算对。
 *  (踩过:snprintf 拿同一个缓冲当输入输出是未定义行为,拼出来的路径会是乱的 —— 这里分开用。) */
static void norm_path(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = in != NULL ? in : ""; *p != '\0' && n + 1 < cap; ++p) {
        out[n++] = (*p == '\\') ? '/' : *p;
    }
    out[n] = '\0';
}

static void check_path(const char *got, const char *want, const char *what) {
    char a[1200];
    char b[1200];
    norm_path(got, a, sizeof(a));
    norm_path(want, b, sizeof(b));
    check_str(a, b, what);
}

/** 隔离目录（一律用 '/' 拼,比较走 check_path） */
static void iso_base(char *out, size_t cap) {
    (void)snprintf(out, cap, "%s/versions/1.21.4", GAME_DIR);
}

static void case_h_isolation(void) {
    sxcl_launch_request req;
    sxcl_launch_result res;
    char err[256];
    char value[1024];
    char opts[1024];

    printf("[h] 版本隔离:--gameDir 换成 versions/<版本名>,assets/libraries 仍指根目录\n");

    {
        sxcl_settings *st = sxcl_settings_open(SETTINGS);
        if (st != NULL) {
            sxcl_settings_set(st, "general.version_isolation", "1");
            sxcl_settings_save(st, SETTINGS);
            sxcl_settings_free(st);
        }
    }

    g_iso_len = 0;
    g_iso_lines[0] = '\0';
    memset(&req, 0, sizeof(req));
    req.game_dir = GAME_DIR;
    req.version_name = "1.21.4";
    req.java_path = g_fake_ok;
    req.instance = "case-h";
    req.backend = SXCL_LAUNCH_BACKEND_OPENGL;
    req.settings_path = SETTINGS;
    req.dry_run = 1;
    req.on_line = iso_capture;
    memset(&res, 0, sizeof(res));
    check_int(sxcl_launch_run(&req, &res, err, sizeof(err)), 0, "隔离开启时 dry-run 成功");
    check_int(res.isolated, 1, "结果里标明用了隔离");
    check(res.isolated_dir[0] != '\0', "结果里给出了隔离目录");

    check(arg_after("--gameDir", value, sizeof(value)), "命令行里有 --gameDir");
    check(strstr(value, "versions") != NULL && strstr(value, "1.21.4") != NULL,
          "  --gameDir 指到 versions/<版本名>");
    check(strstr(g_iso_lines, "libraries") != NULL, "classpath 仍从根目录的 libraries 取（隔离只换数据目录）");
    check(strstr(res.game_dir, "1.21.4") != NULL, "结果里的 game_dir 报的是实际生效的隔离目录");

    iso_base(value, sizeof(value));
    (void)snprintf(opts, sizeof(opts), "%s/options.txt", value);
    check_path(res.options_path, opts, "options.txt 写进隔离目录（每个版本一套渲染设置）");
    /* 注意:sxcl_fs_exists 只认**普通文件**(S_ISREG),目录要用 sxcl_fs_is_dir —— 踩过 */
    (void)snprintf(opts, sizeof(opts), "%s/mods", value);
    check(sxcl_fs_is_dir(opts), "隔离目录里建出了 mods/");
    (void)snprintf(opts, sizeof(opts), "%s/saves", value);
    check(sxcl_fs_is_dir(opts), "隔离目录里建出了 saves/");
    (void)snprintf(opts, sizeof(opts), "%s/config", value);
    check(sxcl_fs_is_dir(opts), "隔离目录里建出了 config/");

    /* 再关掉:一切必须回到根目录行为（老用户升级上来时不能变样） */
    {
        sxcl_settings *st = sxcl_settings_open(SETTINGS);
        if (st != NULL) {
            sxcl_settings_set(st, "general.version_isolation", "0");
            sxcl_settings_save(st, SETTINGS);
            sxcl_settings_free(st);
        }
    }
    g_iso_len = 0;
    g_iso_lines[0] = '\0';
    memset(&res, 0, sizeof(res));
    check_int(sxcl_launch_run(&req, &res, err, sizeof(err)), 0, "隔离关闭时 dry-run 成功");
    check_int(res.isolated, 0, "结果里标明没隔离");
    check(strstr(res.options_path, "1.21.4") == NULL,
          "options.txt 回到根目录（关掉隔离后行为与从前完全一致）");
    check(arg_after("--gameDir", value, sizeof(value)), "命令行里有 --gameDir");
    check(strstr(value, "versions") == NULL, "  --gameDir 回到根目录");
    check_path(res.options_path, GAME_DIR "/options.txt", "根目录 options.txt 路径就是它");
    check_path(res.game_dir, GAME_DIR, "结果里的 game_dir 回到根目录");
}

int main(void) {
    if (setup() != 0) {
        printf("夹具准备失败(写不了 %s?)\n", TMP_DIR);
        return 1;
    }
    abs_path(g_fake_ok, sizeof(g_fake_ok), FAKE_OK);
    abs_path(g_fake_java, sizeof(g_fake_java), FAKE_JAVA);
    abs_path(g_fake_vk, sizeof(g_fake_vk), FAKE_VK);
    abs_path(g_fake_sleep, sizeof(g_fake_sleep), FAKE_SLEEP);
    printf("夹具目录: %s(相对当前工作目录)\n", TMP_DIR);
    printf("假 java: %s\n", g_fake_ok);
    print_state("启动前(夹具初始值)", "case-a");
    case_a_normal();
    case_b_java();
    case_c_vulkan_fallback();
    case_d_dry_run();
    case_e_timeout();
    case_f_errors();
    case_g_complete();
    case_h_isolation();
    printf("launch_driver 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
