/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#  include <sys/stat.h>
#endif

#include "sxcl/android.h"
#include "sxcl/fs.h"
#include "sxcl/launch.h"

#define FIXTURE_ROOT "sxcl_java_finder_fixture"

#if defined(_WIN32)
#  define HOST_OS SXCL_JAVA_OS_WINDOWS
#  define JAVA_EXE "java.exe"
#  define PATH_SEP ";"
#else
#  define HOST_OS SXCL_JAVA_OS_LINUX
#  define JAVA_EXE "java"
#  define PATH_SEP ":"
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

/* ── 夹具小工具 ── */

static void put_bytes(const char *path, const void *data, size_t len)
{
    FILE *fh = NULL;
    (void)sxcl_fs_mkdirs_for_file(path);
    fh = sxcl_fs_fopen(path, "wb");
    if (fh == NULL) {
        printf("  [!!] 夹具写不进去: %s\n", path);
        ++g_fail;
        return;
    }
    if (len > 0) {
        (void)fwrite(data, 1, len, fh);
    }
    (void)fclose(fh);
}

static void put_text(const char *path, const char *text)
{
    put_bytes(path, text, strlen(text));
}

/** 把夹具程序复制成 dst,必要时补执行位(POSIX 上起进程要求有 x)。 */
static void copy_exe(const char *src, const char *dst)
{
    FILE *in = NULL;
    FILE *out = NULL;
    char buf[8192];
    size_t got = 0;
    (void)sxcl_fs_mkdirs_for_file(dst);
    in = sxcl_fs_fopen(src, "rb");
    if (in == NULL) {
        printf("  [!!] 打不开夹具程序: %s\n", src);
        ++g_fail;
        return;
    }
    out = sxcl_fs_fopen(dst, "wb");
    if (out == NULL) {
        (void)fclose(in);
        printf("  [!!] 写不了假 java: %s\n", dst);
        ++g_fail;
        return;
    }
    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
        (void)fwrite(buf, 1, got, out);
    }
    (void)fclose(in);
    (void)fclose(out);
#if !defined(_WIN32)
    (void)chmod(dst, 0755);
#endif
}

/** 造一个 java home:<dir>/bin/java[.exe]。 */
static void make_home(const char *dir, const char *fake_exe)
{
    char exe[512];
    (void)snprintf(exe, sizeof(exe), "%s/bin/" JAVA_EXE, dir);
    copy_exe(fake_exe, exe);
}

/** 一段合法 ELF 头(只够 sxcl_java_binary_arch 读),e_machine 由 machine 决定。 */
static void put_elf(const char *path, unsigned int machine)
{
    unsigned char elf[64];
    (void)memset(elf, 0, sizeof(elf));
    elf[0] = 0x7F;
    elf[1] = 'E';
    elf[2] = 'L';
    elf[3] = 'F';
    elf[4] = 2; /* 64 位 */
    elf[5] = 1; /* 小端 */
    elf[18] = (unsigned char)(machine & 0xFFu);
    elf[19] = (unsigned char)((machine >> 8) & 0xFFu);
    put_bytes(path, elf, sizeof(elf));
}

/** 一段合法 PE 头(machine 由参数决定),e_lfanew = 0x40。 */
static void put_pe(const char *path, unsigned int machine)
{
    unsigned char pe[80];
    (void)memset(pe, 0, sizeof(pe));
    pe[0] = 'M';
    pe[1] = 'Z';
    pe[60] = 0x40; /* e_lfanew 低字节 */
    pe[0x40] = 'P';
    pe[0x41] = 'E';
    pe[0x44] = (unsigned char)(machine & 0xFFu);
    pe[0x45] = (unsigned char)((machine >> 8) & 0xFFu);
    put_bytes(path, pe, sizeof(pe));
}

/* ══════════════════ 1) java -version 文本解析(多厂商多版本) ══════════════════ */

typedef struct version_case {
    const char *text;
    int major;
    const char *vendor;
    const char *arch;
} version_case;

static void test_parse_version_text(void)
{
    static const version_case cases[] = {
        { "java version \"1.8.0_402\"\nJava(TM) SE Runtime Environment (build 1.8.0_402-b06)\n"
          "Java HotSpot(TM) 64-Bit Server VM (build 25.402-b06, mixed mode)\n",
          8, "Oracle", "x64" },
        { "openjdk version \"21.0.3\" 2024-04-16\n"
          "OpenJDK Runtime Environment Temurin-21.0.3+9 (build 21.0.3+9-LTS)\n"
          "OpenJDK 64-Bit Server VM Temurin-21.0.3+9 (build 21.0.3+9-LTS, mixed mode)\n",
          21, "Eclipse Adoptium", "x64" },
        { "openjdk version \"17.0.11\" 2024-04-16\n"
          "OpenJDK Runtime Environment Zulu17.50+19-CA (build 17.0.11+9-LTS)\n"
          "OpenJDK 64-Bit Server VM Zulu17.50+19-CA (build 17.0.11+9-LTS, mixed mode)\n",
          17, "Azul Zulu", "x64" },
        { "openjdk version \"11.0.22\" 2024-01-16 LTS\n"
          "OpenJDK Runtime Environment Corretto-11.0.22.7.1 (build 11.0.22+7-LTS)\n"
          "OpenJDK 64-Bit Server VM Corretto-11.0.22.7.1 (build 11.0.22+7-LTS, mixed mode)\n",
          11, "Amazon Corretto", "x64" },
        { "openjdk version \"1.8.0_402\"\n"
          "OpenJDK Runtime Environment (build 1.8.0_402-b06)\n"
          "OpenJDK 64-Bit Server VM (build 25.402-b06, mixed mode)\n",
          8, "OpenJDK", "x64" },
        { "java version \"17.0.10\" 2024-01-16 LTS\n"
          "Java(TM) SE Runtime Environment Microsoft-9387292 (build 17.0.10+11-LTS)\n"
          "Java HotSpot(TM) 64-Bit Server VM Microsoft-9387292 (build 17.0.10+11-LTS, mixed mode)\n",
          17, "Microsoft", "x64" },
        { "openjdk version \"21.0.2\" 2024-01-16\n"
          "OpenJDK Runtime Environment GraalVM CE 21.0.2+13.1 (build 21.0.2+13-jvmci-23.1-b30)\n"
          "OpenJDK 64-Bit Server VM GraalVM CE 21.0.2+13.1 (build 21.0.2+13-jvmci-23.1-b30,"
          " mixed mode)\n",
          21, "GraalVM", "x64" },
        { "openjdk version \"17.0.9\" 2023-10-17\n"
          "IBM Semeru Runtime Open Edition 17.0.9.0 (build 17.0.9+9)\n"
          "Eclipse OpenJ9 VM 17.0.9.0 (build openj9-0.41.0, JRE 17 Linux ppc64le-64-Bit"
          " 20231017_505)\n",
          17, "IBM Semeru", "x64" },
        { "openjdk version \"17.0.9\" 2023-10-17 LTS\n"
          "OpenJDK Runtime Environment (build 17.0.9+8-LTS)\n"
          "OpenJDK 64-Bit Server VM (build 17.0.9+8-LTS, mixed mode)\n"
          "OpenJDK 64-Bit Server VM warning: aarch64 64-Bit\n",
          17, "OpenJDK", "arm64" },
        { "openjdk version \"1.8.0_402\"\nOpenJDK Runtime Environment (Zulu 8.76.0.1-CA)\n"
          "OpenJDK 32-Bit Server VM (build 25.402-b06, mixed mode)\n",
          8, "Azul Zulu", "x86" },
    };
    size_t i = 0;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        sxcl_java_info info;
        (void)memset(&info, 0, sizeof(info));
        info.is_64bit = -1;
        info.is_jre = -1;
        if (sxcl_java_parse_version_output(cases[i].text, strlen(cases[i].text), &info) != 0) {
            printf("  [!!] 解析第 %zu 个样例失败: %s\n", i + 1, info.error);
            ++g_fail;
            continue;
        }
        check_int(info.major, cases[i].major, "解析:主版本");
        check_str(info.vendor, cases[i].vendor, "解析:厂商");
        check_str(info.arch, cases[i].arch, "解析:架构");
    }

    /* 认不出来的文本必须失败,而不是编一个版本出来 */
    {
        sxcl_java_info info;
        (void)memset(&info, 0, sizeof(info));
        (void)memset(&info.vendor, 0, sizeof(info.vendor));
        check_int(sxcl_java_parse_version_output("hello world\n", 12, &info), -1,
                  "解析:非 Java 输出必须失败");
        check_int(sxcl_java_parse_version_output("", 0, &info), -1, "解析:空输出必须失败");
    }

    /* 版本串 -> 主版本(单独的口子) */
    check_int(sxcl_java_major_of("21.0.3"), 21, "major_of:21.0.3");
    check_int(sxcl_java_major_of("1.8.0_402"), 8, "major_of:1.8.0_402");
    check_int(sxcl_java_major_of("17"), 17, "major_of:17");
    check_int(sxcl_java_major_of("废话"), 0, "major_of:认不出给 0");
}

/* ══════════════════ 2) 可执行文件机器码(读文件头) ══════════════════ */

static void test_binary_arch(void)
{
    char arch[16];
    arch[0] = '\0';
    check_int((long long)strlen(sxcl_java_host_arch()) > 0 ? 1 : 0, 1, "host_arch:非空");

    put_elf(FIXTURE_ROOT "/bin/elf_x64", 0x3E);
    put_elf(FIXTURE_ROOT "/bin/elf_x86", 0x03);
    put_elf(FIXTURE_ROOT "/bin/elf_arm64", 0xB7);
    put_elf(FIXTURE_ROOT "/bin/elf_arm32", 0x28);
    put_elf(FIXTURE_ROOT "/bin/elf_riscv", 0xF3);
    put_pe(FIXTURE_ROOT "/bin/pe_x64.exe", 0x8664);
    put_pe(FIXTURE_ROOT "/bin/pe_x86.exe", 0x014C);
    put_pe(FIXTURE_ROOT "/bin/pe_arm64.exe", 0xAA64);
    put_text(FIXTURE_ROOT "/bin/plain.txt", "this is not an executable at all\n");

    check_int(sxcl_java_binary_arch(FIXTURE_ROOT "/bin/elf_x64", arch, sizeof(arch)), 0,
              "arch:ELF x64 认得");
    check_str(arch, "x64", "arch:ELF x64 归一化");
    check_int(sxcl_java_binary_arch(FIXTURE_ROOT "/bin/elf_x86", arch, sizeof(arch)), 0,
              "arch:ELF x86 认得");
    check_str(arch, "x86", "arch:ELF x86 归一化");
    check_int(sxcl_java_binary_arch(FIXTURE_ROOT "/bin/elf_arm64", arch, sizeof(arch)), 0,
              "arch:ELF arm64 认得");
    check_str(arch, "arm64", "arch:ELF arm64 归一化");
    check_int(sxcl_java_binary_arch(FIXTURE_ROOT "/bin/elf_arm32", arch, sizeof(arch)), 0,
              "arch:ELF arm32 认得");
    check_str(arch, "arm32", "arch:ELF arm32 归一化");
    check_int(sxcl_java_binary_arch(FIXTURE_ROOT "/bin/pe_x64.exe", arch, sizeof(arch)), 0,
              "arch:PE x64 认得");
    check_str(arch, "x64", "arch:PE x64 归一化");
    check_int(sxcl_java_binary_arch(FIXTURE_ROOT "/bin/pe_x86.exe", arch, sizeof(arch)), 0,
              "arch:PE x86 认得");
    check_str(arch, "x86", "arch:PE x86 归一化");
    check_int(sxcl_java_binary_arch(FIXTURE_ROOT "/bin/pe_arm64.exe", arch, sizeof(arch)), 0,
              "arch:PE arm64 认得");
    check_str(arch, "arm64", "arch:PE arm64 归一化");
    check_int(sxcl_java_binary_arch(FIXTURE_ROOT "/bin/elf_riscv", arch, sizeof(arch)), -1,
              "arch:不认识的机器码 -> -1(不猜)");
    check_str(arch, "", "arch:认不出时输出为空");
    check_int(sxcl_java_binary_arch(FIXTURE_ROOT "/bin/plain.txt", arch, sizeof(arch)), -1,
              "arch:普通文本 -> -1");
    check_int(sxcl_java_binary_arch(FIXTURE_ROOT "/bin/nope", arch, sizeof(arch)), -1,
              "arch:文件不存在 -> -1");
    check_int(sxcl_java_binary_arch(NULL, arch, sizeof(arch)), -1, "arch:NULL -> -1");
}

/* ══════════════════ 3) 真的执行 java -version ══════════════════ */

static void test_exec_version(const char *fake_exe)
{
    sxcl_java_info info;
    char ok_exe[512];
    char notjre_exe[512];
    char silent_exe[512];
    (void)snprintf(ok_exe, sizeof(ok_exe), "%s/ok/" JAVA_EXE, FIXTURE_ROOT);
    (void)snprintf(notjre_exe, sizeof(notjre_exe), "%s/notjre/" JAVA_EXE, FIXTURE_ROOT);
    (void)snprintf(silent_exe, sizeof(silent_exe), "%s/silent/" JAVA_EXE, FIXTURE_ROOT);
    copy_exe(fake_exe, ok_exe);
    copy_exe(fake_exe, notjre_exe);
    copy_exe(fake_exe, silent_exe);

    /* 1) 正常的假 java:必须真的跑起来并解析出 21 / Temurin / 64 位 */
    (void)memset(&info, 0, sizeof(info));
    check_int(sxcl_java_exec_version(ok_exe, 8000, &info), 0, "exec:正常 java 返回成功");
    check_int(info.major, 21, "exec:主版本 21(来自**真实执行的输出**)");
    check_str(info.version, "21.0.3", "exec:完整版本串");
    check_str(info.vendor, "Eclipse Adoptium", "exec:厂商");
    check_int(info.is_64bit, 1, "exec:64 位");
    check_contains(info.path, JAVA_EXE, "exec:回填可执行文件路径");
    check_contains(info.home, "ok", "exec:回填 JAVA_HOME");

    /* 2) 能跑但输出不是 Java:必须报"没找到 version",不能瞎编 */
    (void)memset(&info, 0, sizeof(info));
    check_int(sxcl_java_exec_version(notjre_exe, 8000, &info), -1, "exec:非 Java 必须失败");
    check_contains(info.error, "version", "exec:失败原因说清是认不出版本");

    /* 3) 进程起不来(退出码 127 且没有输出):必须报出来,不能静默当"没有" */
    (void)memset(&info, 0, sizeof(info));
    check_int(sxcl_java_exec_version(silent_exe, 8000, &info), -1, "exec:起不来必须失败");
    check_contains(info.error, "127", "exec:失败原因带退出码 127");

    /* 4) 路径不存在 / 超时这两个边界 */
    (void)memset(&info, 0, sizeof(info));
    check_int(sxcl_java_exec_version(FIXTURE_ROOT "/nope/java", 8000, &info), -1,
              "exec:路径不存在必须失败");
    check_contains(info.error, "找不到", "exec:路径不存在的原因");
    check_int(sxcl_java_exec_version(NULL, 8000, &info), -1, "exec:NULL 路径必须失败");
    /* 给一个 JAVA_HOME 目录也能用(内部会拼成 <home>/bin/java[.exe]) */
    (void)memset(&info, 0, sizeof(info));
    check_int(sxcl_java_exec_version(FIXTURE_ROOT "/okhome", 8000, &info), 0,
              "exec:直接给 JAVA_HOME 目录也行");
    check_int(info.major, 21, "exec:给目录时也能解析出主版本");
}

/* ══════════════════ 4) 全链路 sxcl_java_detect ══════════════════ */

static const sxcl_java_installation *find_install(const sxcl_java_installations *all,
                                                  const char *needle)
{
    size_t i = 0;
    for (i = 0; i < all->count; ++i) {
        if (strstr(all->items[i].info.path, needle) != NULL ||
            strstr(all->items[i].info.home, needle) != NULL) {
            return &all->items[i];
        }
    }
    return NULL;
}

static void test_detect(void)
{
    sxcl_java_env env;
    sxcl_java_installations all;
    (void)memset(&env, 0, sizeof(env));
    env.java_home = FIXTURE_ROOT "/jdk21";
    env.program_files = FIXTURE_ROOT "/pf";
    env.user_home = FIXTURE_ROOT "/home";
    env.app_data = FIXTURE_ROOT "/appdata";
    /* PATH 候选在三个平台都成立(候选表按 PATH 的每一项拼 <dir>/java),所以
     * "不是 JRE" 与 "起不来" 这两种结论放这里,Windows/Linux/macOS 都能验。 */
    env.path = FIXTURE_ROOT "/p_notjre" PATH_SEP FIXTURE_ROOT "/p_silent" PATH_SEP
               FIXTURE_ROOT "/pathdir";

    check(sxcl_java_detect(&env, HOST_OS, 8000, &all) > 0, "detect:至少要有条目(不可用的也算)");
    check(all.usable >= 1, "detect:至少 1 条可用(JAVA_HOME 那份)");
    check(all.broken >= 2, "detect:broken 把'不是 Java'与'起不来'都数进去了");

    /* JAVA_HOME:真的执行过 */
    {
        const sxcl_java_installation *jdk = find_install(&all, "jdk21");
        check(jdk != NULL, "detect:JAVA_HOME 候选在列表里");
        if (jdk != NULL) {
            check_int(jdk->verdict, SXCL_JAVA_RUN_OK, "detect:JAVA_HOME 判定为可用");
            check_int(jdk->executed, 1, "detect:JAVA_HOME **真的执行过**(executed=1)");
            check_int(jdk->info.major, 21, "detect:JAVA_HOME 主版本");
            check_str(jdk->source, "JAVA_HOME", "detect:JAVA_HOME 来源键");
            check_contains(jdk->reason, "java -version", "detect:可用条目的原因写明是实测来的");
        }
    }

    /* ProgramFiles 扫描根里的那份(Windows 专有;别的平台这棵夹具树不在候选表里) */
#if defined(_WIN32)
    {
        const sxcl_java_installation *pf = find_install(&all, "jdk17");
        check(pf != NULL, "detect:ProgramFiles 的 Java 扫描根里的 JDK 被扫到");
        if (pf != NULL) {
            check_int(pf->verdict, SXCL_JAVA_RUN_OK, "detect:扫描来的 JDK 也可用");
            check_int(pf->executed, 1, "detect:扫描来的 JDK 也真的执行过");
        }
    }
#endif

    /* 能跑但不是 JRE:必须分类成"不是 Java",**不能**静默丢弃 */
    {
        const sxcl_java_installation *bad = find_install(&all, "notjre");
        check(bad != NULL, "detect:不是 JRE 的候选也被列出来(不静默丢弃)");
        if (bad != NULL) {
            check_int(bad->verdict, SXCL_JAVA_RUN_NOT_A_JRE, "detect:判定为不是 Java");
            check(bad->reason[0] != '\0', "detect:不是 Java 也有人话原因");
        }
    }

    /* 起不来的那份 */
    {
        const sxcl_java_installation *sil = find_install(&all, "silent");
        check(sil != NULL, "detect:起不来的候选也被列出来");
        if (sil != NULL) {
            check_int(sil->verdict, SXCL_JAVA_RUN_EXEC_FAILED, "detect:判定为跑不起来");
        }
    }

    /* 架构不符:arm64 的 ELF 摆在 x64 机器上 */
    {
        const sxcl_java_installation *arm = find_install(&all, "armjdk");
        if (arm != NULL) {
#if defined(_M_X64) || defined(__x86_64__)
            check_int(arm->verdict, SXCL_JAVA_RUN_ARCH_MISMATCH, "detect:判定为架构不符");
            check_contains(arm->reason, "arm64", "detect:架构不符的原因里带实际机器码");
            check_int(arm->executed, 0, "detect:架构不符的**不去执行**(免得白等一个失败进程)");
#else
            check(arm->verdict != SXCL_JAVA_RUN_MISSING, "detect:arm64 候选在本机被如实分类");
#endif
        } else {
            check(0, "detect:armjdk 候选应该被列出来");
        }
    }

    /* PATH 里那个不存在的位置:**不进列表**(候选表里 PATH 的每一项都会产生一条,
     * 全收下会把上限撑满,真正存在的反而被截掉 —— 实测踩过)。它仍然能被单独体检出来。 */
    {
        check(find_install(&all, "pathdir") == NULL, "detect:不存在的候选不进列表");
        sxcl_android_access access = sxcl_android_probe_path(FIXTURE_ROOT "/pathdir/" JAVA_EXE, 1,
                                                             NULL, 0);
        check_int(access, SXCL_ANDROID_MISSING, "detect:那个位置单独体检仍是'不在'");
    }

    /* 排序:优先级降序(JAVA_HOME 必须排在扫描根**之前**),同优先级按主版本降序 */
    {
        size_t i = 0;
        int java_home_at = -1;
        int scan_at = -1;
        for (i = 0; i < all.count; ++i) {
            if (strcmp(all.items[i].source, "JAVA_HOME") == 0 && java_home_at < 0) {
                java_home_at = (int)i;
            }
            if (strcmp(all.items[i].source, "ProgramFiles") == 0 && scan_at < 0) {
                scan_at = (int)i;
            }
        }
        check(java_home_at >= 0, "detect:JAVA_HOME 条目在列表里");
        if (java_home_at >= 0 && scan_at >= 0) {
            check(java_home_at < scan_at, "detect:JAVA_HOME 排在扫描根之前");
        }
        for (i = 1; i < all.count; ++i) {
            check(all.items[i - 1].priority >= all.items[i].priority, "detect:优先级单调不增");
        }
    }

    /* 结论文案:每个 verdict 都要有中文名/英文键/人话建议 */
    {
        int v = 0;
        for (v = 0; v < (int)SXCL_JAVA_RUN_COUNT; ++v) {
            const char *name = sxcl_java_run_verdict_name((sxcl_java_run_verdict)v);
            const char *key = sxcl_java_run_verdict_key((sxcl_java_run_verdict)v);
            const char *hint = sxcl_java_run_verdict_hint((sxcl_java_run_verdict)v);
            check(name != NULL && name[0] != '\0', "verdict:中文名非空");
            check(key != NULL && key[0] != '\0', "verdict:英文键非空");
            check(hint != NULL && hint[0] != '\0', "verdict:人话建议非空");
        }
        check_str(sxcl_java_run_verdict_key(SXCL_JAVA_RUN_ARCH_MISMATCH), "arch_mismatch",
                  "verdict:架构不符的英文键");
        check_str(sxcl_java_run_verdict_name(SXCL_JAVA_RUN_NOEXEC), "共享存储不能执行",
                  "verdict:noexec 的中文名");
        check_contains(sxcl_java_run_verdict_hint(SXCL_JAVA_RUN_DENIED), "私有目录",
                       "verdict:沙箱拒绝的建议说清原因");
    }

    check_int(sxcl_java_detect(&env, HOST_OS, 8000, NULL), 0, "detect:out=NULL 返回 0(不炸)");
}

int main(int argc, char **argv)
{
    const char *fake = (argc > 1) ? argv[1] : NULL;
    if (fake == NULL || fake[0] == '\0') {
        printf("用法: sxcl_java_finder_test <冒充 java 的夹具可执行文件>\n");
        return 2;
    }
    (void)sxcl_fs_remove_tree(FIXTURE_ROOT);

    /* 夹具树:三个 java home + 一个扫描根 + 一份 arm64 的假可执行文件 */
    make_home(FIXTURE_ROOT "/jdk21", fake);
    make_home(FIXTURE_ROOT "/okhome", fake);
    make_home(FIXTURE_ROOT "/pf/Java/jdk17", fake);
    make_home(FIXTURE_ROOT "/pf/Java/armjdk", fake);
    put_elf(FIXTURE_ROOT "/pf/Java/armjdk/bin/" JAVA_EXE, 0xB7);
    /* PATH 上的三份:不是 JRE / 起不来 / 压根不存在 */
    {
        char exe[512];
        (void)snprintf(exe, sizeof(exe), "%s/p_notjre/" JAVA_EXE, FIXTURE_ROOT);
        copy_exe(fake, exe);
        (void)snprintf(exe, sizeof(exe), "%s/p_silent/" JAVA_EXE, FIXTURE_ROOT);
        copy_exe(fake, exe);
    }
    (void)sxcl_fs_mkdirs(FIXTURE_ROOT "/pathdir");
    (void)sxcl_fs_mkdirs(FIXTURE_ROOT "/home");

    printf("[java] 1) java -version 文本解析(多厂商多版本)\n");
    test_parse_version_text();
    printf("[java] 2) 可执行文件机器码(ELF/PE/Mach-O)\n");
    test_binary_arch();
    printf("[java] 3) 真的执行 java -version\n");
    test_exec_version(fake);
    printf("[java] 4) 全链路 sxcl_java_detect\n");
    test_detect();

    (void)sxcl_fs_remove_tree(FIXTURE_ROOT);
    printf("\n[java] 通过 %d,失败 %d\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
