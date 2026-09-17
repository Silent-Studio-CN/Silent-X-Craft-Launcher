/* 启动层纯逻辑测试:Java 版本解析 / release 文件 / 候选排序 / 参数拼装 / 日志归类。
 *
 * 这里**不启动任何进程、不联网、不依赖机器上装了什么 Java**:
 *   - java -version 的输出是内嵌字符串(把已捕获的输出喂进解析函数是正式用法,不是测试后门);
 *   - release 文件也走内嵌字符串;另有一条真实文件的小用例,写在 build 目录下(不进仓库);
 *   - 日志样本是照着 Windows 与安卓(& Krypton 包装层 + Mali GPU)实际会出现的版式写的。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/json.h"
#include "sxcl/launch.h"

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

static int count_sub(const char *hay, const char *needle) {
    int n = 0;
    const char *p = hay;
    const size_t len = strlen(needle);
    if (len == 0) {
        return 0;
    }
    while ((p = strstr(p, needle)) != NULL) {
        ++n;
        p += len;
    }
    return n;
}

/* ── argv 小工具 ── */

static int arg_index(const sxcl_launch_args *args, const char *want) {
    size_t i = 0;
    for (i = 0; i < sxcl_launch_arg_count(args); ++i) {
        if (strcmp(sxcl_launch_arg_at(args, i), want) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int count_prefix(const sxcl_launch_args *args, const char *prefix) {
    int n = 0;
    size_t i = 0;
    const size_t len = strlen(prefix);
    for (i = 0; i < sxcl_launch_arg_count(args); ++i) {
        if (strncmp(sxcl_launch_arg_at(args, i), prefix, len) == 0) {
            ++n;
        }
    }
    return n;
}

static const char *arg_after(const sxcl_launch_args *args, const char *flag) {
    const int idx = arg_index(args, flag);
    if (idx < 0) {
        return NULL;
    }
    return sxcl_launch_arg_at(args, (size_t)idx + 1);
}

/** 以 prefix 开头的第一个参数整体(如 "-Xmx4096M")。 */
static const char *arg_with_prefix(const sxcl_launch_args *args, const char *prefix) {
    size_t i = 0;
    const size_t len = strlen(prefix);
    for (i = 0; i < sxcl_launch_arg_count(args); ++i) {
        if (strncmp(sxcl_launch_arg_at(args, i), prefix, len) == 0) {
            return sxcl_launch_arg_at(args, i);
        }
    }
    return NULL;
}

/** 以 prefix 开头的第一个参数里,prefix 之后的部分(如 "-Djava.library.path=" -> 路径)。 */
static const char *value_with_prefix(const sxcl_launch_args *args, const char *prefix) {
    const char *full = arg_with_prefix(args, prefix);
    return full ? full + strlen(prefix) : NULL;
}

/* ══════════════════ 1. Java 版本文本解析 ══════════════════ */

static const char *kVersion21 =
    "openjdk version \"21.0.3\" 2024-04-16\n"
    "OpenJDK Runtime Environment Temurin-21.0.3+9 (build 21.0.3+9-LTS)\n"
    "OpenJDK 64-Bit Server VM Temurin-21.0.3+9 (build 21.0.3+9-LTS, mixed mode, sharing)\n";

static const char *kVersion8 =
    "java version \"1.8.0_402\"\n"
    "Java(TM) SE Runtime Environment (build 1.8.0_402-b06)\n"
    "Java HotSpot(TM) 64-Bit Server VM (build 25.402-b06, mixed mode)\n";

static const char *kVersion8x86 =
    "java version \"1.8.0_402\"\n"
    "Java(TM) SE Runtime Environment (build 1.8.0_402-b06)\n"
    "Java HotSpot(TM) 32-Bit Server VM (build 25.402-b06, mixed mode)\n";

static const char *kReleaseJdk21 =
    "IMPLEMENTOR=\"Eclipse Adoptium\"\n"
    "IMPLEMENTOR_VERSION=\"Temurin-21.0.3+9\"\n"
    "JAVA_VERSION=\"21.0.3\"\n"
    "JAVA_VERSION_DATE=\"2024-04-16\"\n"
    "LIBC=\"glibc\"\n"
    "MODULES=\"java.base java.compiler java.datatransfer\"\n"
    "OS_ARCH=\"x86_64\"\n"
    "OS_NAME=\"Linux\"\n"
    "SOURCE=\".:git:9b6f4\"\n";

static const char *kReleaseJre17 =
    "JAVA_VERSION=\"17.0.9\"\n"
    "JAVA_RUNTIME_VERSION=\"17.0.9+9\"\n"
    "MODULES=\"java.base java.desktop\"\n"
    "OS_ARCH=\"aarch64\"\n"
    "OS_NAME=\"Linux\"\n";

static const char *kReleaseJdk8 =
    "JAVA_VERSION=\"1.8.0_402\"\n"
    "OS_NAME=\"Windows\"\n"
    "OS_VERSION=\"5.2\"\n"
    "OS_ARCH=\"amd64\"\n"
    "SOURCE=\"\"\n"
    "BUILD_TYPE=\"commercial\"\n";

static void test_java_version_text(void) {
    sxcl_java_info info;

    printf("[1] java -version / release 文本解析\n");
    memset(&info, 0, sizeof(info));
    check(sxcl_java_parse_version_output(kVersion21, strlen(kVersion21), &info) == 0,
          "openjdk version \"21.0.3\" 能解析");
    check_int(info.major, 21, "21.0.3 的主版本");
    check_str(info.version, "21.0.3", "完整版本串");
    check_str(info.vendor, "Eclipse Adoptium", "Temurin 被认成 Eclipse Adoptium");
    check_int(info.is_64bit, 1, "64-Bit Server VM -> 64 位");
    check_str(info.arch, "x64", "架构归一化成 x64");
    check_int(info.is_jre, -1, "-version 输出分不出 JRE/JDK,诚实地给 -1");

    memset(&info, 0, sizeof(info));
    check(sxcl_java_parse_version_output(kVersion8, strlen(kVersion8), &info) == 0, "Java 8 版式能解析");
    check_int(info.major, 8, "1.8.0_402 的主版本是 8(不是 1)");
    check_str(info.version, "1.8.0_402", "Java 8 完整版本串");
    check_str(info.vendor, "Oracle", "HotSpot/Java(TM) -> Oracle");

    memset(&info, 0, sizeof(info));
    check(sxcl_java_parse_version_output(kVersion8x86, strlen(kVersion8x86), &info) == 0, "32 位版式能解析");
    check_int(info.is_64bit, 0, "32-Bit -> 32 位");
    check_str(info.arch, "x86", "32 位架构归一化成 x86");

    memset(&info, 0, sizeof(info));
    check(sxcl_java_parse_version_output("nonsense output", 15, &info) != 0, "乱七八糟的输出返回失败");
    check_int(info.major, 0, "失败时主版本为 0");

    check_int(sxcl_java_major_of("1.8.0_402"), 8, "major_of 1.8.0_402");
    check_int(sxcl_java_major_of("21.0.3"), 21, "major_of 21.0.3");
    check_int(sxcl_java_major_of("17"), 17, "major_of 17");
    check_int(sxcl_java_major_of("9.0.4"), 9, "major_of 9.0.4");
    check_int(sxcl_java_major_of(""), 0, "major_of 空串");
    check_int(sxcl_java_major_of("abc"), 0, "major_of 非数字");

    printf("[2] release 文件(键值清单)\n");
    memset(&info, 0, sizeof(info));
    check(sxcl_java_parse_release(kReleaseJdk21, strlen(kReleaseJdk21), &info) == 0, "JDK 21 的 release 能解析");
    check_int(info.major, 21, "release: 主版本 21");
    check_str(info.vendor, "Eclipse Adoptium", "release: IMPLEMENTOR");
    check_int(info.is_64bit, 1, "release: os_arch=x86_64 -> 64 位");
    check_str(info.arch, "x64", "release: 架构 x64");
    check_int(info.is_jre, 0, "release: 完整 JDK -> 不是 JRE");

    memset(&info, 0, sizeof(info));
    check(sxcl_java_parse_release(kReleaseJre17, strlen(kReleaseJre17), &info) == 0, "JRE 17 的 release 能解析");
    check_int(info.major, 17, "release: 主版本 17");
    check_str(info.arch, "arm64", "release: os_arch=aarch64 -> arm64");
    check_int(info.is_jre, 1, "release: 有 JAVA_RUNTIME_VERSION -> 是 JRE 镜像");

    memset(&info, 0, sizeof(info));
    check(sxcl_java_parse_release(kReleaseJdk8, strlen(kReleaseJdk8), &info) == 0, "JDK 8 的 release 能解析");
    check_int(info.major, 8, "release: JDK 8 主版本");
    check_str(info.vendor, "Unknown", "release: 没有 IMPLEMENTOR 时厂商给 Unknown");
    check_int(info.is_64bit, 1, "release: amd64 -> 64 位");

    memset(&info, 0, sizeof(info));
    check(sxcl_java_parse_release("OS_NAME=\"Windows\"\n", 18, &info) != 0, "没有 JAVA_VERSION 的 release 失败");

    printf("[3] 真实文件:只读 <home>/release(不执行 java)\n");
    if (sxcl_fs_mkdirs("build/_launch_tmp/jdk-21/bin") == 0) {
        /* 用 sxcl 自己的落盘接口写夹具(不进仓库,就在 build/ 里),也顺带覆盖一次 fs 层 */
        sxcl_file *fixture = sxcl_file_open_write("build/_launch_tmp/jdk-21/release", -1);
        check(fixture != NULL, "写临时 release 文件");
        if (fixture) {
            (void)sxcl_file_write_at(fixture, kReleaseJdk21, strlen(kReleaseJdk21), 0);
            (void)sxcl_file_close(fixture);
        }
        fixture = sxcl_file_open_write("build/_launch_tmp/jdk-21/bin/java.exe", -1);
        if (fixture) {
            (void)sxcl_file_write_at(fixture, "not a real exe", 14, 0);
            (void)sxcl_file_close(fixture);
        }
        memset(&info, 0, sizeof(info));
        check(sxcl_java_inspect("build/_launch_tmp/jdk-21", &info) == 0, "inspect(JAVA_HOME 目录)");
        check_int(info.major, 21, "inspect: 版本来自 release");
        check_int(info.is_jre, 1, "inspect: bin/javac 不在 -> 判成 JRE(硬证据优先于文本)");
        check(strstr(info.path, "java.exe") != NULL, "inspect: 补出 java 可执行文件路径");
        check_int(sxcl_java_inspect("build/_launch_tmp/不存在", &info), -1, "inspect 不存在的路径失败");
    } else {
        printf("  [--] 建不了 build/_launch_tmp,跳过真实文件用例\n");
    }
}

/* ══════════════════ 2. 候选路径与排序 ══════════════════ */

static void test_java_paths(void) {
    sxcl_java_env env;
    sxcl_java_candidate cands[64];
    char roots[64][SXCL_JAVA_PATH_MAX];
    size_t n = 0;
    size_t i = 0;
    int found = 0;

    printf("[4] 候选路径与扫描根(纯字符串,按平台分叉)\n");
    memset(&env, 0, sizeof(env));
    env.java_home = "C:\\Java\\jdk-21";
    env.program_files = "C:\\Program Files";
    env.local_app_data = "C:\\Users\\me\\AppData\\Local";
    env.app_data = "C:\\Users\\me\\AppData\\Roaming";
    env.user_home = "C:\\Users\\me";
    env.path = "C:\\Windows\\system32;C:\\Tools\\jdk8\\bin";

    n = sxcl_java_candidate_paths(&env, SXCL_JAVA_OS_WINDOWS, cands, 64);
    check(n >= 3, "Windows: JAVA_HOME + PATH 两项都出得来");
    check_str(cands[0].path, "C:\\Java\\jdk-21\\bin\\java.exe", "Windows: JAVA_HOME 排第一(顺序即优先级)");
    check_str(cands[0].source, "JAVA_HOME", "Windows: 来源标记");
    found = 0;
    for (i = 0; i < n; ++i) {
        if (strstr(cands[i].path, "Tools\\jdk8\\bin\\java.exe")) {
            found = 1;
        }
    }
    check(found, "Windows: PATH 里的条目也在候选里");

    n = sxcl_java_scan_roots(&env, SXCL_JAVA_OS_WINDOWS, roots, 64);
    check(n > 0, "Windows: 有扫描根");
    found = 0;
    for (i = 0; i < n; ++i) {
        if (strcmp(roots[i], "C:\\Program Files\\Java") == 0) {
            found = 1;
        }
    }
    check(found, "Windows: Program Files\\Java 在扫描根里");
    found = 0;
    for (i = 0; i < n; ++i) {
        if (strcmp(roots[i], "C:\\Program Files\\Eclipse Adoptium") == 0) {
            found = 1;
        }
    }
    check(found, "Windows: Eclipse Adoptium 在扫描根里");
    found = 0;
    for (i = 0; i < n; ++i) {
        if (strstr(roots[i], "SilentXCraftLauncher\\runtime")) {
            found = 1;
        }
    }
    check(found, "Windows: 官方 JRE 运行时目录在扫描根里");
    found = 0;
    for (i = 0; i < n; ++i) {
        if (strstr(roots[i], "Programs\\Zulu")) {
            found = 1;
        }
    }
    check(found, "Windows: LOCALAPPDATA\\Programs\\Zulu 在扫描根里");

    memset(&env, 0, sizeof(env));
    env.user_home = "/home/u";
    n = sxcl_java_candidate_paths(&env, SXCL_JAVA_OS_LINUX, cands, 64);
    check(n >= 1, "Linux: 至少有 /usr/bin/java");
    check_str(cands[0].path, "/usr/bin/java", "Linux: 系统 java");
    n = sxcl_java_scan_roots(&env, SXCL_JAVA_OS_LINUX, roots, 64);
    found = 0;
    for (i = 0; i < n; ++i) {
        if (strcmp(roots[i], "/usr/lib/jvm") == 0) {
            found = 1;
        }
    }
    check(found, "Linux: /usr/lib/jvm 在扫描根里");
    found = 0;
    for (i = 0; i < n; ++i) {
        if (strcmp(roots[i], "/home/u/.sdkman/candidates/java") == 0) {
            found = 1;
        }
    }
    check(found, "Linux: ~/.sdkman 在扫描根里");

    memset(&env, 0, sizeof(env));
    env.user_home = "/Users/me";
    n = sxcl_java_scan_roots(&env, SXCL_JAVA_OS_MACOS, roots, 64);
    found = 0;
    for (i = 0; i < n; ++i) {
        if (strcmp(roots[i], "/Library/Java/JavaVirtualMachines") == 0) {
            found = 1;
        }
    }
    check(found, "macOS: /Library/Java/JavaVirtualMachines 在扫描根里");
    n = sxcl_java_candidate_paths(&env, SXCL_JAVA_OS_MACOS, cands, 64);
    found = 0;
    for (i = 0; i < n; ++i) {
        if (strcmp(cands[i].path, "/opt/homebrew/opt/openjdk/bin/java") == 0) {
            found = 1;
        }
    }
    check(found, "macOS: Homebrew 的 openjdk 是候选");

    memset(&env, 0, sizeof(env));
    env.user_home = "/data/user/0/com.example/files";
    n = sxcl_java_scan_roots(&env, SXCL_JAVA_OS_ANDROID, roots, 64);
    check(n >= 1, "Android: 私有运行时目录能算出来");
    check(strstr(roots[0], "files/runtime") != NULL, "Android: files/runtime 是扫描根");
    check_str(sxcl_java_os_name(SXCL_JAVA_OS_ANDROID), "linux", "Android 在 rules 里按 linux 匹配");
    check_str(sxcl_java_exe_name(SXCL_JAVA_OS_WINDOWS), "java.exe", "Windows 的可执行文件名");
    check_str(sxcl_java_exe_name(SXCL_JAVA_OS_LINUX), "java", "Linux 的可执行文件名");
}

static void fill_java(sxcl_java_info *info, const char *path, int major, int is64) {
    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "%s", path);
    info->major = major;
    info->is_64bit = is64;
}

static void test_java_rank(void) {
    sxcl_java_info list[6];
    size_t order[6];
    size_t n = 0;

    printf("[5] 候选排序:精确匹配 > 更高 > 更低\n");
    fill_java(&list[0], "C:/jdk8/bin/java.exe", 8, 1);
    fill_java(&list[1], "C:/jdk21a/bin/java.exe", 21, 1);
    fill_java(&list[2], "C:/jdk21b/bin/java.exe", 21, 0);
    fill_java(&list[3], "C:/jdk25/bin/java.exe", 25, 1);
    fill_java(&list[4], "C:/jdk17/bin/java.exe", 17, 1);
    fill_java(&list[5], "C:/broken/bin/java.exe", 0, -1);

    n = sxcl_java_rank(list, 6, 21, order, 6);
    check_int((long)n, 6, "required=21 时全部参与排序");
    check_int((long)list[order[0]].major, 21, "required=21: 精确匹配排第一");
    check_int(list[order[0]].is_64bit, 1, "同为 21 时 64 位优先");
    check_str(list[order[1]].path, "C:/jdk21b/bin/java.exe", "同为 21 时另一个跟上");
    check_int((long)list[order[2]].major, 25, "其次才是更高的 25");
    check_int((long)list[order[3]].major, 17, "再次是更低的 17");
    check_int((long)list[order[4]].major, 8, "再低是 8");
    check_int((long)list[order[5]].major, 0, "版本未知的垫底");

    n = sxcl_java_rank(list, 6, 20, order, 6);
    check_int((long)list[order[0]].major, 21, "required=20: 更高的里面挑最接近的(21)");
    check_int((long)list[order[1]].major, 21, "required=20: 两个 21 都在");
    check_int((long)list[order[2]].major, 25, "required=20: 再远一点的 25");
    check_int((long)list[order[3]].major, 17, "required=20: 不够用的里面挑最大的(17)");
    check_int((long)list[order[4]].major, 8, "required=20: 然后才是 8");

    n = sxcl_java_rank(list, 6, 8, order, 6);
    check_int((long)list[order[0]].major, 8, "required=8: 精确匹配 8");
    check_int((long)list[order[1]].major, 17, "required=8: 更高的里最小的 17");

    n = sxcl_java_rank(list, 6, 0, order, 6);
    check_int((long)list[order[0]].major, 25, "无要求时从新到旧(25 第一)");
    check_int((long)list[order[5]].major, 0, "无要求时未知的仍然垫底");

    n = sxcl_java_rank(list, 6, 21, order, 3);
    check_int((long)n, 3, "cap 限制生效");
    check_int(sxcl_java_required_major(NULL), 8, "没有版本 JSON 时按 Java 8(1.13 之前的行为)");
    {
        sxcl_json *doc = NULL;
        char err[128];
        const char *jv_json = "{\"javaVersion\":{\"component\":\"java-runtime-delta\",\"majorVersion\":21}}";
        const char *old_json = "{\"id\":\"1.12.2\"}";
        doc = sxcl_json_parse(jv_json, strlen(jv_json), err, sizeof(err));
        check(doc != NULL, "解析 javaVersion 样本");
        check_int(sxcl_java_required_major(doc), 21, "从版本 JSON 读 javaVersion.majorVersion");
        sxcl_json_free(doc);
        doc = sxcl_json_parse(old_json, strlen(old_json), err, sizeof(err));
        check_int(sxcl_java_required_major(doc), 8, "没有 javaVersion 的老版本要求 Java 8");
        sxcl_json_free(doc);
    }
    check_int((long)sxcl_java_rank(NULL, 0, 21, order, 6), 0, "空列表不炸");
}

/* ══════════════════ 3. 参数拼装 ══════════════════ */

/* 新格式版本 JSON:jvm 数组里同时有字符串、自带 -cp 的串、带 rules 的对象;
 * 库故意打乱顺序,并混入一条 disallow windows 的库和一条只有 natives 分类器的库。 */
static const char *kNewVersionJson =
    "{"
    "\"id\":\"1.21.4\","
    "\"type\":\"release\","
    "\"mainClass\":\"net.minecraft.client.main.Main\","
    "\"assetIndex\":{\"id\":\"17\"},"
    "\"arguments\":{"
    "  \"jvm\":["
    "    \"-Djava.library.path=${natives_directory}\","
    "    \"-cp\",\"${classpath}\","
    "    \"-Dminecraft.launcher.brand=${launcher_name}\","
    "    {\"rules\":[{\"action\":\"allow\",\"os\":{\"name\":\"windows\"}}],"
    "     \"value\":[\"-Dwindows.only=1\"]},"
    "    {\"rules\":[{\"action\":\"allow\"},"
    "               {\"action\":\"disallow\",\"os\":{\"name\":\"windows\"}}],"
    "     \"value\":\"-Dnot.on.windows=1\"}"
    "  ],"
    "  \"game\":["
    "    \"--username\",\"${auth_player_name}\","
    "    \"--version\",\"${version_name}\","
    "    \"--gameDir\",\"${game_directory}\","
    "    \"--assetsDir\",\"${assets_root}\","
    "    \"--assetIndex\",\"${assets_index_name}\","
    "    \"--uuid\",\"${auth_uuid}\","
    "    \"--accessToken\",\"${auth_access_token}\","
    "    \"--userType\",\"${user_type}\","
    "    \"--versionType\",\"${version_type}\","
    "    {\"rules\":[{\"action\":\"allow\",\"features\":{\"is_demo_user\":true}}],\"value\":\"--demo\"}"
    "  ]"
    "},"
    "\"libraries\":["
    "  {\"name\":\"org.lwjgl:lwjgl:3.3.3\",\"downloads\":{\"artifact\":{\"path\":\"org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar\"}}},"
    "  {\"name\":\"com.example:alpha:1.0\",\"downloads\":{\"artifact\":{\"path\":\"com/example/alpha/1.0/alpha-1.0.jar\"}}},"
    "  {\"name\":\"zzz:last:1.0\",\"rules\":[{\"action\":\"allow\"},"
    "   {\"action\":\"disallow\",\"os\":{\"name\":\"windows\"}}],"
    "   \"downloads\":{\"artifact\":{\"path\":\"zzz/last/1.0/last-1.0.jar\"}}},"
    "  {\"name\":\"only-natives:old:1.0\",\"downloads\":{\"classifiers\":{\"natives-windows\":{\"path\":\"n/w.jar\"}}}}"
    "]}";

/* 老格式版本 JSON(1.12 那种):只有 minecraftArguments。 */
static const char *kOldVersionJson =
    "{"
    "\"id\":\"1.12.2\","
    "\"mainClass\":\"net.minecraft.launchwrapper.Launch\","
    "\"minecraftArguments\":\"--username ${auth_player_name} --version ${version_name} "
    "--gameDir ${game_directory} --assetsDir ${game_assets} --assetIndex ${assets_index_name} "
    "--uuid ${auth_uuid} --accessToken ${auth_access_token} --userType ${user_type} "
    "--tweakClass optifine.OptiFineForgeTweaker\","
    "\"libraries\":["
    "  {\"name\":\"b:two:1.0\",\"downloads\":{\"artifact\":{\"path\":\"b/two/1.0/two-1.0.jar\"}}},"
    "  {\"name\":\"a:one:1.0\",\"downloads\":{\"artifact\":{\"path\":\"a/one/1.0/one-1.0.jar\"}}}"
    "]}";

static void test_args_new_format(void) {
    sxcl_json *doc = NULL;
    char err[256];
    sxcl_launch_ctx ctx;
    sxcl_launch_args *args = NULL;
    const char *cp = NULL;

    printf("[6] 参数拼装:新格式(arguments.jvm / arguments.game)\n");
    doc = sxcl_json_parse(kNewVersionJson, strlen(kNewVersionJson), err, sizeof(err));
    check(doc != NULL, "解析新格式版本 JSON");
    if (!doc) {
        printf("      parse err: %s\n", err);
        return;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.player_name = "Steve";
    ctx.uuid = "11112222-3333-4444-5555-666677778888";
    ctx.access_token = "tok";
    ctx.user_type = "msa";
    ctx.version_name = "1.21.4";
    ctx.version_type = "release";
    ctx.game_directory = "C:/mc/.minecraft";
    ctx.os = SXCL_LAUNCH_OS_WINDOWS;
    ctx.java_major = 21;
    ctx.is_64bit = 1;

    args = sxcl_launch_build_args(doc, &ctx, err, sizeof(err));
    check(args != NULL, "拼装成功");
    if (!args) {
        printf("      err: %s\n", err);
        sxcl_json_free(doc);
        return;
    }
    check(sxcl_launch_argv(args) != NULL, "argv 可直接给 process.h");
    check(sxcl_launch_argv(args)[sxcl_launch_arg_count(args)] == NULL, "argv 以 NULL 结尾");
    check(sxcl_launch_arg_count(args) > 20, "参数条数合理");
    check_str(arg_after(args, "--username"), "Steve", "占位符 auth_player_name 展开");
    check_str(arg_after(args, "--uuid"), "11112222-3333-4444-5555-666677778888", "占位符 auth_uuid");
    check_str(arg_after(args, "--accessToken"), "tok", "占位符 auth_access_token");
    check_str(arg_after(args, "--userType"), "msa", "占位符 user_type");
    check_str(arg_after(args, "--versionType"), "release", "占位符 version_type");
    check_str(arg_after(args, "--assetIndex"), "17", "assetIndex 从版本 JSON 兜底");
    check_str(arg_after(args, "--gameDir"), "C:\\mc\\.minecraft",
              "Windows 上 game_directory 的分隔符被归一化成反斜杠");
    check_str(arg_after(args, "--assetsDir"), "C:\\mc\\.minecraft\\assets", "assets_root 默认 <game>/assets");
    check_int(arg_index(args, "--demo"), -1, "features 规则的门控参数被丢掉(不启 Demo 模式)");

    check_int(count_prefix(args, "-Djava.library.path="), 1,
              "-Djava.library.path 只出现一次(版本 JSON 自带就不重复补)");
    check_int(count_prefix(args, "-Dorg.lwjgl.librarypath="), 1, "自动补上 -Dorg.lwjgl.librarypath");
    check_str(value_with_prefix(args, "-Dorg.lwjgl.librarypath="),
              "C:\\mc\\.minecraft\\versions\\1.21.4\\1.21.4-natives",
              "natives 目录默认 <game>/versions/<ver>/<ver>-natives");

    check_int(count_prefix(args, "-cp"), 1, "-cp 只出现一次");
    cp = arg_after(args, "-cp");
    check(cp != NULL, "取到 classpath");
    check_str(cp,
              "C:\\mc\\.minecraft\\versions\\1.21.4\\1.21.4.jar;"
              "C:\\mc\\.minecraft\\libraries\\com\\example\\alpha\\1.0\\alpha-1.0.jar;"
              "C:\\mc\\.minecraft\\libraries\\org\\lwjgl\\lwjgl\\3.3.3\\lwjgl-3.3.3.jar",
              "classpath:客户端 jar 在前 + 库按库名升序 + Windows 用 ';'");
    check_int(arg_index(args, "-Dwindows.only=1") >= 0, 1, "windows 专属 jvm 参数在 Windows 上保留");
    check_int(arg_index(args, "-Dnot.on.windows=1"), -1, "disallow windows 的 jvm 参数被过滤");
    check_int(arg_index(args, "net.minecraft.client.main.Main") >= 0, 1, "主类在 argv 里");
    check(arg_index(args, "--username") > arg_index(args, "net.minecraft.client.main.Main"),
          "游戏参数排在主类之后");
    check_str(value_with_prefix(args, "-Dminecraft.launcher.brand="), "SilentXCraftLauncher",
              "启动器品牌");
    check_int(count_prefix(args, "-Xmx"), 1, "默认内存只给一个 -Xmx");
    check_str(arg_with_prefix(args, "-Xmx"), "-Xmx4096M", "64 位默认 4096M");
    check_int(arg_index(args, "-XX:+UseG1GC") >= 0, 1, "64 位用 G1");
    check_int(arg_index(args, "-XX:+UseSerialGC"), -1, "64 位不加 SerialGC");
    check_int(arg_index(args, "--enable-native-access=ALL-UNNAMED") >= 0, 1, "Java 21 加 --enable-native-access");
    check_int(arg_index(args, "--sun-misc-unsafe-memory-access=allow"), -1,
              "Java 21 **不**加 unsafe 开关(该选项 JDK 23 才有,加了会让 JVM 拒绝启动)");
    check_int(arg_index(args, "-XX:+UseCompactObjectHeaders"), -1, "Java 21 不加紧凑对象头(JDK 24 才有)");
    check_int(arg_index(args, "-XstartOnFirstThread"), -1, "Windows 不加 macOS 的 -XstartOnFirstThread");
    check_int(count_sub(cp, ";"), 2, "classpath 里正好两个分隔符(三段)");
    check_int(count_sub(cp, "only-natives"), 0, "只有 natives 分类器的库不占 classpath");
    sxcl_launch_args_free(args);

    /* 同一个版本 JSON 换到 Linux:分隔符、rules、路径归一化都要跟着变 */
    memset(&ctx, 0, sizeof(ctx));
    ctx.player_name = "Alex";
    ctx.version_name = "1.21.4";
    ctx.game_directory = "C:/mc/.minecraft";
    ctx.os = SXCL_LAUNCH_OS_LINUX;
    ctx.java_major = 21;
    ctx.is_64bit = 1;
    args = sxcl_launch_build_args(doc, &ctx, err, sizeof(err));
    check(args != NULL, "Linux 上下文也能拼");
    if (args) {
        cp = arg_after(args, "-cp");
        check(cp != NULL, "Linux 取到 classpath");
        check_str(cp,
                  "C:/mc/.minecraft/versions/1.21.4/1.21.4.jar:"
                  "C:/mc/.minecraft/libraries/com/example/alpha/1.0/alpha-1.0.jar:"
                  "C:/mc/.minecraft/libraries/org/lwjgl/lwjgl/3.3.3/lwjgl-3.3.3.jar:"
                  "C:/mc/.minecraft/libraries/zzz/last/1.0/last-1.0.jar",
                  "Linux:':' 分隔 + disallow windows 的库反过来被保留");
        check_str(arg_after(args, "--gameDir"), "C:/mc/.minecraft", "Linux 上路径保持正斜杠");
        check(arg_index(args, "-Dwindows.only=1") < 0, "Linux 上 windows 专属参数被过滤");
        check(arg_index(args, "-Dnot.on.windows=1") >= 0, "Linux 上反 windows 的参数被保留");
        check_int(arg_index(args, "-Djava.awt.headless=true") >= 0, 1, "Linux 加 headless");
        sxcl_launch_args_free(args);
    }

    /* macOS:必须带 -XstartOnFirstThread */
    memset(&ctx, 0, sizeof(ctx));
    ctx.version_name = "1.21.4";
    ctx.game_directory = "/Users/me/.minecraft";
    ctx.os = SXCL_LAUNCH_OS_MACOS;
    ctx.java_major = 21;
    ctx.is_64bit = 1;
    args = sxcl_launch_build_args(doc, &ctx, err, sizeof(err));
    check(args != NULL, "macOS 上下文也能拼");
    if (args) {
        check_int(arg_index(args, "-XstartOnFirstThread") >= 0, 1, "macOS 必须 -XstartOnFirstThread");
        sxcl_launch_args_free(args);
    }

    /* Android:走 linux 的 rules,classpath 用 ':' */
    memset(&ctx, 0, sizeof(ctx));
    ctx.version_name = "1.21.4";
    ctx.game_directory = "/data/user/0/app/files/.minecraft";
    ctx.os = SXCL_LAUNCH_OS_ANDROID;
    ctx.java_major = 17;
    ctx.is_64bit = 1;
    args = sxcl_launch_build_args(doc, &ctx, err, sizeof(err));
    check(args != NULL, "Android 上下文也能拼");
    if (args) {
        const char *cp2 = arg_after(args, "-cp");
        check(cp2 && strchr(cp2, ':') != NULL, "Android 用 ':' 分隔 classpath");
        check(cp2 && strchr(cp2, '\\') == NULL, "Android 上不出现反斜杠");
        sxcl_launch_args_free(args);
    }

    sxcl_json_free(doc);
}

static void test_args_old_format(void) {
    sxcl_json *doc = NULL;
    char err[256];
    sxcl_launch_ctx ctx;
    sxcl_launch_args *args = NULL;
    const char *cp = NULL;

    printf("[7] 参数拼装:老格式(minecraftArguments)+ 内存/GC 覆盖\n");
    doc = sxcl_json_parse(kOldVersionJson, strlen(kOldVersionJson), err, sizeof(err));
    check(doc != NULL, "解析老格式版本 JSON");
    if (!doc) {
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.player_name = "Notch";
    ctx.version_name = "1.12.2";
    ctx.game_directory = "C:/mc/.minecraft";
    ctx.os = SXCL_LAUNCH_OS_WINDOWS;
    ctx.java_major = 8;
    ctx.is_64bit = 0;
    args = sxcl_launch_build_args(doc, &ctx, err, sizeof(err));
    check(args != NULL, "老格式拼装成功");
    if (args) {
        check_str(arg_after(args, "--tweakClass"), "optifine.OptiFineForgeTweaker", "老格式参数原样保留");
        check_str(arg_after(args, "--assetsDir"), "C:\\mc\\.minecraft\\assets\\virtual\\legacy",
                  "game_assets 展开成 <assets>/virtual/legacy");
        check_int(count_prefix(args, "-cp"), 1, "没有自带 -cp 时由我们补上");
        cp = arg_after(args, "-cp");
        check_str(cp,
                  "C:\\mc\\.minecraft\\versions\\1.12.2\\1.12.2.jar;"
                  "C:\\mc\\.minecraft\\libraries\\a\\one\\1.0\\one-1.0.jar;"
                  "C:\\mc\\.minecraft\\libraries\\b\\two\\1.0\\two-1.0.jar",
                  "老格式 classpath 也按库名排序(a 在 b 前)");
        check_int(arg_index(args, "net.minecraft.launchwrapper.Launch") >= 0, 1, "老格式主类");
        check_str(arg_with_prefix(args, "-Xmx"), "-Xmx1024M", "32 位默认 1024M");
        check_int(arg_index(args, "-XX:+UseSerialGC") >= 0, 1, "32 位用 SerialGC(G1 会拒绝启动)");
        check_int(arg_index(args, "-XX:+UseG1GC"), -1, "32 位不加 G1");
        check_int(arg_index(args, "--enable-native-access=ALL-UNNAMED"), -1, "Java 8 不加 JDK17 的选项");
        sxcl_launch_args_free(args);
    }

    /* 调用方覆盖:内存 + 追加参数 */
    {
        const char *extra_jvm[] = { "-Dmy.flag=1", NULL };
        const char *extra_game[] = { "--width", "1280", "--height", "720", NULL };
        memset(&ctx, 0, sizeof(ctx));
        ctx.version_name = "1.12.2";
        ctx.game_directory = "C:/mc/.minecraft";
        ctx.os = SXCL_LAUNCH_OS_WINDOWS;
        ctx.java_major = 8;
        ctx.is_64bit = 1;
        ctx.memory_mb = 8192;
        ctx.extra_jvm_args = extra_jvm;
        ctx.extra_game_args = extra_game;
        args = sxcl_launch_build_args(doc, &ctx, err, sizeof(err));
        check(args != NULL, "带覆盖的上下文能拼");
        if (args) {
            check_str(arg_with_prefix(args, "-Xmx"), "-Xmx8192M", "memory_mb 覆盖默认内存");
            check_int(arg_index(args, "-Dmy.flag=1") >= 0, 1, "extra_jvm_args 追加进 argv");
            check_str(arg_after(args, "--width"), "1280", "extra_game_args 追加到游戏参数尾部");
            check(arg_index(args, "--width") > arg_index(args, "--username"),
                  "追加的游戏参数在原来的游戏参数之后");
            sxcl_launch_args_free(args);
        }
    }
    /* 调用方覆盖:整块替换默认 JVM 参数 */
    {
        const char *replace_jvm[] = { "-Xmx512M", "-Dreplaced=1", NULL };
        memset(&ctx, 0, sizeof(ctx));
        ctx.version_name = "1.12.2";
        ctx.game_directory = "C:/mc/.minecraft";
        ctx.os = SXCL_LAUNCH_OS_WINDOWS;
        ctx.java_major = 21;
        ctx.is_64bit = 1;
        ctx.jvm_args = replace_jvm;
        args = sxcl_launch_build_args(doc, &ctx, err, sizeof(err));
        check(args != NULL, "jvm_args 整块替换能拼");
        if (args) {
            check_int(count_prefix(args, "-Xmx"), 1, "整块替换后只剩调用方那一个 -Xmx");
            check_str(arg_with_prefix(args, "-Xmx"), "-Xmx512M", "整块替换后内存就是调用方给的");
            check_int(arg_index(args, "-Dreplaced=1") >= 0, 1, "整块替换的内容进了 argv");
            check_int(arg_index(args, "-Dfile.encoding=UTF-8"), -1, "整块替换后默认参数一条都不加");
            sxcl_launch_args_free(args);
        }
    }
    /* 版本相关开关的门槛:Java 23 才有 unsafe 开关,Java 24 才有紧凑对象头 */
    {
        memset(&ctx, 0, sizeof(ctx));
        ctx.version_name = "1.12.2";
        ctx.game_directory = "C:/mc/.minecraft";
        ctx.os = SXCL_LAUNCH_OS_WINDOWS;
        ctx.java_major = 23;
        ctx.is_64bit = 1;
        args = sxcl_launch_build_args(doc, &ctx, err, sizeof(err));
        if (args) {
            check_int(arg_index(args, "--sun-misc-unsafe-memory-access=allow") >= 0, 1,
                      "Java 23 加 unsafe 开关");
            check_int(arg_index(args, "-XX:+UseCompactObjectHeaders"), -1,
                      "Java 23 还没有紧凑对象头");
            sxcl_launch_args_free(args);
        }
    }
    {
        memset(&ctx, 0, sizeof(ctx));
        ctx.version_name = "1.12.2";
        ctx.game_directory = "C:/mc/.minecraft";
        ctx.os = SXCL_LAUNCH_OS_WINDOWS;
        ctx.java_major = 24;
        ctx.is_64bit = 1;
        args = sxcl_launch_build_args(doc, &ctx, err, sizeof(err));
        if (args) {
            check_int(arg_index(args, "-XX:+UseCompactObjectHeaders") >= 0, 1,
                      "Java 24 加 -XX:+UseCompactObjectHeaders");
            check_int(count_prefix(args, "-XX:+UnlockExperimentalVMOptions"), 1,
                      "UnlockExperimentalVMOptions 不重复");
            sxcl_launch_args_free(args);
        }
    }
    /* 位数未知:走保守档 */
    {
        memset(&ctx, 0, sizeof(ctx));
        ctx.version_name = "1.12.2";
        ctx.game_directory = "C:/mc/.minecraft";
        ctx.os = SXCL_LAUNCH_OS_WINDOWS;
        ctx.java_major = 0;
        ctx.is_64bit = -1;
        args = sxcl_launch_build_args(doc, &ctx, err, sizeof(err));
        if (args) {
            check_str(arg_with_prefix(args, "-Xmx"), "-Xmx2048M", "位数未知时取中间档 2048M");
            check_int(arg_index(args, "-XX:+UseSerialGC") >= 0, 1, "位数未知时不赌 G1");
            check_int(arg_index(args, "--enable-native-access=ALL-UNNAMED"), -1,
                      "Java 版本未知时不加版本相关选项(认不出的选项会让 JVM 直接拒绝启动)");
            sxcl_launch_args_free(args);
        }
    }
    check_int(sxcl_launch_default_memory_mb(1), 4096, "默认内存档:64 位");
    check_int(sxcl_launch_default_memory_mb(0), 1024, "默认内存档:32 位");
    check_int(sxcl_launch_default_memory_mb(-1), 2048, "默认内存档:未知");
    {
        sxcl_launch_ctx ctx2;
        char out[256];
        memset(&ctx2, 0, sizeof(ctx2));
        ctx2.game_directory = "C:/mc";
        ctx2.version_name = "1.21.4";
        ctx2.os = SXCL_LAUNCH_OS_WINDOWS;
        check_int((long)sxcl_launch_expand("${version_name}${classpath_separator}${unknown_thing}",
                                           &ctx2, out, sizeof(out)),
                  23, "expand:返回值是写入长度");
        check_str(out, "1.21.4;${unknown_thing}", "expand:未知占位符原样保留");
        check_int(sxcl_launch_build_args(doc, NULL, err, sizeof(err)) == NULL, 1, "ctx=NULL 报错");
        check(err[0] != '\0', "错误信息写进了 err");
    }
    sxcl_json_free(doc);
}

/* ══════════════════ 4. 日志归类 ══════════════════ */

/* Windows 侧样本(1.21 + Fabric 的常见版式) */
static const char *kWinLines[] = {
    "[12:34:56] [main/INFO]: Loading Minecraft 1.21.4 with Fabric Loader 0.16.9",
    "[12:34:58] [Render thread/INFO]: Backend library: LWJGL version 3.3.3+5",
    "[12:34:59] [Render thread/INFO]: OpenGL Vendor: NVIDIA Corporation",
    "[12:34:59] [Render thread/INFO]: OpenGL Renderer: NVIDIA GeForce RTX 4060/PCIe/SSE2",
    "[12:34:59] [Render thread/INFO]: OpenGL Version: 4.6.0 NVIDIA 566.36",
    "[12:35:00] [Render thread/WARN]: Vulkan is not supported on this device, falling back to OpenGL",
    "[12:35:01] [main/ERROR]: java.lang.ClassNotFoundException: org.lwjgl.glfw.GLFW",
    "[12:35:02] [main/ERROR]: java.lang.UnsupportedClassVersionError: net/fabricmc/loader/impl/launch/knot/Knot has been compiled by a more recent version of the Java Runtime (class file version 65.0), this version of the Java Runtime only recognizes class file versions up to 52.0",
    "[12:35:02] [main/WARN]: Mod 'sodium' (0.6.5) requires version 1.21.3 of minecraft, but 1.21.4 is present",
    "# A fatal error has been detected by the Java Runtime Environment:",
    "#  EXCEPTION_ACCESS_VIOLATION (0xc0000005) at pc=0x00007ffb1a2b3c4d, pid=1234, tid=5678",
    "[12:35:03] [main/INFO]: Stopping!",
    "[12:35:04] [Render thread/ERROR]: Failed to create GLFW window",
    "[12:35:05] [main/ERROR]: java.lang.OutOfMemoryError: Java heap space",
    "[12:35:06] [main/WARN]: Unable to connect to https://api.minecraftservices.com/minecraft/profile: java.net.ConnectException: Connection refused",
    NULL
};

/* 安卓侧样本:Krypton 包装层 + Mali GPU。
 * 已知事实:把图形 API 切到 Vulkan,这台设备会回退到 OpenGL —— 日志里必须能认出来。 */
static const char *kAndroidLines[] = {
    "03-12 10:00:00.100  1234  1234 I Krypton : [Krypton] [main/INFO]: Running on Android 14 (API 34), ABI arm64-v8a",
    "03-12 10:00:00.500  1234  1234 I Krypton : [Krypton/JVM] Java 17.0.9-internal",
    "03-12 10:00:01.234  1234  1234 I Krypton : [Render thread/INFO]: Backend library: LWJGL version 3.3.3+5",
    "03-12 10:00:02.001  1234  1234 I Krypton : [Render thread/INFO]: OpenGL Renderer: Mali-G68 MC4",
    "03-12 10:00:02.002  1234  1234 I Krypton : [Render thread/INFO]: OpenGL Version: OpenGL ES 3.2 v1.r38p1-01eac0",
    "03-12 10:00:03.500  1234  1234 W Krypton : [Render thread/WARN]: Vulkan device not found (VK_ERROR_INCOMPATIBLE_DRIVER), falling back to OpenGL ES",
    "03-12 10:00:04.100  1234  1234 E Krypton : [Render thread/ERROR]: java.lang.UnsatisfiedLinkError: dlopen failed: library \"libglfw.so\" not found",
    "03-12 10:00:05.900  1234  1234 I Krypton : Minecraft exited with code 0",
    "03-12 10:00:06.000  1234  1234 E Krypton : Process exited with code 1",
    NULL
};

static void test_log_line(void) {
    sxcl_log_line info;
    sxcl_log_kind kind = SXCL_LOG_UNKNOWN;

    printf("[8] 日志归类:Windows 样本\n");
    kind = sxcl_log_scan_line(kWinLines[0], &info);
    check_int(kind, SXCL_LOG_UNKNOWN, "普通 INFO 行不硬扣帽子");
    kind = sxcl_log_scan_line(kWinLines[1], &info);
    check_int(kind, SXCL_LOG_GRAPHICS, "LWJGL 版本行 -> 图形栈");
    check_int(info.severity, 0, "INFO 行 severity=0");
    kind = sxcl_log_scan_line(kWinLines[3], &info);
    check_int(kind, SXCL_LOG_GRAPHICS, "OpenGL Renderer 行 -> 图形栈");
    check_str(info.gl_renderer, "NVIDIA GeForce RTX 4060/PCIe/SSE2", "抽出渲染器字符串");
    kind = sxcl_log_scan_line(kWinLines[4], &info);
    check_int(kind, SXCL_LOG_GRAPHICS, "OpenGL Version 行 -> 图形栈");
    check_str(info.gl_version, "4.6.0 NVIDIA 566.36", "抽出 GL 版本串");
    (void)sxcl_log_scan_line(kWinLines[1], &info);
    check_str(info.gl_version, "", "LWJGL 的版本号不会被误当成 GL 版本");

    kind = sxcl_log_scan_line(kWinLines[5], &info);
    check_int(kind, SXCL_LOG_VULKAN_FALLBACK, "Vulkan 回退提示 -> 专门的类别");
    check_int(info.vulkan_fallback, 1, "vulkan_fallback 标记");
    check_int(info.severity, 1, "WARN 行 severity=1");

    kind = sxcl_log_scan_line(kWinLines[6], &info);
    check_int(kind, SXCL_LOG_MISSING, "ClassNotFoundException -> 缺东西");
    check_str(info.missing, "org.lwjgl.glfw.GLFW", "抽出缺失的类名");
    check_int(info.severity, 2, "ERROR 行 severity=2");

    kind = sxcl_log_scan_line(kWinLines[7], &info);
    check_int(kind, SXCL_LOG_JAVA_VERSION, "UnsupportedClassVersionError -> Java 版本不符");

    kind = sxcl_log_scan_line(kWinLines[8], &info);
    check_int(kind, SXCL_LOG_MOD_LOADER, "模组要求不满足 -> 模组/加载器");

    kind = sxcl_log_scan_line(kWinLines[9], &info);
    check_int(kind, SXCL_LOG_CRASH, "hs_err 头 -> 崩溃");
    check(strstr(info.reason, "fatal error") != NULL, "崩溃原因首行被抽出来");
    kind = sxcl_log_scan_line(kWinLines[10], &info);
    check_int(kind, SXCL_LOG_CRASH, "EXCEPTION_ACCESS_VIOLATION -> 崩溃");
    check(strstr(info.reason, "EXCEPTION_ACCESS_VIOLATION") != NULL, "访问违例被抽成原因");

    kind = sxcl_log_scan_line(kWinLines[11], &info);
    check_int(kind, SXCL_LOG_EXIT_OK, "Stopping! -> 正常退出");

    kind = sxcl_log_scan_line(kWinLines[12], &info);
    check_int(kind, SXCL_LOG_GRAPHICS, "GLFW 建窗失败 -> 图形栈");
    check_int(info.severity, 2, "ERROR 行 severity=2");

    kind = sxcl_log_scan_line(kWinLines[13], &info);
    check_int(kind, SXCL_LOG_CRASH, "OutOfMemoryError -> 崩溃");
    check(strstr(info.reason, "Java heap space") != NULL, "OOM 原因被抽出来");

    kind = sxcl_log_scan_line(kWinLines[14], &info);
    check_int(kind, SXCL_LOG_ACCOUNT_NET, "登录/网络异常 -> 账户网络");

    printf("[9] 日志归类:安卓样本(Krypton + Mali)\n");
    kind = sxcl_log_scan_line(kAndroidLines[0], &info);
    check_int(kind, SXCL_LOG_UNKNOWN, "Krypton 的启动信息行没有故障");
    kind = sxcl_log_scan_line(kAndroidLines[1], &info);
    check_str(info.java_version, "17.0.9-internal", "从包装层日志里抽出 Java 版本");
    check_int(kind, SXCL_LOG_UNKNOWN, "单纯报 Java 版本不算版本不符");
    kind = sxcl_log_scan_line(kAndroidLines[2], &info);
    check_int(kind, SXCL_LOG_GRAPHICS, "安卓上的 LWJGL 行");
    kind = sxcl_log_scan_line(kAndroidLines[3], &info);
    check_int(kind, SXCL_LOG_GRAPHICS, "Mali GPU 行");
    check_str(info.gl_renderer, "Mali-G68 MC4", "抽出 Mali 渲染器");
    kind = sxcl_log_scan_line(kAndroidLines[4], &info);
    check_int(kind, SXCL_LOG_GRAPHICS, "OpenGL ES 版本行");
    check_str(info.gl_version, "OpenGL ES 3.2 v1.r38p1-01eac0", "抽出 OpenGL ES 版本");

    kind = sxcl_log_scan_line(kAndroidLines[5], &info);
    check_int(kind, SXCL_LOG_VULKAN_FALLBACK, "安卓切 Vulkan 回退到 OGL -> 认出来");
    check_int(info.vulkan_fallback, 1, "安卓回退标记");
    check_int(info.severity, 1, "logcat 的 W 被认成警告");

    kind = sxcl_log_scan_line(kAndroidLines[6], &info);
    check_int(kind, SXCL_LOG_MISSING, "dlopen failed -> 缺原生库");
    check_str(info.missing, "libglfw.so", "抽出缺失的 .so 名");
    check_int(info.severity, 2, "logcat 的 E 被认成错误");

    kind = sxcl_log_scan_line(kAndroidLines[7], &info);
    check_int(kind, SXCL_LOG_EXIT_OK, "安卓上的正常退出");
    check_int(info.exit_code, 0, "抽出退出码 0");

    kind = sxcl_log_scan_line(kAndroidLines[8], &info);
    check_int(kind, SXCL_LOG_CRASH, "退出码非 0 归到崩溃");
    check_int(info.exit_code, 1, "抽出退出码 1");

    check_int(sxcl_log_scan_line(NULL, NULL), SXCL_LOG_UNKNOWN, "空行不炸");
    check_int(sxcl_log_scan_line("", &info), SXCL_LOG_UNKNOWN, "空串不炸");
    (void)sxcl_log_scan_line(kWinLines[6], &info);
    check_str(sxcl_log_kind_name(info.kind), "missing", "类别名");
    check_str(sxcl_log_conclusion_name(SXCL_LOG_CONCLUSION_VULKAN_FALLBACK), "vulkan_fallback",
              "结论文本键");
}

static void test_log_summary(void) {
    sxcl_log_summary s;
    const char *win_ok[] = { kWinLines[0], kWinLines[1], kWinLines[2], kWinLines[3], kWinLines[4],
                             kWinLines[11] };
    const char *win_java[] = { kWinLines[0], kWinLines[7], kWinLines[6], kWinLines[9] };
    const char *android_fallback[] = { kAndroidLines[0], kAndroidLines[1], kAndroidLines[3],
                                       kAndroidLines[4], kAndroidLines[5], kAndroidLines[7] };
    const char *android_broken[] = { kAndroidLines[0], kAndroidLines[2], kAndroidLines[6],
                                     kAndroidLines[8] };
    const char *win_graphics[] = { kWinLines[1], kWinLines[12] };
    const char *win_account[] = { kWinLines[14] };
    const char *win_oom[] = { kWinLines[0], kWinLines[13] };

    printf("[10] 汇总结论\n");
    sxcl_log_summarize(win_ok, 6, &s);
    check_int((long)s.lines, 6, "行数统计");
    check_int(s.conclusion, SXCL_LOG_CONCLUSION_OK, "只有 INFO + Stopping! -> 正常");
    check_int(s.exited_ok, 1, "看到正常退出");
    check_str(s.gl_renderer, "NVIDIA GeForce RTX 4060/PCIe/SSE2", "汇总里留下渲染器");
    check_str(s.gl_version, "4.6.0 NVIDIA 566.36", "汇总里留下 GL 版本");
    check(strstr(s.advice, "正常退出") != NULL, "给出人话结论");
    check_int((long)s.kind_counts[SXCL_LOG_GRAPHICS], 4, "图形栈行计数");

    sxcl_log_summarize(win_java, 4, &s);
    check_int(s.conclusion, SXCL_LOG_CONCLUSION_JAVA,
              "Java 版本不符优先于次生的缺类错误(MISSING 是假线索)");
    check_int(s.voted_missing, 1, "次生错误仍然被记下");
    check(strstr(s.advice, "Java") != NULL, "结论里点名 Java");

    sxcl_log_summarize(android_fallback, 6, &s);
    check_int(s.conclusion, SXCL_LOG_CONCLUSION_VULKAN_FALLBACK,
              "安卓切 Vulkan 回退 -> 结论是回退,而不是笼统的图形栈问题");
    check_int(s.voted_vulkan_fallback, 1, "回退被记为一次");
    check_int(s.voted_graphics, 0, "只有 INFO 级的 GL 行不算图形栈故障");
    check_str(s.java_version, "17.0.9-internal", "汇总里留下 Java 版本");
    check_str(s.gl_renderer, "Mali-G68 MC4", "汇总里留下 Mali");

    sxcl_log_summarize(android_broken, 4, &s);
    check_int(s.conclusion, SXCL_LOG_CONCLUSION_MISSING, "缺 .so -> 结论是缺文件");
    check_str(s.missing, "libglfw.so", "汇总里留下缺的库名");

    sxcl_log_summarize(win_graphics, 2, &s);
    check_int(s.conclusion, SXCL_LOG_CONCLUSION_GRAPHICS, "WARN/ERROR 的图形栈行 -> 图形栈结论");

    sxcl_log_summarize(win_account, 1, &s);
    check_int(s.conclusion, SXCL_LOG_CONCLUSION_ACCOUNT, "网络异常 -> 账户网络结论");

    sxcl_log_summarize(win_oom, 2, &s);
    check_int(s.conclusion, SXCL_LOG_CONCLUSION_CRASH, "OOM -> 崩溃结论");
    check(strstr(s.crash_reason, "Java heap space") != NULL, "崩溃原因进了汇总");

    sxcl_log_summarize(NULL, 0, &s);
    check_int(s.conclusion, SXCL_LOG_CONCLUSION_UNKNOWN, "空日志 -> 未知");
    check_int(s.exit_code, -1, "空日志没有退出码");

    sxcl_log_summary_init(&s);
    check_int((long)s.lines, 0, "init 之后是空的");
    (void)sxcl_log_summary_add(&s, kAndroidLines[5]);
    sxcl_log_summary_finish(&s);
    check_int(s.conclusion, SXCL_LOG_CONCLUSION_VULKAN_FALLBACK, "逐行接口也能得到结论");
}

int main(void) {
    test_java_version_text();
    test_java_paths();
    test_java_rank();
    test_args_new_format();
    test_args_old_format();
    test_log_line();
    test_log_summary();
    printf("launch 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
