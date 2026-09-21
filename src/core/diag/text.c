/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#if defined(_MSC_VER)
#  define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "sxcl/text.h"

#include "diag_internal.h"

#include <stdlib.h>
#include <string.h>

/* 见 include/sxcl/text.h 的编码键表。
 *
 * 平台专有部分只在这一个文件里:Windows 用系统 ANSI 代码页解码(中文机器 = 936),
 * POSIX 用 iconv。这不是洁癖问题 —— 自己写一张 GBK 表要两万多个码位,
 * 而"日志里的中文注释"只是要能读,不值得为此背一张表。
 * 安卓(bionic)的 iconv 不保证存在,退回 Latin-1 直通(至少输出仍是合法 UTF-8)。 */
#if defined(_WIN32)
#  include <windows.h>
#elif !defined(__ANDROID__)
#  include <iconv.h>
#endif

/* ────────────────────────── UTF-8 基本件 ────────────────────────── */

/** 解一个 UTF-8 序列(严格):成功把码位写进 *cp 并返回字节数,非法返回 0。 */
static int utf8_decode(const unsigned char *p, size_t avail, unsigned int *cp)
{
    const unsigned char c = p[0];
    if (avail == 0) {
        return 0;
    }
    if (c < 0x80u) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2u && c <= 0xDFu) {
        if (avail < 2 || (p[1] & 0xC0u) != 0x80u) {
            return 0;
        }
        *cp = ((unsigned int)(c & 0x1Fu) << 6) | (unsigned int)(p[1] & 0x3Fu);
        return 2;
    }
    if (c >= 0xE0u && c <= 0xEFu) {
        if (avail < 3 || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u) {
            return 0;
        }
        if (c == 0xE0u && p[1] < 0xA0u) {
            return 0; /* 超长编码 */
        }
        if (c == 0xEDu && p[1] >= 0xA0u) {
            return 0; /* UTF-16 代理区,不是合法 UTF-8 */
        }
        *cp = ((unsigned int)(c & 0x0Fu) << 12) | ((unsigned int)(p[1] & 0x3Fu) << 6) |
              (unsigned int)(p[2] & 0x3Fu);
        return 3;
    }
    if (c >= 0xF0u && c <= 0xF4u) {
        if (avail < 4 || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u ||
            (p[3] & 0xC0u) != 0x80u) {
            return 0;
        }
        if (c == 0xF0u && p[1] < 0x90u) {
            return 0;
        }
        if (c == 0xF4u && p[1] >= 0x90u) {
            return 0; /* 超出 U+10FFFF */
        }
        *cp = ((unsigned int)(c & 0x07u) << 18) | ((unsigned int)(p[1] & 0x3Fu) << 12) |
              ((unsigned int)(p[2] & 0x3Fu) << 6) | (unsigned int)(p[3] & 0x3Fu);
        return 4;
    }
    return 0;
}

int sxcl_text_is_valid_utf8(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0;
    if (p == NULL) {
        return len == 0;
    }
    while (i < len) {
        unsigned int cp = 0;
        const int n = utf8_decode(p + i, len - i, &cp);
        if (n == 0) {
            return 0;
        }
        i += (size_t)n;
    }
    return 1;
}

/** 控制字符可打印化:日志里出现 0x00-0x1F(除 \t \n \r)会让终端/编辑器显示成一团。 */
static unsigned int printable_cp(unsigned int cp)
{
    if (cp == 0x09u || cp == 0x0Au || cp == 0x0Du) {
        return cp;
    }
    if (cp < 0x20u || cp == 0x7Fu) {
        return 0x2Eu; /* '.' */
    }
    return cp;
}

/** 追加一个码位(合法 UTF-8);写不下置 truncated 并返回原长度。 */
static size_t put_cp(char *out, size_t cap, size_t w, unsigned int cp, int *truncated)
{
    unsigned char tmp[4];
    size_t n = 0;
    cp = printable_cp(cp);
    if (cp < 0x80u) {
        tmp[n++] = (unsigned char)cp;
    } else if (cp < 0x800u) {
        tmp[n++] = (unsigned char)(0xC0u | (cp >> 6));
        tmp[n++] = (unsigned char)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        tmp[n++] = (unsigned char)(0xE0u | (cp >> 12));
        tmp[n++] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        tmp[n++] = (unsigned char)(0x80u | (cp & 0x3Fu));
    } else {
        tmp[n++] = (unsigned char)(0xF0u | (cp >> 18));
        tmp[n++] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
        tmp[n++] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        tmp[n++] = (unsigned char)(0x80u | (cp & 0x3Fu));
    }
    if (w + n + 1u > cap) {
        if (truncated != NULL) {
            *truncated = 1;
        }
        return w;
    }
    memcpy(out + w, tmp, n);
    return w + n;
}

/** 已经是 UTF-8(或 ASCII):逐个序列过一遍,非法字节换成 U+FFFD(不是直接丢,读的人要看得见)。 */
static size_t utf8_copy_checked(const void *data, size_t len, char *out, size_t cap, int *truncated)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0;
    size_t w = 0;
    while (i < len) {
        unsigned int cp = 0;
        int n = 0;
        n = utf8_decode(p + i, len - i, &cp);
        if (n == 0) {
            w = put_cp(out, cap, w, 0xFFFDu, truncated);
            ++i;
        } else {
            w = put_cp(out, cap, w, cp, truncated);
            i += (size_t)n;
        }
        if (w + 1u >= cap) {
            /* 连一个字节都放不下了(还要留 NUL):剩下的记成截断 */
            if (i < len && truncated != NULL) {
                *truncated = 1;
            }
            break;
        }
    }
    return w;
}

static size_t latin1_to_utf8(const void *data, size_t len, char *out, size_t cap, int *truncated)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0;
    size_t w = 0;
    for (i = 0; i < len; ++i) {
        if (w + 4u >= cap) {
            if (truncated != NULL) {
                *truncated = 1;
            }
            break;
        }
        w = put_cp(out, cap, w, p[i], truncated);
    }
    return w;
}

static size_t utf16_to_utf8(const void *data, size_t len, char *out, size_t cap, int big_endian,
                            int *truncated)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0;
    size_t w = 0;
    if (len >= 2) {
        const unsigned int bom = big_endian ? ((unsigned int)p[0] << 8 | p[1])
                                            : ((unsigned int)p[1] << 8 | p[0]);
        if (bom == 0xFEFFu) {
            i = 2;
        }
    }
    while (i + 1u < len) {
        unsigned int unit = big_endian ? ((unsigned int)p[i] << 8 | p[i + 1])
                                       : ((unsigned int)p[i + 1] << 8 | p[i]);
        i += 2;
        if (w + 4u >= cap) {
            if (truncated != NULL) {
                *truncated = 1;
            }
            break;
        }
        if (unit >= 0xD800u && unit <= 0xDBFFu && i + 1u < len) {
            const unsigned int low = big_endian ? ((unsigned int)p[i] << 8 | p[i + 1])
                                                : ((unsigned int)p[i + 1] << 8 | p[i]);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                i += 2;
                w = put_cp(out, cap, w, 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u),
                           truncated);
                continue;
            }
        }
        if (unit >= 0xD800u && unit <= 0xDFFFu) {
            w = put_cp(out, cap, w, 0xFFFDu, truncated); /* 落单的代理:half pair */
            continue;
        }
        w = put_cp(out, cap, w, unit, truncated);
    }
    return w;
}

unsigned int sxcl_text_ansi_codepage(void)
{
#if defined(_WIN32)
    const UINT cp = GetACP();
    if (cp == 0u || cp == 65001u) {
        return 936u; /* 机器已经是 UTF-8 代码页:中文日志基本就是 GBK */
    }
    return (unsigned int)cp;
#else
    return 0u; /* POSIX 没有"系统 ANSI 代码页"这个概念,走 iconv(见下) */
#endif
}

#if defined(_WIN32)
static size_t ansi_to_utf8(const void *data, size_t len, char *out, size_t cap, int *truncated)
{
    UINT cp = (UINT)sxcl_text_ansi_codepage();
    wchar_t *wide = NULL;
    int wlen = 0;
    size_t w = 0;
    int u8len = 0;
    unsigned char *utf8 = NULL;
    if (len > 32u * 1024u * 1024u) {
        len = 32u * 1024u * 1024u;
        if (truncated != NULL) {
            *truncated = 1;
        }
    }
    for (;;) {
        wlen = MultiByteToWideChar(cp, 0, (const char *)data, (int)len, NULL, 0);
        if (wlen > 0 || cp == 936u) {
            break;
        }
        cp = 936u; /* GetACP() 给的代码页解不动:按 GBK 再试一次 */
    }
    if (wlen <= 0) {
        return latin1_to_utf8(data, len, out, cap, truncated);
    }
    wide = (wchar_t *)malloc(((size_t)wlen + 1u) * sizeof(wchar_t));
    if (wide == NULL) {
        return latin1_to_utf8(data, len, out, cap, truncated);
    }
    wlen = MultiByteToWideChar(cp, 0, (const char *)data, (int)len, wide, wlen);
    if (wlen <= 0) {
        free(wide);
        return latin1_to_utf8(data, len, out, cap, truncated);
    }
    u8len = WideCharToMultiByte(CP_UTF8, 0, wide, wlen, NULL, 0, NULL, NULL);
    if (u8len <= 0) {
        free(wide);
        return latin1_to_utf8(data, len, out, cap, truncated);
    }
    utf8 = (unsigned char *)malloc((size_t)u8len + 1u);
    if (utf8 == NULL) {
        free(wide);
        return latin1_to_utf8(data, len, out, cap, truncated);
    }
    u8len = WideCharToMultiByte(CP_UTF8, 0, wide, wlen, (char *)utf8, u8len, NULL, NULL);
    free(wide);
    if (u8len <= 0) {
        free(utf8);
        return latin1_to_utf8(data, len, out, cap, truncated);
    }
    w = utf8_copy_checked(utf8, (size_t)u8len, out, cap, truncated);
    free(utf8);
    return w;
}
#elif !defined(__ANDROID__)
static size_t iconv_convert(const char *from, const void *data, size_t len, char *out, size_t cap,
                            int *truncated)
{
    iconv_t cd = iconv_open("UTF-8", from);
    char *in_ptr = (char *)data;
    size_t in_left = len;
    size_t w = 0;
    if (cd == (iconv_t)-1) {
        return (size_t)-1;
    }
    while (in_left > 0 && w + 4u < cap) {
        char *out_ptr = out + w;
        size_t out_left = cap - w - 1u;
        const size_t rc = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
        w = (size_t)(out_ptr - out);
        if (rc == (size_t)-1) {
            w = put_cp(out, cap, w, 0xFFFDu, truncated); /* 坏字节:替换后跳过一个字节继续 */
            if (in_left > 0) {
                ++in_ptr;
                --in_left;
            } else {
                break;
            }
        }
    }
    (void)iconv_close(cd);
    if (in_left > 0 && truncated != NULL) {
        *truncated = 1;
    }
    return w;
}

static size_t ansi_to_utf8(const void *data, size_t len, char *out, size_t cap, int *truncated)
{
    static const char *const kFrom[] = {"GBK", "CP936", "GB18030", NULL};
    size_t i = 0;
    size_t w = 0;
    for (i = 0; kFrom[i] != NULL; ++i) {
        w = iconv_convert(kFrom[i], data, len, out, cap, truncated);
        if (w != (size_t)-1) {
            return w;
        }
    }
    return latin1_to_utf8(data, len, out, cap, truncated);
}
#else
static size_t ansi_to_utf8(const void *data, size_t len, char *out, size_t cap, int *truncated)
{
    /* 安卓:bionic 的 iconv 不保证存在(API 28+),不赌它;Latin-1 直通至少不产生非法字节。 */
    return latin1_to_utf8(data, len, out, cap, truncated);
}
#endif

/* ────────────────────────── 探测 ────────────────────────── */

/** 高字节是否全部符合 GBK 双字节结构(0x81-0xFE 首字节 + 0x40-0xFE 次字节)。 */
static int gbk_looks_valid(const unsigned char *p, size_t len)
{
    size_t i = 0;
    size_t high = 0;
    while (i < len) {
        const unsigned char c = p[i];
        if (c < 0x80u) {
            ++i;
            continue;
        }
        ++high;
        if (c == 0x80u || c == 0xFFu) {
            return 0;
        }
        if (i + 1u >= len) {
            return 0; /* 结尾半个汉字:不要声称是 GBK */
        }
        if (p[i + 1u] < 0x40u || p[i + 1u] == 0x7Fu || p[i + 1u] == 0xFFu) {
            return 0;
        }
        i += 2;
    }
    return high > 0;
}

const char *sxcl_text_detect_encoding(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0;
    size_t high = 0;
    size_t nul = 0;
    size_t even_nul = 0;
    size_t odd_nul = 0;

    if (p == NULL || len == 0) {
        return "empty";
    }
    if (len >= 3u && p[0] == 0xEFu && p[1] == 0xBBu && p[2] == 0xBFu) {
        return "utf-8-bom";
    }
    if (len >= 2u && p[0] == 0xFFu && p[1] == 0xFEu) {
        return "utf-16le";
    }
    if (len >= 2u && p[0] == 0xFEu && p[1] == 0xFFu) {
        return "utf-16be";
    }
    for (i = 0; i < len; ++i) {
        if (p[i] == 0u) {
            ++nul;
            if ((i & 1u) == 0u) {
                ++even_nul;
            } else {
                ++odd_nul;
            }
        } else if (p[i] >= 0x80u) {
            ++high;
        }
    }
    if (nul > 0) {
        /* 无 BOM 的 UTF-16:零字节稳定落在一侧(ASCII 为主的日志就是这个形状) */
        if (len >= 8u && nul * 4u >= len &&
            (odd_nul > even_nul * 4u + 1u || even_nul > odd_nul * 4u + 1u)) {
            return (odd_nul > even_nul) ? "utf-16le" : "utf-16be";
        }
        return "binary";
    }
    if (high == 0) {
        return "ascii";
    }
    if (sxcl_text_is_valid_utf8(data, len)) {
        return "utf-8";
    }
    if (gbk_looks_valid(p, len)) {
        return "gbk";
    }
    return "ansi";
}

size_t sxcl_text_count_lines(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0;
    size_t lines = 0;
    if (p == NULL || len == 0) {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        if (p[i] == 0x0Au) {
            ++lines;
        }
    }
    if (p[len - 1u] != 0x0Au) {
        ++lines; /* 最后一行没有换行符也是"一行" */
    }
    return lines;
}

int sxcl_text_next_line(const char **cursor, char *out, size_t out_cap)
{
    const char *p = NULL;
    size_t n = 0;
    if (cursor == NULL || *cursor == NULL) {
        return 0;
    }
    p = *cursor;
    if (*p == '\0') {
        *cursor = NULL;
        return 0;
    }
    if (out != NULL && out_cap > 0u) {
        while (p[n] != '\0' && p[n] != '\n' && p[n] != '\r' && n + 1u < out_cap) {
            out[n] = p[n];
            ++n;
        }
        out[n] = '\0';
    }
    while (*p != '\0' && *p != '\n') {
        ++p;
    }
    if (*p == '\n') {
        ++p;
    }
    *cursor = (*p == '\0') ? NULL : p;
    return 1;
}

size_t sxcl_diag_text_to_utf8_as(const char *encoding, const void *data, size_t len, char *out,
                                 size_t out_cap, int *truncated)
{
    const char *enc = (encoding != NULL && *encoding != '\0') ? encoding : "utf-8";
    size_t w = 0;
    if (truncated != NULL) {
        *truncated = 0;
    }
    if (out == NULL || out_cap == 0u) {
        return 0;
    }
    out[0] = '\0';
    if (data == NULL || len == 0) {
        return 0;
    }
    if (strcmp(enc, "utf-8-bom") == 0) {
        w = utf8_copy_checked((const unsigned char *)data + 3u, len - 3u, out, out_cap, truncated);
    } else if (strcmp(enc, "utf-8") == 0 || strcmp(enc, "ascii") == 0) {
        w = utf8_copy_checked(data, len, out, out_cap, truncated);
    } else if (strcmp(enc, "utf-16le") == 0) {
        w = utf16_to_utf8(data, len, out, out_cap, 0, truncated);
    } else if (strcmp(enc, "utf-16be") == 0) {
        w = utf16_to_utf8(data, len, out, out_cap, 1, truncated);
    } else if (strcmp(enc, "gbk") == 0 || strcmp(enc, "ansi") == 0) {
        w = ansi_to_utf8(data, len, out, out_cap, truncated);
    } else {
        w = latin1_to_utf8(data, len, out, out_cap, truncated); /* binary:可打印化直通 */
    }
    out[w] = '\0';
    return w;
}

size_t sxcl_text_to_utf8(const void *data, size_t len, char *out, size_t out_cap, int *truncated)
{
    if (data == NULL || len == 0) {
        if (out != NULL && out_cap > 0u) {
            out[0] = '\0';
        }
        if (truncated != NULL) {
            *truncated = 0;
        }
        return 0;
    }
    return sxcl_diag_text_to_utf8_as(sxcl_text_detect_encoding(data, len), data, len, out, out_cap,
                                     truncated);
}
