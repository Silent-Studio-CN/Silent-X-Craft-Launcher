/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <string.h>

#include "sxcl/crash.h"
#include "sxcl/fs.h"
#include "sxcl/launch.h"

/* 原因键(第 3b 节)的夹具测试:三类断言
 *   1) **文案完备性**:每一个原因键在中英两张表里都有非空短名 + 可执行建议(漏一条就红);
 *   2) **逐行判定**:合成的日志行 -> 期望的原因键(OOM / 显卡驱动 / 冲突 mod / Mixin …);
 *   3) **同一套分析**:一份合成的 crash-report 文本喂进 sxcl_log_summarize,以及喂进
 *      sxcl_launch_scan_artifacts(读磁盘上的夹具游戏目录)时,得到的键必须一致。 */

#define FIX_DIR  "_crash_reason_fixture"
#define GAME_DIR FIX_DIR "/game"

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

static void check_str(const char *got, const char *want, const char *what)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)");
    }
}

static int contains(const char *hay, const char *needle)
{
    return hay != NULL && strstr(hay, needle) != NULL;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = sxcl_fs_fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    (void)fwrite(text, 1, strlen(text), f);
    (void)fclose(f);
}

/* ── 1) 文案完备性 ── */

static void test_table(void)
{
    size_t i = 0;
    size_t n = sxcl_log_reason_count();
    printf("[1] 原因表(%llu 条)\n", (unsigned long long)n);
    check(n >= 48u, "覆盖面 >= 48 条(对标 PCL 的 48 项)");
    for (i = 0; i < n; ++i) {
        const sxcl_log_reason r = sxcl_log_reason_at(i);
        const char *key = sxcl_log_reason_key(r);
        const char *zh = sxcl_log_reason_name(r, "zh-cn");
        const char *en = sxcl_log_reason_name(r, "en-us");
        const char *zh_a = sxcl_log_reason_advice(r, "zh-cn");
        const char *en_a = sxcl_log_reason_advice(r, "en-us");
        if (r != (sxcl_log_reason)i) {
            check(0, "枚举顺序与表顺序一致");
            return;
        }
        if (key == NULL || *key == '\0' || strlen(key) > 40u) {
            printf("  [!!] 第 %llu 条的键不合法\n", (unsigned long long)i);
            ++g_fail;
            continue;
        }
        if (zh == NULL || *zh == '\0' || en == NULL || *en == '\0' || zh_a == NULL ||
            *zh_a == '\0' || en_a == NULL || *en_a == '\0') {
            printf("  [!!] %s 的文案不全(中/英/建议都不能空)\n", key);
            ++g_fail;
            continue;
        }
        if (strcmp(zh, en) == 0) {
            printf("  [!!] %s 的英文没翻(与中文逐字相同)\n", key);
            ++g_fail;
            continue;
        }
        /* 建议要能照着做:至少 12 个字符,不能是"未知错误"这种空话 */
        if (strlen(zh_a) < 12u || strlen(en_a) < 12u) {
            printf("  [!!] %s 的建议太短,谈不上可执行\n", key);
            ++g_fail;
            continue;
        }
        ++g_pass;
    }
    /* 键必须唯一(否则 UI/统计会串) */
    for (i = 0; i < n; ++i) {
        size_t k = 0;
        for (k = i + 1u; k < n; ++k) {
            if (strcmp(sxcl_log_reason_key(sxcl_log_reason_at(i)),
                       sxcl_log_reason_key(sxcl_log_reason_at(k))) == 0) {
                check(0, "原因键唯一");
            }
        }
    }
    check_str(sxcl_log_reason_key(SXCL_REASON_OUT_OF_MEMORY), "out_of_memory", "OOM 的键");
    check_str(sxcl_log_reason_key(SXCL_REASON_JVM_32BIT), "jvm_32bit", "32 位 Java 的键");
    check_str(sxcl_log_reason_key(SXCL_REASON_GRAPHICS_DRIVER_INTEL), "graphics_driver_intel",
              "Intel 核显的键");
    check_str(sxcl_log_reason_key(SXCL_REASON_MOD_DUPLICATE), "mod_duplicate", "重复模组的键");
    check_str(sxcl_log_reason_key(SXCL_REASON_MOD_RESOLUTION_CONFLICT), "mod_resolution_conflict",
              "模组冲突的键");
    check_str(sxcl_log_reason_key(SXCL_REASON_MISSING_JAVA_LIBRARY), "missing_java_library",
              "缺 Java 库的键");
    check_str(sxcl_log_reason_key(SXCL_REASON_MIXIN_FAILURE), "mixin_failure", "Mixin 的键");
    check_str(sxcl_log_reason_key((sxcl_log_reason)9999), "unknown", "越界的键按 unknown");
    {
        char text[512];
        check(sxcl_log_reason_text(SXCL_REASON_OUT_OF_MEMORY, "zh-cn", text, sizeof(text)) > 0,
              "合成一句建议");
        check(contains(text, "内存不足"), "合成句里有短名");
        check(contains(text, sxcl_log_reason_advice(SXCL_REASON_OUT_OF_MEMORY, "zh-cn")),
              "合成句里有建议全文");
        check(sxcl_log_reason_text(SXCL_REASON_OUT_OF_MEMORY, "en-us", text, sizeof(text)) > 0 &&
                  contains(text, "Out of memory"),
              "英文短名");
    }
}

/* ── 2) 逐行判定 ── */

typedef struct line_case {
    const char *line;
    sxcl_log_reason want;
} line_case;

static const line_case kLineCases[] = {
    {"java.lang.OutOfMemoryError: Java heap space", SXCL_REASON_OUT_OF_MEMORY},
    {"java.lang.OutOfMemoryError: GC overhead limit exceeded", SXCL_REASON_OUT_OF_MEMORY},
    {"Error occurred during initialization of VM", SXCL_REASON_UNKNOWN},
    {"Could not reserve enough space for 2097152KB object heap", SXCL_REASON_JVM_32BIT},
    {"Invalid maximum heap size: -Xmx999999m", SXCL_REASON_HEAP_TOO_LARGE},
    {"# A fatal error has been detected by the Java Runtime Environment:",
     SXCL_REASON_JVM_FATAL},
    {"#  EXCEPTION_ACCESS_VIOLATION (0xc0000005) at pc=0x00007ff", SXCL_REASON_JVM_FATAL},
    {"java.lang.StackOverflowError", SXCL_REASON_STACK_OVERFLOW},
    {"java.lang.UnsupportedClassVersionError: net/fabricmc/loader/impl/launch/knot/Knot has been "
     "compiled by a more recent version of the Java Runtime",
     SXCL_REASON_JAVA_VERSION_MISMATCH},
    {"[Render thread/ERROR]: GLFW error 65542: Failed to create window", SXCL_REASON_GLFW_INIT_FAILED},
    {"[Render thread/ERROR]: WGL: The driver does not appear to support OpenGL",
     SXCL_REASON_GRAPHICS_DRIVER_OUTDATED},
    {"OpenGL Renderer: Intel(R) UHD Graphics 630", SXCL_REASON_UNKNOWN}, /* INFO 环境行不算故障 */
    {"[Render thread/ERROR]: Failed to create window: Intel(R) UHD Graphics driver crashed",
     SXCL_REASON_GRAPHICS_DRIVER_INTEL},
    {"Caused by: org.lwjgl.opengl.OpenGLException: NVIDIA driver nvoglv64.dll crashed",
     SXCL_REASON_GRAPHICS_DRIVER_NVIDIA},
    {"[main/ERROR]: Radeon driver atio6axx.dll reported a fatal error",
     SXCL_REASON_GRAPHICS_DRIVER_AMD},
    {"[main/ERROR]: Mesa llvmpipe software rendering is not supported",
     SXCL_REASON_GRAPHICS_DRIVER_SOFTWARE},
    {"[main/ERROR]: Mesa gallium driver error: failed to load GL", SXCL_REASON_GRAPHICS_DRIVER_MESA},
    {"Duplicatemodsfoundexception: Duplicate mod: sodium (2 files)", SXCL_REASON_MOD_DUPLICATE},
    {"ModResolutionException: incompatible mod set! unresolved dependency on fabric-api",
     SXCL_REASON_MOD_RESOLUTION_CONFLICT},
    {"Missing or unsupported mandatory dependencies: mod 'create' requires mod 'flywheel'",
     SXCL_REASON_MOD_MISSING_DEPENDENCY},
    {"Mixin apply failed: mixin config sodium.mixins.json could not be applied",
     SXCL_REASON_MIXIN_FAILURE},
    {"[main/ERROR]: java.lang.UnsatisfiedLinkError: no lwjgl in java.library.path",
     SXCL_REASON_MISSING_JAVA_LIBRARY},
    {"java.lang.NoClassDefFoundError: net/minecraft/client/main/Main", SXCL_REASON_MISSING_CLASS},
    {"[main/ERROR]: Could not find or load main class net.minecraft.client.main.Main",
     SXCL_REASON_CLASSPATH_BROKEN},
    {"[main/ERROR]: invalid or corrupt jarfile client.jar", SXCL_REASON_CORRUPT_JAR},
    {"[main/ERROR]: Failed to extract native libraries to natives dir",
     SXCL_REASON_NATIVES_EXTRACT_FAILED},
    {"java.io.FileNotFoundException: assets/minecraft/lang/en_us.json (Access is denied)",
     SXCL_REASON_FILE_PERMISSION},
    {"java.io.IOException: No space left on device", SXCL_REASON_DISK_FULL},
    {"java.io.FileNotFoundException: C:\\Users\\me\\.minecraft\\versions\\1.0 (The system cannot "
     "find the path specified)",
     SXCL_REASON_PATH_NOT_FOUND},
    {"java.lang.UnsatisfiedLinkError: ... wrong ELF class: ELFCLASS32", SXCL_REASON_ARCH_MISMATCH},
    {"com.mojang.authlib.exceptions.AuthenticationException: Invalid session", 
     SXCL_REASON_ACCOUNT_INVALID_SESSION},
    {"java.net.ConnectException: Connection refused", SXCL_REASON_NETWORK_UNREACHABLE},
    {"java.net.UnknownHostException: piston-meta.mojang.com", SXCL_REASON_DNS_FAILURE},
    {"javax.net.ssl.SSLHandshakeException: PKIX path building failed", SXCL_REASON_SSL_FAILURE},
    {"java.net.SocketTimeoutException: Read timed out", SXCL_REASON_NET_TIMEOUT},
    {"[Render thread/INFO]: Vulkan device not found (VK_ERROR_INCOMPATIBLE_DRIVER), falling back "
     "to OpenGL",
     SXCL_REASON_VULKAN_UNAVAILABLE},
    {"[main/ERROR]: /storage/emulated/0/java/bin/java: Permission denied (mount noexec)",
     SXCL_REASON_ANDROID_NOEXEC},
    {"avc: denied { execute_no_trans } for path=\"/data/data/pkg/files/bin/java\"",
     SXCL_REASON_ANDROID_SELINUX},
    {"[Render thread/INFO]: Stopping!", SXCL_REASON_EXIT_OK},
    {"[main/INFO]: Loading Minecraft 1.0", SXCL_REASON_UNKNOWN},
    {NULL, SXCL_REASON_UNKNOWN},
};

static void test_lines(void)
{
    size_t i = 0;
    printf("[2] 逐行判定\n");
    for (i = 0; kLineCases[i].line != NULL; ++i) {
        const sxcl_log_reason got = sxcl_log_classify_reason(kLineCases[i].line);
        if (got != kLineCases[i].want) {
            printf("  [!!] 行 -> 键不符: got %s want %s | 行: %.70s\n",
                   sxcl_log_reason_key(got), sxcl_log_reason_key(kLineCases[i].want),
                   kLineCases[i].line);
            ++g_fail;
        } else {
            ++g_pass;
        }
    }
}

/* ── 3) 同一套分析:文本 -> 汇总 / 磁盘夹具 -> 取证 ── */

static const char *kReportOom =
    "---- Minecraft Crash Report ----\n"
    "Time: 2026-01-01 00:00:00\n"
    "Description: Ticking entity\n"
    "\n"
    "java.lang.OutOfMemoryError: Java heap space\n"
    "\tat net.minecraft.client.renderer.LevelRenderer.render(LevelRenderer.java:1)\n"
    "Caused by: java.lang.OutOfMemoryError: Java heap space\n";

static const char *kReportMods =
    "---- Minecraft Crash Report ----\n"
    "Description: Mod loading failures have occurred\n"
    "ModResolutionException: incompatible mod set!\n"
    "\tDuplicate mod: fabric-api\n"
    "Mixin apply failed: mixin config create.mixins.json\n";

static const char *kReportGfx =
    "---- Minecraft Crash Report ----\n"
    "Description: Rendering overlay\n"
    "Caused by: org.lwjgl.opengl.OpenGLException: The driver does not appear to support OpenGL\n"
    "\tOpenGL Renderer: Intel(R) HD Graphics 620 (driver igdumd64.dll)\n";

static void summarize_text(const char *text, sxcl_log_summary *s)
{
    const char *cursor = text;
    char line[1024];
    sxcl_log_summary_init(s);
    while (sxcl_text_next_line(&cursor, line, sizeof(line))) {
        (void)sxcl_log_summary_add(s, line);
    }
    sxcl_log_summary_finish(s);
}

static void test_reports(void)
{
    sxcl_log_summary s;
    printf("[3] 合成 crash-report -> 原因键\n");
    summarize_text(kReportOom, &s);
    check_str(s.reason_key, "out_of_memory", "OOM 报告命中 out_of_memory");
    check(s.conclusion == SXCL_LOG_CONCLUSION_CRASH, "OOM 报告结论是崩溃");
    check(contains(s.advice, "内存"), "OOM 的建议里说内存");
    check(contains(s.reason_advice, "4096"), "OOM 的建议给了具体做法");

    summarize_text(kReportMods, &s);
    check_str(s.reason_key, "mod_resolution_conflict", "模组冲突报告命中 mod_resolution_conflict");
    check(s.conclusion == SXCL_LOG_CONCLUSION_MOD, "模组报告结论是模组类");

    summarize_text(kReportGfx, &s);
    check(s.reason == SXCL_REASON_GRAPHICS_DRIVER_INTEL ||
              s.reason == SXCL_REASON_GRAPHICS_DRIVER_OUTDATED,
          "显卡驱动报告命中 Intel/驱动过旧");
    check(s.conclusion == SXCL_LOG_CONCLUSION_GRAPHICS, "图形报告结论是图形栈");
}

static void test_artifacts(void)
{
    sxcl_log_summary s;
    sxcl_crash_evidence ev;
    long long fed = 0;
    printf("[4] 磁盘夹具 -> sxcl_launch_scan_artifacts(与 stdout 同一套分析)\n");
    (void)sxcl_fs_mkdirs(GAME_DIR "/crash-reports");
    (void)sxcl_fs_mkdirs(GAME_DIR "/logs");
    write_text(GAME_DIR "/crash-reports/crash-2026-03-03_00.00.00-client.txt", kReportOom);
    write_text(GAME_DIR "/logs/latest.log",
               "[00:00:00] [main/INFO]: Loading Minecraft 1.0\n"
               "[00:00:01] [main/ERROR]: java.lang.OutOfMemoryError: Java heap space\n"
               "[00:00:02] [main/ERROR]: Duplicate mod: fabric-api\n");
    sxcl_log_summary_init(&s);
    memset(&ev, 0, sizeof(ev));
    fed = sxcl_launch_scan_artifacts(GAME_DIR, &s, &ev, SXCL_CRASH_SCAN_ALL, 0, NULL, 0);
    check(fed > 0, "取证喂进来了一些行");
    check(contains(ev.report_path, "crash-2026-03-03"), "读到了夹具里那份报告");
    check(ev.latest_log_lines == 3, "latest.log 行数 = 3");
    check_str(s.reason_key, "out_of_memory", "最高优先级的键是 out_of_memory(OOM 比重复模组更具体)");
    check(s.reason_score >= 90, "分数 >= 90(规则表里最具体的一档)");
    check(contains(s.reason_advice, "内存") || contains(s.reason_advice, "Memory"),
          "建议文案跟着键走");
    /* 只扫 latest.log:必须也能得到键(两条证据互相独立) */
    sxcl_log_summary_init(&s);
    fed = sxcl_launch_scan_artifacts(GAME_DIR, &s, NULL, SXCL_CRASH_SCAN_LATEST, 0, NULL, 0);
    check(fed == 3, "只扫 latest.log:3 行");
    check_str(s.reason_key, "out_of_memory", "latest.log 一路也能命中 out_of_memory");
    /* 参数错误 */
    check(sxcl_launch_scan_artifacts(NULL, NULL, NULL, 0, 0, NULL, 0) < 0, "缺游戏目录返回负值");
}

int main(void)
{
    printf("崩溃原因键测试\n");
    test_table();
    test_lines();
    test_reports();
    test_artifacts();
    printf("crash_reason 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
