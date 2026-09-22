/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 *
 * processors[] 重放的单测（docs/22 的 A3）。
 *
 * 夹具自己造：仓库里不放二进制夹具，所以这里带一个"只写 stored 条目"的最小 ZIP 写入器
 * （处理器 jar 必须是一份**里面有 MANIFEST.MF 的真 zip**，Main-Class 那一步才测得到；
 *  安装器 jar 同理 —— data 里的裸值要从它里面取出来）。
 * 起进程那一步用**假 run 回调**：它记录命令行、按需写出产物，于是"幂等跳过 / 产物校验 /
 * Java 重试 / 取消 / dry-run"全都能在不起 JVM 的情况下断言。
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/hash.h"
#include "sxcl/processor.h"
#include "sxcl/zip.h"

static int g_pass = 0, g_fail = 0;
static void check(int ok, const char *what) {
    if (ok) { ++g_pass; } else { ++g_fail; printf("  [!!] %s\n", what); }
}
static void check_int(long got, long want, const char *what) {
    if (got == want) { ++g_pass; }
    else { ++g_fail; printf("  [!!] %s: got %ld want %ld\n", what, got, want); }
}
static void check_str(const char *got, const char *want, const char *what) {
    if (got && want && strcmp(got, want) == 0) { ++g_pass; }
    else { ++g_fail; printf("  [!!] %s: got '%s' want '%s'\n", what, got ? got : "(null)", want ? want : "(null)"); }
}
static void check_has(const char *got, const char *needle, const char *what) {
    if (got && needle && strstr(got, needle) != NULL) { ++g_pass; }
    else { ++g_fail; printf("  [!!] %s: '%s' 里没有 '%s'\n", what, got ? got : "(null)", needle); }
}

/* ── 夹具 ── */

#define TMP_ROOT "_processor_test_tmp"
#define GAME_DIR TMP_ROOT "/game"

static int write_text(const char *path, const char *text) {
    if (sxcl_fs_mkdirs_for_file(path) != 0) { return -1; }
    FILE *fh = fopen(path, "wb");
    if (!fh) { return -1; }
    const size_t n = strlen(text);
    const size_t got = fwrite(text, 1, n, fh);
    fclose(fh);
    return got == n ? 0 : -1;
}

static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) { c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1); }
        crc_table[i] = c;
    }
}
static uint32_t crc32_of(const char *text) {
    uint32_t c = 0xFFFFFFFFu;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        c = crc_table[(c ^ *p) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}
static void put16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}
static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/** 写一个只含 stored 条目的最小 zip（够 sxcl_zip_open 读）。 */
static int zip_write(const char *path, const char **names, const char **texts, size_t count) {
    size_t total = 22;
    for (size_t i = 0; i < count; ++i) {
        total += 30 + strlen(names[i]) + strlen(texts[i]);
        total += 46 + strlen(names[i]);
    }
    unsigned char *buf = (unsigned char *)calloc(1, total);
    uint32_t *local_off = (uint32_t *)calloc(count ? count : 1, sizeof(uint32_t));
    if (!buf || !local_off) { free(buf); free(local_off); return -1; }
    size_t off = 0;
    for (size_t i = 0; i < count; ++i) {
        const size_t nl = strlen(names[i]);
        const size_t dl = strlen(texts[i]);
        local_off[i] = (uint32_t)off;
        put32(buf + off, 0x04034b50u); off += 4;
        put16(buf + off, 20); off += 2;
        put16(buf + off, 0); off += 2;
        put16(buf + off, 0); off += 2;
        put16(buf + off, 0); off += 2;
        put16(buf + off, 0x21); off += 2;
        put32(buf + off, crc32_of(texts[i])); off += 4;
        put32(buf + off, (uint32_t)dl); off += 4;
        put32(buf + off, (uint32_t)dl); off += 4;
        put16(buf + off, (unsigned)nl); off += 2;
        put16(buf + off, 0); off += 2;
        memcpy(buf + off, names[i], nl); off += nl;
        memcpy(buf + off, texts[i], dl); off += dl;
    }
    const size_t cd_start = off;
    for (size_t i = 0; i < count; ++i) {
        const size_t nl = strlen(names[i]);
        const size_t dl = strlen(texts[i]);
        put32(buf + off, 0x02014b50u); off += 4;
        put16(buf + off, 20); off += 2;
        put16(buf + off, 20); off += 2;
        put16(buf + off, 0); off += 2;
        put16(buf + off, 0); off += 2;
        put16(buf + off, 0); off += 2;
        put16(buf + off, 0x21); off += 2;
        put32(buf + off, crc32_of(texts[i])); off += 4;
        put32(buf + off, (uint32_t)dl); off += 4;
        put32(buf + off, (uint32_t)dl); off += 4;
        put16(buf + off, (unsigned)nl); off += 2;
        put16(buf + off, 0); off += 2;
        put16(buf + off, 0); off += 2;
        put16(buf + off, 0); off += 2;
        put16(buf + off, 0); off += 2;
        put32(buf + off, 0); off += 4;
        put32(buf + off, local_off[i]); off += 4;
        memcpy(buf + off, names[i], nl); off += nl;
    }
    const size_t cd_size = off - cd_start;
    put32(buf + off, 0x06054b50u); off += 4;
    put16(buf + off, 0); off += 2;
    put16(buf + off, 0); off += 2;
    put16(buf + off, (unsigned)count); off += 2;
    put16(buf + off, (unsigned)count); off += 2;
    put32(buf + off, (uint32_t)cd_size); off += 4;
    put32(buf + off, (uint32_t)cd_start); off += 4;
    put16(buf + off, 0); off += 2;
    int rc = -1;
    if (off == total && sxcl_fs_mkdirs_for_file(path) == 0) {
        FILE *fh = fopen(path, "wb");
        if (fh) {
            rc = fwrite(buf, 1, off, fh) == off ? 0 : -1;
            fclose(fh);
        }
    }
    free(buf);
    free(local_off);
    return rc;
}

static void make_jar(const char *rel, const char *main_class) {
    char path[512];
    snprintf(path, sizeof(path), "%s/libraries/%s", GAME_DIR, rel);
    char manifest[256];
    snprintf(manifest, sizeof(manifest),
             "Manifest-Version: 1.0\r\nMain-Class: %s\r\n\r\n", main_class);
    const char *names[1] = { "META-INF/MANIFEST.MF" };
    const char *texts[1] = { manifest };
    if (zip_write(path, names, texts, 1) != 0) {
        printf("  [!!] 夹具写不出来: %s\n", path);
        ++g_fail;
    }
}

static int file_text(const char *path, char *buf, size_t cap) {
    FILE *fh = fopen(path, "rb");
    if (!fh) { return -1; }
    const size_t got = fread(buf, 1, cap - 1, fh);
    buf[got] = '\0';
    fclose(fh);
    return 0;
}

/* ── 夹具里的 profile ── */

#define LIB_JAR_TOOLS   "net/minecraftforge/installertools/1.3.0/installertools-1.3.0.jar"
#define LIB_JAR_SPLIT   "net/minecraftforge/jarsplitter/1.1.4/jarsplitter-1.1.4.jar"
#define LIB_ASM         "org/ow2/asm/asm/9.3/asm-9.3.jar"
#define LIB_SPECIAL     "net/md-5/SpecialSource/1.11.0/SpecialSource-1.11.0.jar"

/* 形状照 Forge 1.20.1 的真 install_profile.json 缩：data 是按 side 分档的对象 +
 * 一条老格式的裸字符串；processors 里有一条 server-only、一条无产物、一条带产物
 * （哈希是运行期算出来的）、一条 DOWNLOAD_MOJMAPS。 */
static const char *kProfileFmt =
    "{\"spec\":1,\"data\":{"
    "\"MAPPINGS\":{\"client\":\"[de.oceanlabs.mcp:mcp_config:1.20.1-20230612.114412:mappings@txt]\"},"
    "\"MOJMAPS\":{\"client\":\"[net.minecraft:client:1.20.1-20230612.114412:mappings@txt]\"},"
    "\"MC_SLIM\":{\"client\":\"[net.minecraft:client:1.20.1-20230612.114412:slim]\"},"
    "\"MC_SLIM_SHA\":{\"client\":\"'%s'\"},"
    "\"PLAIN\":\"'value'\""
    "},\"processors\":["
    "{\"sides\":[\"server\"],\"jar\":\"net.minecraftforge:installertools:1.3.0\","
    "\"args\":[\"--task\",\"EXTRACT_FILES\"]},"
    "{\"jar\":\"net.minecraftforge:installertools:1.3.0\","
    "\"classpath\":[\"net.md-5:SpecialSource:1.11.0\"],"
    "\"args\":[\"--task\",\"MCP_DATA\",\"--input\","
    "\"[de.oceanlabs.mcp:mcp_config:1.20.1-20230612.114412@zip]\",\"--output\",\"{MAPPINGS}\"]},"
    "{\"jar\":\"net.minecraftforge:jarsplitter:1.1.4\","
    "\"classpath\":[\"org.ow2.asm:asm:9.3\"],"
    "\"args\":[\"--input\",\"{MINECRAFT_JAR}\",\"--slim\",\"{MC_SLIM}\",\"--apply\",\"{INSTALLER}\"],"
    "\"outputs\":{\"{MC_SLIM}\":\"{MC_SLIM_SHA}\"}},"
    "{\"jar\":\"net.minecraftforge:installertools:1.3.0\","
    "\"args\":[\"--task\",\"DOWNLOAD_MOJMAPS\",\"--version\",\"1.20.1\",\"--side\",\"{SIDE}\","
    "\"--output\",\"{MOJMAPS}\"]}"
    "]}";

static const char *kTakeProfile =
    "{\"data\":{\"PATCH\":{\"client\":\"/data/client.lzma\"}},"
    "\"processors\":[{\"jar\":\"net.minecraftforge:installertools:1.3.0\","
    "\"args\":[\"--apply\",\"{PATCH}\"]}]}";

/* ── 假 run 回调 ── */

/* 下载替代实现的记录：**嵌在 fake_run 里**，因为 sxcl_processor_ctx 的三个回调共用一个
 * userdata（生产里就是 install_ctx），各给各的 ud 会读到错的结构体。 */
typedef struct moj_ud {
    int calls;
    int ok;
    char version[64];
    char output[512];
} moj_ud;

typedef struct fake_run {
    int calls;
    int argv_not_terminated;     /* 1 = 收到过"没按 NULL 结尾"的 argv（process.h 的契约） */
    const char *slim_body;       /* 非空 = 把这段内容写进 --slim 指的文件 */
    const char *fail_java;       /* 等于这个 program 就直接报"起不来" */
    char programs[8][256];
    char argv0[8][64];           /* 第一个参数（应该是 -cp） */
    char argv1[8][256];          /* Main-Class */
    char first_arg[8][256];      /* Main-Class 之后的第一个参数 */
    char cp_seen[8][1024];
    char patch_seen[512];        /* take_file 那条路径 */
    moj_ud moj;
} fake_run;

static int run_impl(fake_run *f, const char *program, const char *const *argv, size_t argc,
                    const char *work_dir, int timeout_ms, int *exit_code, char *err,
                    size_t err_len) {
    (void)work_dir;
    (void)timeout_ms;
    if (argv[argc] != NULL) {
        /* 真机上踩过：argv 忘了 NULL 结尾，引擎读到栈上的垃圾，把上一条处理器的参数
         * 当成这一条的（jarsplitter 收到 MERGE_MAPPING 的 --classes 直接报错）。 */
        f->argv_not_terminated = 1;
    }
    if (f->fail_java && program && strcmp(program, f->fail_java) == 0) {
        snprintf(err, err_len, "这个 java 起不来");
        if (exit_code) { *exit_code = -1; }
        return 1;
    }
    if (f->calls < 8) {
        snprintf(f->programs[f->calls], sizeof(f->programs[0]), "%s", program ? program : "");
        if (argc > 0) { snprintf(f->argv0[f->calls], sizeof(f->argv0[0]), "%s", argv[0]); }
        if (argc > 2) { snprintf(f->argv1[f->calls], sizeof(f->argv1[0]), "%s", argv[2]); }
        if (argc > 3) { snprintf(f->first_arg[f->calls], sizeof(f->first_arg[0]), "%s", argv[3]); }
        if (argc > 1) { snprintf(f->cp_seen[f->calls], sizeof(f->cp_seen[0]), "%s", argv[1]); }
    }
    ++f->calls;
    for (size_t i = 0; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--slim") == 0 && f->slim_body) {
            if (write_text(argv[i + 1], f->slim_body) != 0) {
                snprintf(err, err_len, "产物写不进去");
                if (exit_code) { *exit_code = 1; }
                return 0;
            }
        }
        if (strcmp(argv[i], "--apply") == 0) {
            snprintf(f->patch_seen, sizeof(f->patch_seen), "%s", argv[i + 1]);
        }
    }
    if (exit_code) { *exit_code = 0; }
    return 0;
}

static int fake_run_cb(void *ud, const char *program, const char *const *argv, size_t argc,
                       const char *work_dir, int timeout_ms, int *exit_code, char *err,
                       size_t err_len) {
    return run_impl((fake_run *)ud, program, argv, argc, work_dir, timeout_ms, exit_code, err, err_len);
}

/* data 里裸值的取文件实现（生产里在 installer.c，这里直接开夹具 zip）。 */
typedef struct take_ud {
    const char *zip_path;
    const char *temp_dir;   /* 带结尾斜杠 */
} take_ud;

/* 取文件那一条用例要"run 与 take_file 共用一个 ud"，于是把两者打进一个结构体。 */
typedef struct take_env {
    fake_run run;
    take_ud take;
} take_env;

static int env_run_cb(void *ud, const char *program, const char *const *argv, size_t argc,
                      const char *work_dir, int timeout_ms, int *exit_code, char *err,
                      size_t err_len) {
    take_env *e = (take_env *)ud;
    return run_impl(&e->run, program, argv, argc, work_dir, timeout_ms, exit_code, err, err_len);
}

static int env_take_file(void *ud, const char *entry, char *out, size_t cap) {
    take_ud *t = &((take_env *)ud)->take;
    sxcl_zip *zip = sxcl_zip_open(t->zip_path);
    if (!zip) { return -1; }
    const char *lookup = (entry[0] == '/') ? entry + 1 : entry;   /* zip 里没有前导斜杠 */
    char name[256];
    snprintf(name, sizeof(name), "%s", lookup);
    for (size_t i = 0; name[i]; ++i) { if (name[i] == '/') { name[i] = '_'; } }
    char dest[512];
    snprintf(dest, sizeof(dest), "%s%s", t->temp_dir, name);
    const int rc = sxcl_zip_extract_file(zip, lookup, dest);
    sxcl_zip_close(zip);
    if (rc != 0) { return -1; }
    snprintf(out, cap, "%s", dest);
    return 0;
}

/* 下载替代实现（FCL 的 patchDownloadMojangMappingsTask） */
static int fake_mojmaps(void *ud, const char *version, const char *output, char *err, size_t err_len) {
    moj_ud *m = &((fake_run *)ud)->moj;
    ++m->calls;
    snprintf(m->version, sizeof(m->version), "%s", version ? version : "");
    snprintf(m->output, sizeof(m->output), "%s", output ? output : "");
    if (!m->ok) {
        snprintf(err, err_len, "镜像也不通");
        return 1;
    }
    return write_text(output, "MOJMAPS") == 0 ? 0 : 1;
}

static int g_cancel = 0;
static int fake_cancelled(void *ud) { (void)ud; return g_cancel; }

/* ── 1) 访问器 ── */

static void test_accessors(const sxcl_json *profile) {
    char buf[256];
    check_int((long)sxcl_processor_count(profile), 4, "processors 条数");
    check_int(sxcl_processor_wants(profile, 0, "client"), 0, "sides=[server] 的不在 client 侧跑");
    check_int(sxcl_processor_wants(profile, 0, "server"), 1, "  同一条在 server 侧要跑");
    check_int(sxcl_processor_wants(profile, 1, "client"), 1, "没有 sides 键的两侧都跑");

    check_int(sxcl_processor_jar(profile, 2, buf, sizeof(buf)), 0, "取 processor[2].jar");
    check_str(buf, "net.minecraftforge:jarsplitter:1.1.4", "  坐标");
    check_int((long)sxcl_processor_classpath_count(profile, 2), 1, "classpath 条数");
    check_int(sxcl_processor_classpath_at(profile, 2, 0, buf, sizeof(buf)), 0, "取 classpath[0]");
    check_str(buf, "org.ow2.asm:asm:9.3", "  坐标");
    check_int((long)sxcl_processor_classpath_count(profile, 0), 0, "server 那条没有 classpath");
    check_int((long)sxcl_processor_arg_count(profile, 2), 6, "args 条数");
    check_str(sxcl_processor_arg_at(profile, 2, 0), "--input", "  args[0]");
    check_int((long)sxcl_processor_output_count(profile, 2), 1, "outputs 条数");
    check_str(sxcl_processor_output_key_at(profile, 2, 0), "{MC_SLIM}", "  outputs 的键原文");
    check_str(sxcl_processor_output_value_at(profile, 2, 0), "{MC_SLIM_SHA}", "  outputs 的值原文");
    check_int((long)sxcl_processor_output_count(profile, 1), 0, "没有 outputs 的返回 0");
}

/* ── 2) data → 变量表 + literal 求值 ── */

static void test_vars(const sxcl_json *profile) {
    sxcl_processor_vars *vars = (sxcl_processor_vars *)calloc(1, sizeof(*vars));
    char err[SXCL_PROCESSOR_ERR_MAX];
    char out[512];
    err[0] = '\0';
    check_int(sxcl_processor_vars_from_data(profile, GAME_DIR, NULL, NULL, vars, err, sizeof(err)),
              0, "data 求值");
    check_str(sxcl_processor_vars_get(vars, "MAPPINGS"),
              GAME_DIR "/libraries/de/oceanlabs/mcp/mcp_config/1.20.1-20230612.114412/"
                       "mcp_config-1.20.1-20230612.114412-mappings.txt",
              "  [坐标] 展开成 libraries 下的绝对路径");
    check_str(sxcl_processor_vars_get(vars, "PLAIN"), "value", "  带引号的字面量（老格式就是这么写的）");
    check_str(sxcl_processor_vars_get(vars, "MC_SLIM"),
              GAME_DIR "/libraries/net/minecraft/client/1.20.1-20230612.114412/"
                       "client-1.20.1-20230612.114412-slim.jar",
              "  带分类器的坐标");
    check(sxcl_processor_vars_get(vars, "MC_SLIM_SHA") != NULL &&
              strlen(sxcl_processor_vars_get(vars, "MC_SLIM_SHA")) == 40,
          "  '字面量' 去引号后是 40 位哈希");

    check_int(sxcl_processor_eval("'hi'", vars, GAME_DIR, NULL, NULL, out, sizeof(out), err,
                                  sizeof(err)),
              0, "eval 'hi'");
    check_str(out, "hi", "  去引号");
    check_int(sxcl_processor_eval("X{PLAIN}Y", vars, GAME_DIR, NULL, NULL, out, sizeof(out), err,
                                  sizeof(err)),
              0, "eval 内联 {键}");
    check_str(out, "XvalueY", "  替换进去");
    check_int(sxcl_processor_eval("a'b c'd", vars, GAME_DIR, NULL, NULL, out, sizeof(out), err,
                                  sizeof(err)),
              0, "eval 内联 '字面量'");
    check_str(out, "ab cd", "  引号里的空格保住");
    check_int(sxcl_processor_eval("{NOPE}", vars, GAME_DIR, NULL, NULL, out, sizeof(out), err,
                                  sizeof(err)),
              -3, "eval 缺键要报错");
    check_has(err, "没有这个键", "  错误里点名缺哪个键");
    check_int(sxcl_processor_eval("{NOPE", vars, GAME_DIR, NULL, NULL, out, sizeof(out), err,
                                  sizeof(err)),
              -3, "eval 括号没闭合要报错");
    check_has(err, "没有闭合", "  错误里说清楚");
    check_int(sxcl_processor_eval("", vars, GAME_DIR, NULL, NULL, out, sizeof(out), err,
                                  sizeof(err)),
              0, "空串不报错");
    check_str(out, "", "  展开成空");
    free(vars);
}

/* ── 3) 重放 ── */

static void test_run(const sxcl_json *profile, const char *installer_jar) {
    fake_run f;
    sxcl_processor_stats stats;
    char err[SXCL_PROCESSOR_ERR_MAX];
    char slim[512];
    char slim_body[64];
    char split_jar[512];

    snprintf(slim, sizeof(slim), "%s/libraries/%s", GAME_DIR,
             "net/minecraft/client/1.20.1-20230612.114412/"
             "client-1.20.1-20230612.114412-slim.jar");
    snprintf(slim_body, sizeof(slim_body), "SLIM-BYTES");
    snprintf(split_jar, sizeof(split_jar), "%s/libraries/%s", GAME_DIR, LIB_JAR_SPLIT);

    sxcl_processor_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.game_dir = GAME_DIR;
    ctx.installer_jar = installer_jar;
    ctx.minecraft_jar = GAME_DIR "/versions/1.20.1/1.20.1.jar";
    ctx.minecraft_version = "1.20.1";
    ctx.temp_dir = TMP_ROOT "/tmp";
    ctx.java_path = "C:/fake/java.exe";
    ctx.run = fake_run_cb;
    ctx.is_cancelled = fake_cancelled;

    memset(&f, 0, sizeof(f));
    f.slim_body = slim_body;
    ctx.ud = &f;
    (void)sxcl_fs_remove(slim);
    err[0] = '\0';
    /* t1：产物不在 -> 真跑。server 那条不算数，client 三条（MCP_DATA 无产物 / jarsplitter /
     * DOWNLOAD_MOJMAPS）都要跑。 */
    check_int(sxcl_processors_run(profile, "client", &ctx, &stats, err, sizeof(err)), 0,
              "重放跑完（t1）");
    check_int(stats.considered, 3, "  按 side 过滤后 3 条");
    check_int(stats.ran, 3, "  跑了 3 条");
    check_int(stats.skipped, 0, "  没有跳过的");
    check_int(f.calls, 3, "  假 run 被调了 3 次");
    check_str(f.programs[0], "C:/fake/java.exe", "  用调用方给的 java");
    check_str(f.argv0[0], "-cp", "  第一个参数是 -cp");
    check_str(f.argv1[0], "net.minecraftforge.installertools.Main", "  Main-Class 来自 jar 清单");
    check_str(f.first_arg[0], "--task", "  处理器自己的参数跟在后面");
    check_has(f.cp_seen[1], "asm-9.3.jar", "  classpath 里有依赖 jar");
    check_has(f.cp_seen[1], "jarsplitter-1.1.4.jar", "  处理器自己的 jar 也排在 classpath 里");
    check(sxcl_fs_exists(slim), "  产物落盘了");
    check_int(f.argv_not_terminated, 0, "  argv 按 process.h 的契约 NULL 结尾");

    /* t2：产物齐了 -> 幂等跳过（FCL 同款）；没有产物的那条照样跑。 */
    memset(&f, 0, sizeof(f));
    f.slim_body = slim_body;
    f.moj.ok = 1;
    ctx.download_mappings = fake_mojmaps;
    ctx.ud = &f;
    err[0] = '\0';
    check_int(sxcl_processors_run(profile, "client", &ctx, &stats, err, sizeof(err)), 0,
              "重放跑完（t2）");
    check_int(stats.skipped, 1, "  jarsplitter 那条被跳过");
    check_int(stats.downloaded, 1, "  DOWNLOAD_MOJMAPS 走了下载替代实现");
    check_int(stats.ran, 1, "  只剩没有产物的那条真跑");
    check_int(f.moj.calls, 1, "  下载回调被调了 1 次");
    check_str(f.moj.version, "1.20.1", "  它拿到的版本号");
    check_has(f.moj.output, "client-1.20.1-20230612.114412-mappings.txt", "  它拿到的输出路径");
    ctx.download_mappings = NULL;

    /* t3：产物内容不对 -> 报错 + 把那件不合格的产物删掉。 */
    memset(&f, 0, sizeof(f));
    f.slim_body = "WRONG-BYTES";
    ctx.ud = &f;
    err[0] = '\0';
    (void)sxcl_fs_remove(slim);
    check_int(sxcl_processors_run(profile, "client", &ctx, &stats, err, sizeof(err)), 1,
              "产物哈希不对要失败（t3）");
    check_has(err, "产物不对", "  错误里说清是产物校验");
    check(!sxcl_fs_exists(slim), "  不合格的产物被删掉了");

    /* t4：产物齐了但工具 jar 不在 -> 仍然跳过（FCL 的顺序：先看产物）。 */
    (void)write_text(slim, slim_body);
    (void)sxcl_fs_remove(split_jar);
    memset(&f, 0, sizeof(f));
    f.slim_body = slim_body;
    ctx.ud = &f;
    err[0] = '\0';
    check_int(sxcl_processors_run(profile, "client", &ctx, &stats, err, sizeof(err)), 0,
              "工具不在但产物齐 -> 跳过（t4）");
    check_int(stats.skipped, 1, "  确实跳过了");

    /* t5：产物不在且工具 jar 也不在 -> 点名报"处理器 jar 不在"。 */
    (void)sxcl_fs_remove(slim);
    memset(&f, 0, sizeof(f));
    f.slim_body = slim_body;
    ctx.ud = &f;
    err[0] = '\0';
    check_int(sxcl_processors_run(profile, "client", &ctx, &stats, err, sizeof(err)), 1,
              "工具不在且要真跑 -> 失败（t5）");
    check_has(err, "处理器 jar 不在", "  错误里点名");
    make_jar(LIB_JAR_SPLIT, "net.minecraftforge.jarsplitter.Main");

    /* t6：Java 候选：首选起不来就换下一个（FCL 换 Java 版本，我们换路径）。 */
    memset(&f, 0, sizeof(f));
    f.slim_body = slim_body;
    f.fail_java = "C:/fake/java.exe";
    ctx.ud = &f;
    err[0] = '\0';
    (void)sxcl_fs_remove(slim);
    check_int(sxcl_processors_run(profile, "client", &ctx, &stats, err, sizeof(err)), 0,
              "换一个 Java 之后跑成功（t6）");
    check(f.calls >= 1, "  确实重试过");
    check(f.calls >= 1 && strcmp(f.programs[0], "C:/fake/java.exe") != 0,
          "  重试用的不是失败的那个");

    /* t7：取消。 */
    g_cancel = 1;
    memset(&f, 0, sizeof(f));
    ctx.ud = &f;
    err[0] = '\0';
    check_int(sxcl_processors_run(profile, "client", &ctx, &stats, err, sizeof(err)), 2,
              "取消要如实返回 2（t7）");
    check_int(f.calls, 0, "  一条都没跑");
    g_cancel = 0;

    /* t8：dry-run 只算不跑。 */
    memset(&f, 0, sizeof(f));
    ctx.ud = &f;
    ctx.dry_run = 1;
    err[0] = '\0';
    check_int(sxcl_processors_run(profile, "client", &ctx, &stats, err, sizeof(err)), 0,
              "dry-run 不跑也成功（t8）");
    check_int(f.calls, 0, "  没起进程");
    ctx.dry_run = 0;

    /* t9：没有 processors 的老格式（1.12 及以前）不该报错。 */
    {
        const char *legacy = "{\"install\":{\"libraries\":[]}}";
        char perr[SXCL_PROCESSOR_ERR_MAX];
        perr[0] = '\0';
        sxcl_json *doc = sxcl_json_parse(legacy, strlen(legacy), perr, sizeof(perr));
        check(doc != NULL, "老格式 fixture 解析");
        if (doc) {
            check_int(sxcl_processors_run(doc, "client", &ctx, &stats, err, sizeof(err)), 0,
                      "没有 processors 直接成功（t9）");
            sxcl_json_free(doc);
        }
    }
    (void)sxcl_fs_remove(slim);
}

/* ── 4) data 里的裸值（take_file） ── */

static void test_take_file(const char *installer_jar) {
    char err[SXCL_PROCESSOR_ERR_MAX];
    err[0] = '\0';
    sxcl_json *profile = sxcl_json_parse(kTakeProfile, strlen(kTakeProfile), err, sizeof(err));
    check(profile != NULL, "take_file 夹具 profile 解析");
    if (!profile) { return; }

    take_env env;
    memset(&env, 0, sizeof(env));
    env.take.zip_path = installer_jar;
    env.take.temp_dir = TMP_ROOT "/tmp/";

    sxcl_processor_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.game_dir = GAME_DIR;
    ctx.installer_jar = installer_jar;
    ctx.temp_dir = TMP_ROOT "/tmp";
    ctx.java_path = "C:/fake/java.exe";
    ctx.run = env_run_cb;
    ctx.take_file = env_take_file;
    ctx.ud = &env;

    check_int(sxcl_processors_run(profile, "client", &ctx, NULL, err, sizeof(err)), 0,
              "带裸值的 data 跑通");
    check_str(env.run.patch_seen, TMP_ROOT "/tmp/data_client.lzma", "  裸值取成了临时文件");
    char body[64];
    check_int(file_text(env.run.patch_seen, body, sizeof(body)), 0, "  临时文件真在");
    check_str(body, "LZMA-BYTES", "  内容与安装器 zip 里的条目一致");

    /* 没有 take_file 时，同一条必须**如实报错**（而不是塞一个 zip 里的相对路径给处理器）。 */
    memset(&env.run, 0, sizeof(env.run));
    ctx.take_file = NULL;
    ctx.ud = &env;
    err[0] = '\0';
    check_int(sxcl_processors_run(profile, "client", &ctx, NULL, err, sizeof(err)), 1,
              "没有 take_file 时裸值要报错");
    check_has(err, "take_file", "  错误里说清是缺取文件的实现");
    sxcl_json_free(profile);
}

int main(void) {
    crc_init();
    (void)sxcl_fs_remove_tree(TMP_ROOT);
    if (sxcl_fs_mkdirs(GAME_DIR "/libraries") != 0 || sxcl_fs_mkdirs(TMP_ROOT "/tmp") != 0) {
        printf("建不了测试目录\n");
        return 1;
    }

    /* 夹具：两个处理器 jar（真 zip，带 MANIFEST.MF）、两个 classpath 依赖、安装器 zip、原版 jar */
    make_jar(LIB_JAR_TOOLS, "net.minecraftforge.installertools.Main");
    make_jar(LIB_JAR_SPLIT, "net.minecraftforge.jarsplitter.Main");
    (void)write_text(GAME_DIR "/libraries/" LIB_ASM, "asm");
    (void)write_text(GAME_DIR "/libraries/" LIB_SPECIAL, "special");
    (void)write_text(GAME_DIR "/versions/1.20.1/1.20.1.jar", "VANILLA");

    const char *inst_names[1] = { "data/client.lzma" };
    const char *inst_texts[1] = { "LZMA-BYTES" };
    char installer_jar[512];
    snprintf(installer_jar, sizeof(installer_jar), "%s/src/forge-installer.jar", TMP_ROOT);
    check_int(zip_write(installer_jar, inst_names, inst_texts, 1), 0, "造安装器 zip 夹具");
    {
        sxcl_zip *probe = sxcl_zip_open(installer_jar);
        check(probe != NULL, "  我们自己写的 zip 能被 sxcl_zip_open 打开");
        if (probe) { sxcl_zip_close(probe); }
        probe = sxcl_zip_open(installer_jar);
        if (probe) {
            check_int((long)sxcl_zip_count(probe), 1, "  条目数");
            check_int(sxcl_zip_find(probe, "data/client.lzma"), 0, "  按名字找得到条目");
            const int xrc = sxcl_zip_extract_file(probe, "data/client.lzma", TMP_ROOT "/zip_probe.lzma");
            check_int(xrc, 0, "  条目能解出来");
            char probe_body[64];
            if (xrc == 0 && file_text(TMP_ROOT "/zip_probe.lzma", probe_body, sizeof(probe_body)) == 0) {
                check_str(probe_body, "LZMA-BYTES", "  解出来的内容对");
            }
            sxcl_zip_close(probe);
        }
        probe = sxcl_zip_open(GAME_DIR "/libraries/" LIB_JAR_SPLIT);
        check(probe != NULL, "  处理器 jar 夹具也能打开");
        if (probe) { sxcl_zip_close(probe); }
    }

    char slim_body[64];
    snprintf(slim_body, sizeof(slim_body), "SLIM-BYTES");
    char slim_sha[64];
    check_int(sxcl_hash_digest(SXCL_HASH_SHA1, slim_body, strlen(slim_body), slim_sha, sizeof(slim_sha)),
              0, "算夹具产物的 SHA-1");

    char profile_text[4096];
    snprintf(profile_text, sizeof(profile_text), kProfileFmt, slim_sha);
    char err[SXCL_PROCESSOR_ERR_MAX];
    err[0] = '\0';
    sxcl_json *profile = sxcl_json_parse(profile_text, strlen(profile_text), err, sizeof(err));
    check(profile != NULL, "夹具 profile 解析");
    if (!profile) { printf("  %s\n", err); return 1; }

    test_accessors(profile);
    test_vars(profile);
    test_run(profile, installer_jar);
    test_take_file(installer_jar);

    sxcl_json_free(profile);
    (void)sxcl_fs_remove_tree(TMP_ROOT);
    printf("processor 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
