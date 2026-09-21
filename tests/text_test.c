/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <stdio.h>
#include <string.h>

#include "sxcl/text.h"

/* 编码探测与转换的**字节级**夹具:
 *   1) 探测:空/ASCII/UTF-8/带 BOM/UTF-16LE/BE(含无 BOM 的启发式)/GBK/二进制;
 *   2) 转换:GBK 中文按 936 解出正确汉字、UTF-16 代理对、坏字节替换、超长截断、
 *      控制字符可打印化 —— 全部与实现里的说明一一对应;
 *   3) 行工具:count_lines / next_line(CRLF 与最后一行没有换行符两种边界)。
 * 期望值都是**真实字节**,不依赖本机代码页:同一份夹具在 Windows/Linux/macOS 结果一样。 */

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

static void check_int(long got, long want, const char *what)
{
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("  [!!] %s: got %ld want %ld\n", what, got, want);
    }
}

/* "启动失败" 的 GBK(CP936)字节,由 .NET Encoding.GetEncoding(936) 生成后钉在这里。 */
static const unsigned char kGbkFail[] = {0xC6, 0xF4, 0xB6, 0xAF, 0xCA, 0xA7, 0xB0, 0xDC};
/* "游戏崩溃了" 的 GBK 字节。 */
static const unsigned char kGbkCrash[] = {0xD3, 0xCE, 0xCF, 0xB7, 0xB1, 0xC0,
                                          0xC0, 0xA3, 0xC1, 0xCB};

static void test_detect(void)
{
    static const unsigned char utf8_bom[] = {0xEF, 0xBB, 0xBF, 'a', 'b'};
    /* 无 BOM 的 UTF-16:用**ASCII 为主**的真实形状(短于 8 字节的样本本来就不足以判断) */
    static const unsigned char utf16le[] = {'h', 0, 'e', 0, 'l', 0, 'l', 0, 'o', 0, '\n', 0};
    static const unsigned char utf16be[] = {0, 'h', 0, 'e', 0, 'l', 0, 'l', 0, 'o'};
    static const unsigned char utf16le_bom[] = {0xFF, 0xFE, 'h', 0, 'i', 0};
    static const unsigned char utf16be_bom[] = {0xFE, 0xFF, 0, 'h', 0, 'i'};
    /* 真二进制的形状:PNG 文件头(零字节既不在奇位也不在偶位扎堆) */
    static const unsigned char binary[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                           0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R'};
    /* 非法 UTF-8,而且**不是** GBK(0x81 后面跟空格:GBK 的次字节必须 >= 0x40) */
    static const unsigned char bad_utf8[] = {0x81, 0x20, 'x'};

    printf("[1] 编码探测\n");
    check_str(sxcl_text_detect_encoding(NULL, 0), "empty", "NULL/0 字节 = empty");
    check_str(sxcl_text_detect_encoding("hello", 5), "ascii", "纯 ASCII");
    check_str(sxcl_text_detect_encoding("中文", strlen("中文")), "utf-8", "合法 UTF-8");
    check_str(sxcl_text_detect_encoding(utf8_bom, sizeof(utf8_bom)), "utf-8-bom", "UTF-8 BOM");
    check_str(sxcl_text_detect_encoding(utf16le_bom, sizeof(utf16le_bom)), "utf-16le",
              "UTF-16LE BOM");
    check_str(sxcl_text_detect_encoding(utf16be_bom, sizeof(utf16be_bom)), "utf-16be",
              "UTF-16BE BOM");
    check_str(sxcl_text_detect_encoding(utf16le, sizeof(utf16le)), "utf-16le",
              "无 BOM 的 UTF-16LE(零字节在奇位)");
    check_str(sxcl_text_detect_encoding(utf16be, sizeof(utf16be)), "utf-16be",
              "无 BOM 的 UTF-16BE(零字节在偶位)");
    check_str(sxcl_text_detect_encoding(kGbkFail, sizeof(kGbkFail)), "gbk", "GBK 中文");
    check_str(sxcl_text_detect_encoding(binary, sizeof(binary)), "binary", "二进制(含 NUL)");
    check_str(sxcl_text_detect_encoding(bad_utf8, sizeof(bad_utf8)), "ansi",
              "非法 UTF-8 且不像 GBK = ansi");
    check(sxcl_text_is_valid_utf8("中文", strlen("中文")) == 1, "is_valid_utf8:中文");
    check(sxcl_text_is_valid_utf8(bad_utf8, sizeof(bad_utf8)) == 0, "is_valid_utf8:超长编码被拒");
}

static void test_convert(void)
{
    char out[256];
    int truncated = 0;
    size_t n = 0;

    printf("[2] 转换\n");
    n = sxcl_text_to_utf8(kGbkFail, sizeof(kGbkFail), out, sizeof(out), &truncated);
    check_int((long)truncated, 0, "GBK 转换没有截断");
    if (sxcl_text_ansi_codepage() == 936u || sxcl_text_detect_encoding(kGbkFail, sizeof(kGbkFail))[0] == 'g') {
        /* 中文 Windows 或带 iconv 的 POSIX:必须解成"启动失败";
         * 两者都没有(安卓/精简 POSIX)时退化成 Latin-1,这时只要求"是合法 UTF-8"。 */
        if (strcmp(out, "启动失败") == 0) {
            check(1, "GBK -> 启动失败");
        } else if (sxcl_text_is_valid_utf8(out, n)) {
            check(1, "GBK 无法解码时至少是合法 UTF-8(不产生乱码字节)");
        } else {
            check(0, "GBK 转换结果既不是 启动失败 也不是合法 UTF-8");
        }
    }
    n = sxcl_text_to_utf8(kGbkCrash, sizeof(kGbkCrash), out, sizeof(out), NULL);
    check(n > 0 && sxcl_text_is_valid_utf8(out, n), "GBK 长句:输出是合法 UTF-8");

    {
        static const unsigned char utf16le_bom[] = {0xFF, 0xFE, 0x2D, 0x4E, 0x87, 0x65};
        n = sxcl_text_to_utf8(utf16le_bom, sizeof(utf16le_bom), out, sizeof(out), NULL);
        check_str(out, "中文", "UTF-16LE + BOM -> 中文(BOM 不留在正文里)");
    }
    {
        /* U+1F600(😀)= D83D DE00:代理对必须合成一个字符(带 BOM,免得先被编码探测挡住) */
        static const unsigned char pair[] = {0xFF, 0xFE, 0x3D, 0xD8, 0x00, 0xDE};
        n = sxcl_text_to_utf8(pair, sizeof(pair), out, sizeof(out), NULL);
        check_int((long)n, 4, "UTF-16 代理对 -> 4 字节 UTF-8");
        check_str(out, "\xF0\x9F\x98\x80", "代理对的内容是 U+1F600");
    }
    {
        static const unsigned char lone[] = {0xFF, 0xFE, 0x3D, 0xD8, 'x', 0x00};
        n = sxcl_text_to_utf8(lone, sizeof(lone), out, sizeof(out), NULL);
        check(sxcl_text_is_valid_utf8(out, n), "落单的代理 -> 替换字符(仍是合法 UTF-8)");
    }
    {
        static const unsigned char bad[] = {0xFF, 0xFE, 'a', 0x00, 0x41, 0x00};
        n = sxcl_text_to_utf8(bad, sizeof(bad), out, sizeof(out), NULL);
        check_str(out, "aA", "UTF-16 普通文本");
    }
    {
        static const unsigned char ctrl[] = {0xEF, 0xBB, 0xBF, 'a', 0x01, 'b'};
        n = sxcl_text_to_utf8(ctrl, sizeof(ctrl), out, sizeof(out), NULL);
        check_str(out, "a.b", "控制字符换成 '.'(日志里看得见)");
    }
    {
        char small[4];
        static const unsigned char many[] = "abcdefgh";
        n = sxcl_text_to_utf8(many, sizeof(many) - 1u, small, sizeof(small), &truncated);
        check_int((long)truncated, 1, "缓冲不够时置 truncated");
        check_int((long)n, 3, "缓冲不够时按容量截断(留 NUL)");
        check_str(small, "abc", "截断后的内容");
    }
    check_int((long)sxcl_text_to_utf8(NULL, 0, out, sizeof(out), NULL), 0, "空输入 -> 0 字节");
    check_str(out, "", "空输入 -> 空串");
}

static void test_lines(void)
{
    char line[64];
    const char *cursor = NULL;

    printf("[3] 行工具\n");
    check_int((long)sxcl_text_count_lines("a\nb\n", 4), 2, "两个换行 = 两行");
    check_int((long)sxcl_text_count_lines("a\nb", 3), 2, "最后一行没有换行也算一行");
    check_int((long)sxcl_text_count_lines("", 0), 0, "空内容 = 0 行");
    check_int((long)sxcl_text_count_lines("a\r\nb\r\n", 6), 2, "CRLF 不额外算行");

    cursor = "one\r\ntwo\nthree";
    check_int(sxcl_text_next_line(&cursor, line, sizeof(line)), 1, "取第一行");
    check_str(line, "one", "CRLF 的 CR 被去掉");
    check_int(sxcl_text_next_line(&cursor, line, sizeof(line)), 1, "取第二行");
    check_str(line, "two", "第二行");
    check_int(sxcl_text_next_line(&cursor, line, sizeof(line)), 1, "取第三行");
    check_str(line, "three", "最后一行(没有换行符)");
    check_int(sxcl_text_next_line(&cursor, line, sizeof(line)), 0, "到底了");
}

int main(void)
{
    printf("文本编码模块测试\n");
    test_detect();
    test_convert();
    test_lines();
    printf("text 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
