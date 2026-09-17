/* SXCL-C ZIP 读取实现 —— 语义见 include/sxcl/zip.h。
 *
 * 结构:
 *   1) zip_fopen_utf8():UTF-8 路径打开(Win32 走 _wfopen_s,不受代码页影响);
 *   2) EOCD 回扫 + 中央目录解析:一次读进内存建条目表(名字统一转 UTF-8),
 *      之后条目查询是纯内存操作;解压时才 seek 到本地头按需读压缩数据;
 *   3) 解压:stored 直通;deflate 交给 sxcl_inflate 流式解,全程只占
 *      一块 32 KiB 读缓冲 + 解压器自己的 32 KiB 窗口;
 *   4) extract_file 先写 <dest>.tmp 再原子改名(与 settings/hashcache 同一套路),
 *      写入用 fs.h 的 sxcl_file_*(Windows 下按 UTF-8 -> UTF-16 打开路径)。
 *
 * 只用 stdio + 本工程的 fs/inflate,无第三方依赖;
 * MSVC /W4 /WX 与 gcc -Wall -Wextra -Wpedantic -Werror 双零警告。
 */
#include "sxcl/zip.h"

#include "sxcl/fs.h"
#include "sxcl/inflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>   /* 仅为了用 _wfopen_s 打开 UTF-8 路径 */
#endif

/* ── 常量 ── */

#define ZIP_SIG_LOCAL    0x04034B50u   /* PK\x03\x04 本地文件头 */
#define ZIP_SIG_CENTRAL  0x02014B50u   /* PK\x01\x02 中央目录条目 */
#define ZIP_SIG_EOCD     0x06054B50u   /* PK\x05\x06 中央目录结束记录 */

#define ZIP_EOCD_SIZE    22u                     /* EOCD 定长部分 */
#define ZIP_MAX_COMMENT  65535u                  /* EOCD 注释上限 */
#define ZIP_TAIL_MAX     (ZIP_EOCD_SIZE + ZIP_MAX_COMMENT)
#define ZIP_LOCAL_SIZE   30u                     /* 本地头定长部分 */
#define ZIP_CENTRAL_SIZE 46u                     /* 中央目录条目定长部分 */
#define ZIP_READ_BUF     ((size_t)32768u)        /* 解压时的压缩数据读缓冲 */
#define ZIP_CD_MAX       ((int64_t)256 * 1024 * 1024)  /* 中央目录大小上限(防呆) */

#define ZIP_FLAG_ENCRYPTED 0x0001u   /* 通用位 0:加密 */
#define ZIP_FLAG_UTF8      0x0800u   /* 通用位 11:名字是 UTF-8 */

#define ZIP_U32_SENTINEL 0xFFFFFFFFu

/* CP437 上半区(0x80..0xFF)对应的 Unicode 码点;ZIP 没置 UTF-8 位时用它转码。 */
static const unsigned short zip_cp437[128] = {
    0x00C7u, 0x00FCu, 0x00E9u, 0x00E2u, 0x00E4u, 0x00E0u, 0x00E5u, 0x00E7u,
    0x00EAu, 0x00EBu, 0x00E8u, 0x00EFu, 0x00EEu, 0x00ECu, 0x00C4u, 0x00C5u,
    0x00C9u, 0x00E6u, 0x00C6u, 0x00F4u, 0x00F6u, 0x00F2u, 0x00FBu, 0x00F9u,
    0x00FFu, 0x00D6u, 0x00DCu, 0x00A2u, 0x00A3u, 0x00A5u, 0x20A7u, 0x0192u,
    0x00E1u, 0x00EDu, 0x00F3u, 0x00FAu, 0x00F1u, 0x00D1u, 0x00AAu, 0x00BAu,
    0x00BFu, 0x2310u, 0x00ACu, 0x00BDu, 0x00BCu, 0x00A1u, 0x00ABu, 0x00BBu,
    0x2591u, 0x2592u, 0x2593u, 0x2502u, 0x2524u, 0x2561u, 0x2562u, 0x2556u,
    0x2555u, 0x2563u, 0x2551u, 0x2557u, 0x255Du, 0x255Cu, 0x255Bu, 0x2510u,
    0x2514u, 0x2534u, 0x252Cu, 0x251Cu, 0x2500u, 0x253Cu, 0x255Eu, 0x255Fu,
    0x255Au, 0x2554u, 0x2569u, 0x2566u, 0x2560u, 0x2550u, 0x256Cu, 0x2567u,
    0x2568u, 0x2564u, 0x2565u, 0x2559u, 0x2558u, 0x2552u, 0x2553u, 0x256Bu,
    0x256Au, 0x2518u, 0x250Cu, 0x2588u, 0x2584u, 0x258Cu, 0x2590u, 0x2580u,
    0x03B1u, 0x00DFu, 0x0393u, 0x03C0u, 0x03A3u, 0x03C3u, 0x00B5u, 0x03C4u,
    0x03A6u, 0x0398u, 0x03A9u, 0x03B4u, 0x221Eu, 0x03C6u, 0x03B5u, 0x2229u,
    0x2261u, 0x00B1u, 0x2265u, 0x2264u, 0x2320u, 0x2321u, 0x00F7u, 0x2248u,
    0x00B0u, 0x2219u, 0x00B7u, 0x221Au, 0x207Fu, 0x00B2u, 0x25A0u, 0x00A0u
};

/* ── 数据结构 ── */

typedef struct zip_entry {
    char    *name;          /* 指向 name_pool,UTF-8,NUL 结尾 */
    int64_t  comp_size;     /* 中央目录里的压缩后大小 */
    int64_t  uncomp_size;   /* 解压后大小 */
    int64_t  local_off;     /* 本地文件头偏移 */
    uint32_t crc;           /* 只记录(本模块不做 CRC 校验) */
    int      method;        /* 0 = stored,8 = deflate */
    int      unusable;      /* 1 = 加密 / ZIP64 / 名字异常:解压返回 -3 */
} zip_entry;

struct sxcl_zip {
    FILE      *fp;
    int64_t    file_size;
    zip_entry *entries;
    size_t     count;
    char      *name_pool;
};

/* ── 小工具 ── */

static uint16_t zip_le16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t zip_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* UTF-8 路径打开(和 src/core/dl/hashcache.c 同一套写法)。 */
static FILE *zip_fopen_utf8(const char *path, const char *mode)
{
#if defined(_WIN32)
    const int need = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *wide;
    wchar_t wmode[8];
    FILE *f = NULL;
    size_t i;
    size_t n;

    if (need <= 0) {
        return NULL;
    }
    wide = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (wide == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, need) == need) {
        n = strlen(mode);
        for (i = 0u; i < n && i + 1u < (sizeof wmode / sizeof wmode[0]); ++i) {
            wmode[i] = (wchar_t)(unsigned char)mode[i];
        }
        wmode[i] = L'\0';
        if (_wfopen_s(&f, wide, wmode) != 0) {   /* _wfopen 会触发 C4996,/WX 下必须用 _s 版 */
            f = NULL;
        }
    }
    free(wide);
    return f;
#else
    return fopen(path, mode);
#endif
}

static int zip_read_at(FILE *fp, int64_t off, void *buf, size_t len)
{
    if (fseek(fp, (long)off, SEEK_SET) != 0) {
        return -1;
    }
    if (len == 0u) {
        return 0;
    }
    return (fread(buf, 1u, len, fp) == len) ? 0 : -1;
}

/* ── 名字解码(UTF-8 原样 / CP437 -> UTF-8) ── */

/* 码点写成 UTF-8,返回写入字节数(表只到 U+FFFF,最多 3 字节)。 */
static size_t zip_utf8_put(char *dst, unsigned cp)
{
    if (cp < 0x80u) {
        dst[0] = (char)cp;
        return 1u;
    }
    if (cp < 0x800u) {
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2u;
    }
    dst[0] = (char)(0xE0u | (cp >> 12));
    dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    dst[2] = (char)(0x80u | (cp & 0x3Fu));
    return 3u;
}

/* 名字转 UTF-8,写进 dst 并补 NUL;返回写入字节数(不含 NUL),名字里有 NUL 返回 (size_t)-1。 */
static size_t zip_name_to_utf8(const unsigned char *raw, size_t len, int is_utf8, char *dst)
{
    size_t i;
    size_t out = 0u;

    for (i = 0u; i < len; ++i) {
        const unsigned char c = raw[i];
        if (c == 0u) {
            dst[out] = '\0';
            return (size_t)-1;
        }
        if (is_utf8 || c < 0x80u) {
            dst[out] = (char)c;
            ++out;
        } else {
            out += zip_utf8_put(dst + out, zip_cp437[c - 0x80u]);
        }
    }
    dst[out] = '\0';
    return out;
}

/* ── 打开 / 解析 ── */

/* 从尾部回扫 EOCD:签名 + 注释长度必须正好补到文件末尾。 */
static int zip_find_eocd(FILE *fp, int64_t file_size, int64_t *eocd_off)
{
    unsigned char *tail;
    size_t tail_len;
    size_t k;

    if (file_size < (int64_t)ZIP_EOCD_SIZE) {
        return -1;
    }
    tail_len = (file_size > (int64_t)ZIP_TAIL_MAX) ? (size_t)ZIP_TAIL_MAX : (size_t)file_size;
    tail = (unsigned char *)malloc(tail_len);
    if (tail == NULL) {
        return -1;
    }
    if (zip_read_at(fp, file_size - (int64_t)tail_len, tail, tail_len) != 0) {
        free(tail);
        return -1;
    }

    k = tail_len - (size_t)ZIP_EOCD_SIZE;
    for (;;) {
        if (zip_le32(tail + k) == ZIP_SIG_EOCD) {
            const unsigned comment_len = (unsigned)zip_le16(tail + k + 20u);
            if (k + (size_t)ZIP_EOCD_SIZE + (size_t)comment_len == tail_len) {
                *eocd_off = file_size - (int64_t)tail_len + (int64_t)k;
                free(tail);
                return 0;
            }
        }
        if (k == 0u) {
            break;
        }
        k -= 1u;
    }
    free(tail);
    return -1;
}

sxcl_zip *sxcl_zip_open(const char *path)
{
    unsigned char eocd[ZIP_EOCD_SIZE];
    sxcl_zip *zip;
    int64_t eocd_off = 0;
    int64_t cd_off;
    int64_t cd_size;
    uint64_t total;
    unsigned char *cd;
    size_t pool_used = 0u;
    size_t off = 0u;
    size_t n;

    if (path == NULL) {
        return NULL;
    }

    zip = (sxcl_zip *)malloc(sizeof(sxcl_zip));
    if (zip == NULL) {
        return NULL;
    }
    zip->fp = NULL;
    zip->file_size = 0;
    zip->entries = NULL;
    zip->count = 0u;
    zip->name_pool = NULL;

    zip->fp = zip_fopen_utf8(path, "rb");
    if (zip->fp == NULL) {
        free(zip);
        return NULL;
    }
    if (fseek(zip->fp, 0, SEEK_END) != 0) {
        sxcl_zip_close(zip);
        return NULL;
    }
    zip->file_size = (int64_t)ftell(zip->fp);
    if (zip->file_size <= 0 || zip_find_eocd(zip->fp, zip->file_size, &eocd_off) != 0) {
        sxcl_zip_close(zip);   /* 尾部没有 EOCD:不是 zip */
        return NULL;
    }
    if (zip_read_at(zip->fp, eocd_off, eocd, ZIP_EOCD_SIZE) != 0) {
        sxcl_zip_close(zip);
        return NULL;
    }

    total = (uint64_t)zip_le16(eocd + 10u);       /* 本盘条目数 */
    cd_size = (int64_t)zip_le32(eocd + 12u);
    cd_off = (int64_t)zip_le32(eocd + 16u);

    /* 整包 ZIP64(条目数/大小/偏移饱和):按头文件说明直接判为打不开。
     * 单条 ZIP64(大小字段 0xFFFFFFFF)在下面标成 unusable,解压时返回 -3。 */
    if (total == 0xFFFFu || cd_size == (int64_t)ZIP_U32_SENTINEL ||
        cd_off == (int64_t)ZIP_U32_SENTINEL) {
        sxcl_zip_close(zip);
        return NULL;
    }
    if (cd_size < 0 || cd_off < 0 || cd_size > ZIP_CD_MAX || cd_off + cd_size > zip->file_size) {
        sxcl_zip_close(zip);
        return NULL;
    }

    cd = (unsigned char *)malloc((size_t)cd_size + 1u);
    if (cd == NULL) {
        sxcl_zip_close(zip);
        return NULL;
    }
    if (zip_read_at(zip->fp, cd_off, cd, (size_t)cd_size) != 0) {
        free(cd);
        sxcl_zip_close(zip);
        return NULL;
    }

    /* 名字池:CP437 最坏 1 字节变 3 字节,再加每个名字一个 NUL */
    zip->name_pool = (char *)malloc((size_t)cd_size * 3u + (size_t)total + 1u);
    zip->entries = (zip_entry *)calloc((size_t)total + 1u, sizeof(zip_entry));
    if (zip->name_pool == NULL || zip->entries == NULL) {
        free(cd);
        sxcl_zip_close(zip);
        return NULL;
    }

    for (n = 0u; n < (size_t)total; ++n) {
        zip_entry *e = &zip->entries[n];
        unsigned flags;
        unsigned name_len;
        unsigned extra_len;
        unsigned comment_len;
        size_t need;

        if (off + (size_t)ZIP_CENTRAL_SIZE > (size_t)cd_size) {
            break;   /* 中央目录被截断 */
        }
        if (zip_le32(cd + off) != ZIP_SIG_CENTRAL) {
            break;   /* 条目签名不对:停在这里,保留前面认出来的 */
        }
        flags = (unsigned)zip_le16(cd + off + 8u);
        e->method = (int)zip_le16(cd + off + 10u);
        e->crc = zip_le32(cd + off + 16u);
        e->comp_size = (int64_t)zip_le32(cd + off + 20u);
        e->uncomp_size = (int64_t)zip_le32(cd + off + 24u);
        name_len = (unsigned)zip_le16(cd + off + 28u);
        extra_len = (unsigned)zip_le16(cd + off + 30u);
        comment_len = (unsigned)zip_le16(cd + off + 32u);
        e->local_off = (int64_t)zip_le32(cd + off + 42u);

        need = (size_t)ZIP_CENTRAL_SIZE + (size_t)name_len + (size_t)extra_len + (size_t)comment_len;
        if (off + need > (size_t)cd_size) {
            break;   /* 这一条越界:中央目录不完整 */
        }

        e->unusable = 0;
        if ((flags & ZIP_FLAG_ENCRYPTED) != 0u ||
            (uint32_t)e->comp_size == ZIP_U32_SENTINEL ||
            (uint32_t)e->uncomp_size == ZIP_U32_SENTINEL ||
            (uint32_t)e->local_off == ZIP_U32_SENTINEL) {
            e->unusable = 1;   /* 加密 / ZIP64:列得出来但不给解 */
        }

        e->name = zip->name_pool + pool_used;
        {
            const size_t wrote = zip_name_to_utf8(cd + off + (size_t)ZIP_CENTRAL_SIZE, (size_t)name_len,
                                                  (flags & ZIP_FLAG_UTF8) != 0u,
                                                  zip->name_pool + pool_used);
            if (wrote == (size_t)-1) {
                pool_used += 1u;      /* 名字里有 NUL:留个空串,标成不可用 */
                e->unusable = 1;
            } else {
                pool_used += wrote + 1u;
            }
        }

        off += need;
        zip->count = n + 1u;
    }

    free(cd);
    /* 空 zip(只有 EOCD、0 个条目)是合法的:照样返回句柄,count() 为 0。 */
    return zip;
}

void sxcl_zip_close(sxcl_zip *zip)
{
    if (zip == NULL) {
        return;
    }
    if (zip->fp != NULL) {
        fclose(zip->fp);
    }
    free(zip->entries);
    free(zip->name_pool);
    free(zip);
}

/* ── 条目查询 ── */

size_t sxcl_zip_count(const sxcl_zip *zip)
{
    return (zip != NULL) ? zip->count : 0u;
}

const char *sxcl_zip_name_at(const sxcl_zip *zip, size_t index)
{
    if (zip == NULL || index >= zip->count) {
        return NULL;
    }
    return zip->entries[index].name;
}

int64_t sxcl_zip_size_at(const sxcl_zip *zip, size_t index)
{
    if (zip == NULL || index >= zip->count) {
        return -1;
    }
    return zip->entries[index].uncomp_size;
}

int sxcl_zip_method_at(const sxcl_zip *zip, size_t index)
{
    if (zip == NULL || index >= zip->count) {
        return -1;
    }
    return zip->entries[index].method;
}

int sxcl_zip_find(const sxcl_zip *zip, const char *name)
{
    size_t i;

    if (zip == NULL || name == NULL) {
        return -1;
    }
    for (i = 0u; i < zip->count; ++i) {
        if (strcmp(zip->entries[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ── 解压 ── */

typedef struct zip_reader {
    FILE          *fp;
    int64_t        remaining;   /* 压缩数据还剩多少字节 */
    unsigned char  buf[ZIP_READ_BUF];
} zip_reader;

/* 取下一块压缩数据:0 = 拿到,1 = 读完,-1 = IO 出错。 */
static int zip_reader_next(zip_reader *r, const unsigned char **out, size_t *out_len)
{
    size_t want;

    if (r->remaining <= 0) {
        return 1;
    }
    want = (r->remaining >= (int64_t)sizeof r->buf) ? sizeof r->buf : (size_t)r->remaining;
    if (fread(r->buf, 1u, want, r->fp) != want) {
        return -1;
    }
    r->remaining -= (int64_t)want;
    *out = r->buf;
    *out_len = want;
    return 0;
}

/* 数据区起点 = 本地头 + 本地头里的名字/扩展域长度。
 * 必须用本地头的字段:中央目录与本地头的扩展域不保证一致。 */
static int zip_locate_data(sxcl_zip *zip, const zip_entry *e, int64_t *data_off)
{
    unsigned char hdr[ZIP_LOCAL_SIZE];
    unsigned name_len;
    unsigned extra_len;
    int64_t off;

    if (e->local_off < 0 || e->local_off + (int64_t)ZIP_LOCAL_SIZE > zip->file_size) {
        return -1;
    }
    if (zip_read_at(zip->fp, e->local_off, hdr, ZIP_LOCAL_SIZE) != 0) {
        return -1;
    }
    if (zip_le32(hdr) != ZIP_SIG_LOCAL) {
        return -1;   /* 本地头签名不对 */
    }
    name_len = (unsigned)zip_le16(hdr + 26u);
    extra_len = (unsigned)zip_le16(hdr + 28u);
    off = e->local_off + (int64_t)ZIP_LOCAL_SIZE + (int64_t)name_len + (int64_t)extra_len;
    if (off < 0 || off > zip->file_size || e->comp_size < 0 ||
        off + e->comp_size > zip->file_size) {
        return -1;   /* 声明的长度超出文件:损坏 */
    }
    *data_off = off;
    return 0;
}

/* 找条目并判可行性;不可行时把错误码写进 *err。 */
static const zip_entry *zip_lookup_usable(const sxcl_zip *zip, const char *name, int *err)
{
    const int idx = sxcl_zip_find(zip, name);
    const zip_entry *e;
    size_t nlen;

    if (idx < 0) {
        *err = SXCL_ZIP_ERR;
        return NULL;
    }
    e = &zip->entries[(size_t)idx];
    nlen = strlen(e->name);
    if (e->unusable != 0 || (e->method != 0 && e->method != 8)) {
        *err = SXCL_ZIP_ERR_UNSUP;
        return NULL;
    }
    if (nlen > 0u && e->name[nlen - 1u] == '/') {
        *err = SXCL_ZIP_ERR_UNSUP;   /* 目录条目没有内容 */
        return NULL;
    }
    *err = SXCL_ZIP_OK;
    return e;
}

int sxcl_zip_extract_stream(sxcl_zip *zip, const char *name,
                            int (*sink)(void *ud, const void *data, size_t len), void *ud)
{
    const zip_entry *e;
    zip_reader rd;
    int64_t data_off = 0;
    int err = SXCL_ZIP_OK;
    int rc;

    if (zip == NULL || name == NULL || sink == NULL) {
        return SXCL_ZIP_ERR;
    }
    e = zip_lookup_usable(zip, name, &err);
    if (e == NULL) {
        return err;
    }
    if (zip_locate_data(zip, e, &data_off) != 0) {
        return SXCL_ZIP_ERR;
    }
    if (fseek(zip->fp, (long)data_off, SEEK_SET) != 0) {
        return SXCL_ZIP_ERR;
    }

    rd.fp = zip->fp;
    rd.remaining = e->comp_size;

    if (e->method == 0) {
        /* stored:压缩数据就是原文,直接透传 */
        for (;;) {
            const unsigned char *blk = NULL;
            size_t blk_len = 0u;
            const int got = zip_reader_next(&rd, &blk, &blk_len);

            if (got < 0) {
                return SXCL_ZIP_ERR;
            }
            if (got > 0) {
                break;   /* 压缩数据读完 */
            }
            if (sink(ud, blk, blk_len) != 0) {
                return SXCL_ZIP_ERR_ABORT;
            }
        }
        return SXCL_ZIP_OK;
    }

    /* deflate:边读边解边写,内存里只有一块读缓冲 + 解压器的 32 KiB 窗口 */
    {
        sxcl_inflate *inf = sxcl_inflate_open();
        if (inf == NULL) {
            return SXCL_ZIP_ERR;
        }
        rc = 0;
        for (;;) {
            const unsigned char *blk = NULL;
            size_t blk_len = 0u;
            const int got = zip_reader_next(&rd, &blk, &blk_len);
            int frc;

            if (got < 0) {
                rc = SXCL_ZIP_ERR;
                break;
            }
            if (got > 0) {
                break;
            }
            frc = sxcl_inflate_feed(inf, blk, blk_len, sink, ud);
            if (frc < 0) {
                rc = (frc == SXCL_INFLATE_ERR_ABORT) ? SXCL_ZIP_ERR_ABORT : SXCL_ZIP_ERR;
                break;
            }
            if (frc == 1) {
                break;   /* deflate 流结束 */
            }
        }
        if (rc == 0 && sxcl_inflate_finish(inf) != 0) {
            rc = SXCL_ZIP_ERR;
        }
        sxcl_inflate_close(inf);
        return rc;
    }
}

/* 内存 sink:容量在 extract_memory 里预先判过,这里只兜底。 */
typedef struct zip_mem_sink {
    unsigned char *out;
    size_t cap;
    size_t len;
    int overflow;
} zip_mem_sink;

static int zip_mem_sink_write(void *ud, const void *data, size_t len)
{
    zip_mem_sink *s = (zip_mem_sink *)ud;

    if (len == 0u) {
        return 0;
    }
    if (s->len >= s->cap || len > s->cap - s->len) {
        s->overflow = 1;
        return 1;   /* 中止 */
    }
    memcpy(s->out + s->len, data, len);
    s->len += len;
    return 0;
}

/* 文件 sink:按偏移顺序写,任何一次写不满就判失败。 */
typedef struct zip_file_sink {
    sxcl_file *file;
    int64_t    off;
    int        bad;
} zip_file_sink;

static int zip_file_sink_write(void *ud, const void *data, size_t len)
{
    zip_file_sink *s = (zip_file_sink *)ud;

    if (len == 0u) {
        return 0;
    }
    if (sxcl_file_write_at(s->file, data, len, s->off) != (int64_t)len) {
        s->bad = 1;
        return 1;   /* 中止 */
    }
    s->off += (int64_t)len;
    return 0;
}

int sxcl_zip_extract_memory(sxcl_zip *zip, const char *name, void *out, size_t out_cap, size_t *out_len)
{
    const zip_entry *e;
    zip_mem_sink ms;
    int err = SXCL_ZIP_OK;
    int rc;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (zip == NULL || name == NULL || (out == NULL && out_cap != 0u)) {
        return SXCL_ZIP_ERR;
    }
    e = zip_lookup_usable(zip, name, &err);
    if (e == NULL) {
        return err;
    }
    if ((uint64_t)e->uncomp_size > (uint64_t)out_cap) {
        if (out_len != NULL) {
            *out_len = (size_t)e->uncomp_size;   /* 告诉调用方需要多大 */
        }
        return SXCL_ZIP_ERR_SPACE;
    }

    ms.out = (unsigned char *)out;
    ms.cap = out_cap;
    ms.len = 0u;
    ms.overflow = 0;

    rc = sxcl_zip_extract_stream(zip, name, zip_mem_sink_write, &ms);
    if (rc == SXCL_ZIP_ERR_ABORT && ms.overflow != 0) {
        rc = SXCL_ZIP_ERR_SPACE;   /* 声明的大小与真实长度不符 */
    }
    if (rc == SXCL_ZIP_OK && out_len != NULL) {
        *out_len = ms.len;
    }
    return rc;
}

int sxcl_zip_extract_file(sxcl_zip *zip, const char *name, const char *dest)
{
    const zip_entry *e;
    zip_file_sink fs;
    char *tmp;
    size_t len;
    int err = SXCL_ZIP_OK;
    int rc;

    if (zip == NULL || name == NULL || dest == NULL) {
        return SXCL_ZIP_ERR;
    }
    e = zip_lookup_usable(zip, name, &err);
    if (e == NULL) {
        return err;
    }
    if (sxcl_fs_mkdirs_for_file(dest) != 0) {
        return SXCL_ZIP_ERR;
    }

    len = strlen(dest);
    tmp = (char *)malloc(len + 5u);
    if (tmp == NULL) {
        return SXCL_ZIP_ERR;
    }
    memcpy(tmp, dest, len);
    memcpy(tmp + len, ".tmp", 5u);

    (void)sxcl_fs_remove(tmp);   /* 上次失败留下的半截 .tmp 不能参与"不截断"的写入 */
    fs.file = sxcl_file_open_write(tmp, -1);
    fs.off = 0;
    fs.bad = 0;
    if (fs.file == NULL) {
        free(tmp);
        return SXCL_ZIP_ERR;
    }

    rc = sxcl_zip_extract_stream(zip, name, zip_file_sink_write, &fs);
    if (sxcl_file_close(fs.file) != 0) {
        rc = SXCL_ZIP_ERR;
    }
    if (rc != SXCL_ZIP_OK) {
        sxcl_fs_remove(tmp);     /* 失败不留 .tmp */
        free(tmp);
        return rc;
    }
    if (fs.bad != 0 || sxcl_fs_rename_replace(tmp, dest) != 0) {
        sxcl_fs_remove(tmp);
        free(tmp);
        return SXCL_ZIP_ERR;
    }
    free(tmp);
    return SXCL_ZIP_OK;
}
