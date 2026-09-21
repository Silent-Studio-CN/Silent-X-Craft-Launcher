/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* tar_test.c - tar 解包器的夹具测试(ustar / prefix / pax / GNU 长名 / 链接 / 负例)。
 *
 * 夹具用 Python 的 tarfile 生成(POSIX/PAX/GNU 三种格式各一份),签入仓库:
 *   links.tar           普通文件 + 目录 + 符号链接(带可执行位)
 *   pax-long.tar        pax 扩展头给的长路径(160 字节)+ 紧跟一个短名条目
 *   ustar-prefix.tar    ustar 的 prefix 字段 + name 拼接
 *   gnu-long.tar        GNU 'L' 长名
 *   unsafe-dotdot.tar   "../escape.txt" -> 必须拒
 *   unsafe-abs.tar      "/etc/passwd"   -> 必须拒
 *   unsupported-type.tar 设备节点('3')  -> 必须报 unsupported
 * 解包目录在构建目录下的 _tar_tmp(仓库里不留产物)。
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#else
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"
#include "sxcl/tar.h"

#if !defined(_WIN32)
#  include <sys/stat.h>
#  include <unistd.h>
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

static void check_int(long got, long want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %ld want %ld\n", what, got, want);
    }
}

static char g_dir[1024];
static char g_tmp[1024];

static int fixture_path(char *out, size_t out_len, const char *name)
{
    if (g_dir[0] == '\0') {
        const char *env = getenv("SXCL_TAR_FIXTURES");
        snprintf(g_dir, sizeof(g_dir), "%s", env != NULL ? env : "tests/fixtures/tar");
    }
    return snprintf(out, out_len, "%s/%s", g_dir, name) < (int)out_len ? 0 : -1;
}

static unsigned char *read_file(const char *name, size_t *len_out)
{
    char path[1200];
    FILE *fp;
    long size;
    unsigned char *buf;
    if (fixture_path(path, sizeof(path), name) != 0) {
        return NULL;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        printf("  [!!] 打不开夹具 %s\n", path);
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    buf = (unsigned char *)malloc((size_t)(size > 0 ? size : 0) + 1u);
    if (buf == NULL) {
        (void)fclose(fp);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        (void)fclose(fp);
        return NULL;
    }
    (void)fclose(fp);
    buf[size > 0 ? size : 0] = 0;
    *len_out = (size_t)(size > 0 ? size : 0);
    return buf;
}

/** 在 xz 与 tar 两个夹具目录里找一个文件(源树里两边都签入了内容)。 */
static int any_fixture_path(char *out, size_t out_len, const char *name)
{
    const char *envs[2];
    int i;
    envs[0] = getenv("SXCL_TAR_FIXTURES");
    envs[1] = getenv("SXCL_XZ_FIXTURES");
    for (i = 0; i < 2; ++i) {
        if (envs[i] != NULL && envs[i][0] != '\0') {
            FILE *fp;
            snprintf(out, out_len, "%s/%s", envs[i], name);
            fp = fopen(out, "rb");
            if (fp != NULL) {
                (void)fclose(fp);
                return 0;
            }
        }
    }
    return -1;
}

static void tmp_reset(const char *leaf)
{
    snprintf(g_tmp, sizeof(g_tmp), "_tar_tmp/%s", leaf);
    (void)sxcl_fs_remove_tree("_tar_tmp");
    (void)sxcl_fs_mkdirs(g_tmp);
}

static void join(char *out, size_t out_len, const char *leaf)
{
    snprintf(out, out_len, "%s/%s", g_tmp, leaf);
}

static int file_equals(const char *leaf, const char *want, size_t want_len)
{
    char path[1200];
    FILE *fp;
    static unsigned char buf[4096];
    size_t n;
    join(path, sizeof(path), leaf);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf), fp);
    (void)fclose(fp);
    return n == want_len && memcmp(buf, want, want_len) == 0;
}

static int is_executable(const char *leaf)
{
#if defined(_WIN32)
    (void)leaf;
    return 1; /* Windows 没有可执行位:这一项按"不适用"算通过 */
#else
    char path[1200];
    struct stat st;
    join(path, sizeof(path), leaf);
    if (stat(path, &st) != 0) {
        return 0;
    }
    return (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0 ? 1 : 0;
#endif
}

static int is_symlink_to(const char *leaf, const char *target)
{
#if defined(_WIN32)
    (void)leaf;
    (void)target;
    return 1; /* Windows 上建链接要特权:按"不适用"算通过 */
#else
    char path[1200];
    char buf[512];
    ssize_t n;
    join(path, sizeof(path), leaf);
    n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) {
        return 0;
    }
    buf[n] = '\0';
    return strcmp(buf, target) == 0 ? 1 : 0;
#endif
}

static int exists(const char *leaf)
{
    char path[1200];
    join(path, sizeof(path), leaf);
    return sxcl_fs_exists(path);
}

/* 计数回调 */
typedef struct counter {
    int entries;
    int last_type;
    char last_path[512];
} counter;

static int on_entry(void *ud, const sxcl_tar_entry *e)
{
    counter *c = (counter *)ud;
    ++c->entries;
    c->last_type = (int)e->type;
    snprintf(c->last_path, sizeof(c->last_path), "%s", e->path);
    return 0;
}

static int cancel_probe(void *ud)
{
    int *hits = (int *)ud;
    ++*hits;
    return (*hits > 2) ? 1 : 0;
}

/* ── 用例 ── */

static void test_links(void)
{
    size_t len = 0;
    unsigned char *raw = read_file("links.tar", &len);
    sxcl_tar_opts opts;
    sxcl_tar *tar;
    counter c;
    char err[256];
    int rc;

    printf("== ustar:文件/目录/符号链接/可执行位(links.tar)\n");
    check(raw != NULL, "读到 links.tar");
    if (raw == NULL) {
        return;
    }
    tmp_reset("links");
    memset(&opts, 0, sizeof(opts));
    opts.dest_dir = g_tmp;
    memset(&c, 0, sizeof(c));
    opts.on_entry = on_entry;
    opts.ud = &c;
    tar = sxcl_tar_open(&opts, err, sizeof(err));
    check(tar != NULL, "打开解包器");
    if (tar != NULL) {
        rc = sxcl_tar_feed(tar, raw, len);
        check_int(rc, SXCL_TAR_OK, "feed");
        rc = sxcl_tar_finish(tar, err, sizeof(err));
        check_int(rc, SXCL_TAR_OK, "finish");
        check_int((long)sxcl_tar_files_written(tar), 2, "文件数");
        check_int((long)sxcl_tar_dirs_created(tar), 1, "目录数");
        check_int((long)sxcl_tar_links_created(tar), 1, "链接数");
        check_int((long)sxcl_tar_bytes_out(tar), 7 + 10, "字节数");
        check_int(sxcl_tar_is_done(tar), 1, "看到归档结束标记");
        sxcl_tar_close(tar);
    }
    check_int(c.entries, 4, "on_entry 报了 4 次");
    check(file_equals("lib/libjli.so", "\x7f" "ELFJLI", 7), "lib/libjli.so 内容");
    check(file_equals("bin/java", "#!/bin/sh\n", 10), "bin/java 内容");
    check(is_executable("bin/java"), "bin/java 有可执行位");
    check(is_symlink_to("lib/libjli-link.so", "libjli.so"), "软链接指向 libjli.so");
    free(raw);
}

static void test_pax_and_gnu(void)
{
    static const char *longname =
        "modules/java.base/very-long-component-name-very-long-component-name-"
        "very-long-component-name-very-long-component-name-very-long-component-name-"
        "module-info.class";
    size_t len = 0;
    unsigned char *raw;
    sxcl_tar_opts opts;
    sxcl_tar *tar;
    char err[256];
    char leaf[600];

    printf("== pax 长名 / ustar prefix / GNU 长名\n");
    raw = read_file("pax-long.tar", &len);
    check(raw != NULL, "读到 pax-long.tar");
    if (raw != NULL) {
        tmp_reset("pax");
        memset(&opts, 0, sizeof(opts));
        opts.dest_dir = g_tmp;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        check(tar != NULL, "打开(pax)");
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, raw, len), SXCL_TAR_OK, "feed(pax)");
            check_int(sxcl_tar_finish(tar, err, sizeof(err)), SXCL_TAR_OK, "finish(pax)");
            sxcl_tar_close(tar);
        }
        {
            /* 夹具里的内容是 b"PAXDATA" * 100 = 700 字节 */
            static char paxwant[701];
            int rep;
            for (rep = 0; rep < 100; ++rep) {
                memcpy(paxwant + rep * 7, "PAXDATA", 7);
            }
            paxwant[700] = '\0';
            check(file_equals(longname, paxwant, 700), "pax 长路径落盘");
        }
        /* 关键:**pax 的名字不能粘到后面那个短名条目上** */
        check(file_equals("short.txt", "hello\n", 6), "紧跟的 short.txt 没被长名污染");
        free(raw);
    }

    raw = read_file("ustar-prefix.tar", &len);
    check(raw != NULL, "读到 ustar-prefix.tar");
    if (raw != NULL) {
        tmp_reset("prefix");
        memset(&opts, 0, sizeof(opts));
        opts.dest_dir = g_tmp;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, raw, len), SXCL_TAR_OK, "feed(prefix)");
            check_int(sxcl_tar_finish(tar, err, sizeof(err)), SXCL_TAR_OK, "finish(prefix)");
            sxcl_tar_close(tar);
        }
        snprintf(leaf, sizeof(leaf), "release-notes/%s/README.txt", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        check(file_equals(leaf, "prefix-split\n", 13), "ustar prefix+name 拼出来的路径");
        free(raw);
    }

    raw = read_file("gnu-long.tar", &len);
    check(raw != NULL, "读到 gnu-long.tar");
    if (raw != NULL) {
        tmp_reset("gnu");
        memset(&opts, 0, sizeof(opts));
        opts.dest_dir = g_tmp;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, raw, len), SXCL_TAR_OK, "feed(gnu)");
            check_int(sxcl_tar_finish(tar, err, sizeof(err)), SXCL_TAR_OK, "finish(gnu)");
            sxcl_tar_close(tar);
        }
        check(file_equals(longname, "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA"
                                     "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA"
                                     "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA"
                                     "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA"
                                     "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA"
                                     "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA"
                                     "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA"
                                     "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA"
                                     "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA"
                                     "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA" "GNUDATA",
                         350),
              "GNU 'L' 长路径落盘");
        free(raw);
    }
}

static void test_negative(void)
{
    size_t len = 0;
    unsigned char *raw;
    sxcl_tar_opts opts;
    sxcl_tar *tar;
    char err[256];

    printf("== 负例:不安全路径 / 不支持的条目 / 截断 / 总量上限\n");
    memset(&opts, 0, sizeof(opts));

    /* ../escape.txt */
    raw = read_file("unsafe-dotdot.tar", &len);
    check(raw != NULL, "读到 unsafe-dotdot.tar");
    if (raw != NULL) {
        tmp_reset("unsafe1");
        opts.dest_dir = g_tmp;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        check(tar != NULL, "打开(unsafe1)");
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, raw, len), SXCL_TAR_ERR_UNSAFE, "../ 必须拒");
            (void)sxcl_tar_finish(tar, err, sizeof(err)); /* feed 不返回人话:去 finish 里取 */
            check(strstr(err, "..") != NULL, "错误信息点名 ..");
            sxcl_tar_close(tar);
        }
        check(!sxcl_fs_exists("_tar_tmp/escape.txt"), "确实没写到解包目录之外");
        free(raw);
    }
    /* /etc/passwd */
    raw = read_file("unsafe-abs.tar", &len);
    check(raw != NULL, "读到 unsafe-abs.tar");
    if (raw != NULL) {
        tmp_reset("unsafe2");
        opts.dest_dir = g_tmp;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, raw, len), SXCL_TAR_ERR_UNSAFE, "绝对路径必须拒");
            sxcl_tar_close(tar);
        }
        free(raw);
    }
    /* 设备节点 */
    raw = read_file("unsupported-type.tar", &len);
    check(raw != NULL, "读到 unsupported-type.tar");
    if (raw != NULL) {
        tmp_reset("unsup");
        opts.dest_dir = g_tmp;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, raw, len), SXCL_TAR_ERR_UNSUPPORTED, "设备节点必须拒");
            sxcl_tar_close(tar);
        }
        free(raw);
    }
    /* 截断:喂到 bin/java 的数据中间(头 4 块 + 5 字节)就断掉 */
    raw = read_file("links.tar", &len);
    check(raw != NULL, "读到 links.tar(截断用)");
    if (raw != NULL) {
        tmp_reset("trunc");
        opts.dest_dir = g_tmp;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, raw, 512u * 5u + 5u), SXCL_TAR_OK, "喂到一半");
            check_int(sxcl_tar_finish(tar, err, sizeof(err)), SXCL_TAR_ERR_DATA,
                      "截断必须报 data");
            sxcl_tar_close(tar);
        }
        /* close 必须把"正在写的半个文件"删掉 */
        check(!sxcl_fs_exists("_tar_tmp/bin/java"), "截断不留半个文件");
        free(raw);
    }
    /* 总量上限 */
    raw = read_file("links.tar", &len);
    if (raw != NULL) {
        tmp_reset("limit");
        opts.dest_dir = g_tmp;
        opts.max_total = 3;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, raw, len), SXCL_TAR_ERR_LIMIT, "max_total 必须拦住");
            sxcl_tar_close(tar);
        }
        free(raw);
    }
    /* 取消 */
    raw = read_file("links.tar", &len);
    if (raw != NULL) {
        int hits = 0;
        tmp_reset("cancel");
        opts.dest_dir = g_tmp;
        opts.max_total = 0;
        opts.is_cancelled = cancel_probe;
        opts.ud = &hits;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, raw, len), SXCL_TAR_ERR_ABORT, "取消 -> abort");
            sxcl_tar_close(tar);
        }
        opts.is_cancelled = NULL;
        opts.ud = NULL;
        free(raw);
    }
    check(sxcl_tar_open(NULL, err, sizeof(err)) == NULL, "opts 为空 -> NULL");
    memset(&opts, 0, sizeof(opts));
    check(sxcl_tar_open(&opts, err, sizeof(err)) == NULL, "dest_dir 没给 -> NULL");
}

/* ── 边界夹具:坏校验和 / 空归档 / 超长路径(最容易越界的那几条) ── */

static void test_boundaries(void)
{
    size_t len = 0;
    unsigned char *raw;
    sxcl_tar_opts opts;
    sxcl_tar *tar;
    char err[256];

    printf("== 边界:坏头校验和 / 空归档 / 超长路径\n");
    memset(&opts, 0, sizeof(opts));

    /* 头校验和字段被改过:必须报 data,而且要**在写任何文件之前**就报 */
    raw = read_file("badchecksum.tar", &len);
    check(raw != NULL, "读到 badchecksum.tar");
    if (raw != NULL) {
        tmp_reset("badsum");
        opts.dest_dir = g_tmp;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, raw, len), SXCL_TAR_ERR_DATA, "坏校验和必须报 data");
            (void)sxcl_tar_finish(tar, err, sizeof(err));
            check(strstr(err, "校验和") != NULL, "错误信息点到校验和");
            sxcl_tar_close(tar);
        }
        check(!sxcl_fs_exists("_tar_tmp/lib/libjli.so"), "坏归档一个文件都没落盘");
        free(raw);
    }

    /* 0 字节的归档:截断,不是崩溃 */
    {
        static const unsigned char kNothing[1] = {0};
        tmp_reset("empty");
        opts.dest_dir = g_tmp;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, kNothing, 0), SXCL_TAR_OK, "喂 0 字节不算错");
            check_int(sxcl_tar_finish(tar, err, sizeof(err)), SXCL_TAR_OK,
                      "0 字节归档 = 空归档(没有条目也没有结束标记,按宽容口径收)");
            check_int((long)sxcl_tar_bytes_out(tar), 0, "解出来 0 字节");
            sxcl_tar_close(tar);
        }
    }
    raw = read_file("empty.tar", &len);
    check(raw != NULL && len == 0, "empty.tar 是 0 字节");
    free(raw);

    /* 超长路径(pax 给的 1500+ 字符):必须拒(ERR_UNSAFE),不许溢出/写到别处 */
    raw = read_file("longname.tar", &len);
    check(raw != NULL, "读到 longname.tar");
    if (raw != NULL) {
        tmp_reset("longname");
        opts.dest_dir = g_tmp;
        tar = sxcl_tar_open(&opts, err, sizeof(err));
        if (tar != NULL) {
            check_int(sxcl_tar_feed(tar, raw, len), SXCL_TAR_ERR_UNSAFE, "超长路径必须拒");
            (void)sxcl_tar_finish(tar, err, sizeof(err));
            check(strstr(err, "太长") != NULL, "错误信息点到「太长」");
            sxcl_tar_close(tar);
        }
        check(!sxcl_fs_exists("_tar_tmp/deep"), "超长路径没落盘");
        free(raw);
    }
}

static void test_via_xz(void)
{
    char archive[1200];
    sxcl_tar_opts opts;
    char err[256];
    char leaf[1200];
    int rc;

    printf("== 一步到位:tar.xz -> 目录(走 xz 解码器)\n");
    if (any_fixture_path(archive, sizeof(archive), "jre-tiny-crc64.xz") != 0) {
        check(0, "拼夹具路径");
        return;
    }
    tmp_reset("viaxz");
    memset(&opts, 0, sizeof(opts));
    opts.dest_dir = g_tmp;
    rc = sxcl_tar_extract_file(archive, &opts, err, sizeof(err));
    check_int(rc, SXCL_TAR_OK, "extract_file(tar.xz)");
    if (rc != SXCL_TAR_OK) {
        printf("      err=%s\n", err);
    }
    {
        /* jre-tiny.tar 里的 bin/java = "#!/bin/sh\n" + bytes(range(256))*4 = 1034 字节 */
        static unsigned char want[1034];
        size_t i;
        memcpy(want, "#!/bin/sh\n", 10);
        for (i = 0; i < 1024; ++i) {
            want[10 + i] = (unsigned char)(i & 0xFF);
        }
        check(file_equals("bin/java", (const char *)want, sizeof(want)), "bin/java 内容(经 xz)");
    }
    (void)snprintf(leaf, sizeof(leaf), "%s", "lib/server/libjvm.so");
    check(exists("lib/server/libjvm.so"), "lib/server/libjvm.so 落盘");
    check(exists("conf/security/java.security"), "conf/security/java.security 落盘");
    check(file_equals("release", "JAVA_VERSION=\"17.0.9\"\nMODULES=\"java.base\"\n", 42),
          "release 内容");
    check(is_executable("lib/server/libjvm.so"), "libjvm.so 有可执行位");
    check(!exists("../escape.txt"), "没有越界文件");
    /* 错误的归档:不存在 -> IO */
    rc = sxcl_tar_extract_file("_tar_tmp/does-not-exist.tar.xz", &opts, err, sizeof(err));
    check_int(rc, SXCL_TAR_ERR_IO, "打不开的归档 -> IO");
    /* 拿 tar 当 xz:必须报 DATA */
    {
        char tarpath[1200];
        tmp_reset("notxz");
        if (fixture_path(tarpath, sizeof(tarpath), "links.tar") == 0) {
            rc = sxcl_tar_extract_file(tarpath, &opts, err, sizeof(err));
            check_int(rc, SXCL_TAR_ERR_DATA, "拿裸 tar 当 tar.xz -> DATA");
        }
    }
}

static void test_names(void)
{
    printf("== 返回码名字\n");
    check(strcmp(sxcl_tar_code_name(SXCL_TAR_OK), "ok") == 0, "ok");
    check(strcmp(sxcl_tar_code_name(SXCL_TAR_ERR_UNSAFE), "unsafe") == 0, "unsafe");
    check(strcmp(sxcl_tar_code_name(SXCL_TAR_ERR_UNSUPPORTED), "unsupported") == 0,
          "unsupported");
    check(strcmp(sxcl_tar_code_name(-999), "?") == 0, "未知");
}

int main(void)
{
    test_links();
    test_pax_and_gnu();
    test_negative();
    test_boundaries();
    test_via_xz();
    test_names();
    (void)sxcl_fs_remove_tree("_tar_tmp");
    printf("\ntar_test: 通过 %d,失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
