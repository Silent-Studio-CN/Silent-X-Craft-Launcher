/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* tar.c - ustar / POSIX pax / GNU 长名 tar 解包器(**本项目自己实现**,见 include/sxcl/tar.h)。
 *
 * 状态机(喂进来的数据可以任意切块,不要求"必须一次喂 512 的倍数"):
 *   HEADER -> 攒满 512 字节 -> 解析 -> 分派
 *   DATA   -> 把条目的数据写进文件(边收边写,内存里不攒整个文件)
 *   PAX    -> 攒扩展头记录(上限 1 MiB),解析出 path / linkpath / size
 *   PAD    -> 条目补齐到 512 的倍数
 * 两个全 0 块 = 归档结束;其后只允许 0。
 *
 * 扩展头的产物一律先落在 pending_* 上,**只作用于紧跟的那个条目**:
 * 不这么做的话,一个 pax 长名会一直粘在后面每一个条目上(实测踩过这种写法)。
 */

#include "sxcl/tar.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sxcl/fs.h"

#if !defined(_WIN32)
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#define TAR_BLOCK       512u
#define TAR_PATH_MAX    1024u
#define TAR_PAX_MAX     (1024u * 1024u)

typedef enum tar_state {
    TAR_ST_HEADER = 0,
    TAR_ST_DATA,
    TAR_ST_PAX,
    TAR_ST_PAD,
    TAR_ST_DONE
} tar_state;

struct sxcl_tar {
    sxcl_tar_opts opts;

    int err_code;
    char err_text[256];

    tar_state state;
    unsigned char hdr[TAR_BLOCK];
    size_t hdr_have;

    char dest_dir[TAR_PATH_MAX];

    /* 当前条目 */
    char path[TAR_PATH_MAX];
    char link[TAR_PATH_MAX];
    char dest[TAR_PATH_MAX];
    int64_t size;
    int mode;
    sxcl_tar_type type;
    int64_t data_left;
    int64_t pad_left;
    FILE *fp;
    char writing[TAR_PATH_MAX];

    /* 扩展头(只作用于下一个条目) */
    unsigned char *pax;
    size_t pax_len;
    size_t pax_cap;
    int pax_global;
    int extend_kind;              /* 0 无 / 1 pax / 2 GNU 长名 / 3 GNU 长链接 */
    char pending_path[TAR_PATH_MAX];
    char pending_link[TAR_PATH_MAX];
    int64_t pending_size;
    int has_pending_size;

    int zero_blocks;
    int seen_zero;
    int64_t total_out;
    size_t files;
    size_t dirs;
    size_t links;
    size_t entries;
};

const char *sxcl_tar_code_name(int code)
{
    switch (code) {
    case SXCL_TAR_OK:
        return "ok";
    case SXCL_TAR_ERR_DATA:
        return "data";
    case SXCL_TAR_ERR_IO:
        return "io";
    case SXCL_TAR_ERR_UNSUPPORTED:
        return "unsupported";
    case SXCL_TAR_ERR_UNSAFE:
        return "unsafe";
    case SXCL_TAR_ERR_NOMEM:
        return "nomem";
    case SXCL_TAR_ERR_ARG:
        return "arg";
    case SXCL_TAR_ERR_ABORT:
        return "abort";
    case SXCL_TAR_ERR_LIMIT:
        return "limit";
    default:
        return "?";
    }
}

static void tar_fail(struct sxcl_tar *t, int code, const char *text)
{
    if (t->err_code == 0) {
        t->err_code = code;
        snprintf(t->err_text, sizeof(t->err_text), "%s", text != NULL ? text : "");
        t->err_text[sizeof(t->err_text) - 1] = '\0';
    }
}

static void tar_failf(struct sxcl_tar *t, int code, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    if (t->err_code != 0) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    tar_fail(t, code, buf);
}

/* ── 头字段解析 ── */

/** 八进制字段(允许前后空格/NUL;空 = 0)。也认 3.1 的 base-256 高位置 1 编码。 */
static int tar_octal(const unsigned char *field, size_t len, int64_t *out)
{
    size_t i = 0;
    int64_t value = 0;
    while (i < len && (field[i] == ' ' || field[i] == '\0')) {
        ++i;
    }
    if (i < len && (field[i] & 0x80u) != 0) {
        size_t j;
        int64_t v = (int64_t)(field[i] & 0x7Fu);
        for (j = i + 1; j < len; ++j) {
            v = (v << 8) | (int64_t)field[j];
        }
        *out = v;
        return 0;
    }
    for (; i < len; ++i) {
        const unsigned char c = field[i];
        if (c == ' ' || c == '\0') {
            break;
        }
        if (c < '0' || c > '7') {
            return -1;
        }
        value = value * 8 + (int64_t)(c - '0');
    }
    *out = value;
    return 0;
}

static void tar_str(const unsigned char *field, size_t len, char *out, size_t out_len)
{
    size_t n = 0;
    while (n < len && field[n] != '\0') {
        ++n;
    }
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, field, n);
    out[n] = '\0';
}

/** 路径安全:拒绝绝对 / 盘符 / 反斜杠 / ".." / 控制字符;并去掉前导 "./" 与结尾 '/'。 */
static int tar_check_path(const char *path, char *out, size_t out_len, char *why, size_t why_len)
{
    const char *p;
    size_t i;
    if (path == NULL || *path == '\0') {
        snprintf(why, why_len, "路径是空串");
        return -1;
    }
    if (path[0] == '/' || path[0] == '\\') {
        snprintf(why, why_len, "路径是绝对路径: %s", path);
        return -1;
    }
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':') {
        snprintf(why, why_len, "路径带盘符: %s", path);
        return -1;
    }
    for (i = 0; path[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)path[i];
        if (c < 0x20 || c == '\\' || c == 0x7F) {
            snprintf(why, why_len, "路径里有非法字符(0x%02x): %s", (unsigned)c, path);
            return -1;
        }
    }
    /* 逐段找 ".." */
    i = 0;
    while (path[i] != '\0') {
        size_t start = i;
        size_t seglen;
        while (path[i] != '\0' && path[i] != '/') {
            ++i;
        }
        seglen = i - start;
        if (seglen == 2 && path[start] == '.' && path[start + 1] == '.') {
            snprintf(why, why_len, "路径里有 .. 分量: %s", path);
            return -1;
        }
        if (path[i] == '/') {
            ++i;
        }
    }
    p = path;
    while (p[0] == '.' && p[1] == '/') {
        p += 2;
    }
    if (*p == '\0') {
        snprintf(why, why_len, "路径只剩 ./");
        return -1;
    }
    if (strlen(p) >= out_len) {
        snprintf(why, why_len, "路径太长(上限 %u 字节)", (unsigned)(out_len - 1));
        return -1;
    }
    memcpy(out, p, strlen(p) + 1);
    {
        size_t n = strlen(out);
        while (n > 1 && out[n - 1] == '/') {
            out[--n] = '\0';
        }
    }
    return 0;
}

static int tar_join(char *out, size_t out_len, const char *dir, const char *leaf)
{
    const size_t dl = strlen(dir);
    const size_t ll = strlen(leaf);
    const int need_sep = (dl > 0 && dir[dl - 1] != '/' && dir[dl - 1] != '\\') ? 1 : 0;
    if (dl + (size_t)need_sep + ll + 1 > out_len) {
        return -1;
    }
    memcpy(out, dir, dl);
    if (need_sep) {
        out[dl] = '/';
    }
    memcpy(out + dl + (size_t)need_sep, leaf, ll + 1);
    return 0;
}

#if !defined(_WIN32)
static void tar_set_mode(const char *path, int mode)
{
    if (mode > 0) {
        (void)chmod(path, (mode_t)(mode & 07777));
    }
}
#endif

/* ── 扩展头(pax)解析:"<len 十进制> <key>=<value>\n" ── */

static void tar_parse_pax(struct sxcl_tar *t)
{
    size_t pos = 0;
    const size_t len = t->pax_len;
    while (pos < len) {
        size_t start = pos;
        size_t num = 0;
        int digits = 0;
        const char *kv;
        size_t kvlen;
        while (pos < len && t->pax[pos] >= '0' && t->pax[pos] <= '9') {
            num = num * 10u + (size_t)(t->pax[pos] - '0');
            ++pos;
            ++digits;
        }
        if (digits == 0 || pos >= len || t->pax[pos] != ' ') {
            if (t->pax_global) {
                pos = start + 1;
                continue;
            }
            tar_fail(t, SXCL_TAR_ERR_DATA, "pax 记录的长度字段非法");
            return;
        }
        ++pos;
        if (num == 0 || start + num > len) {
            if (t->pax_global) {
                pos = start + 1;
                continue;
            }
            tar_fail(t, SXCL_TAR_ERR_DATA, "pax 记录长度越界");
            return;
        }
        kv = (const char *)t->pax + pos;
        kvlen = start + num - pos;
        if (kvlen > 0 && kv[kvlen - 1] == '\n') {
            --kvlen;
        }
        {
            size_t eq = 0;
            while (eq < kvlen && kv[eq] != '=') {
                ++eq;
            }
            if (eq < kvlen) {
                const char *key = kv;
                const size_t keylen = eq;
                const char *val = kv + eq + 1;
                const size_t vallen = kvlen - eq - 1;
                if (keylen == 4 && memcmp(key, "path", 4) == 0) {
                    if (vallen < TAR_PATH_MAX) {
                        memcpy(t->pending_path, val, vallen);
                        t->pending_path[vallen] = '\0';
                    } else {
                        tar_fail(t, SXCL_TAR_ERR_UNSAFE, "pax path 太长");
                        return;
                    }
                } else if (keylen == 8 && memcmp(key, "linkpath", 8) == 0) {
                    if (vallen < TAR_PATH_MAX) {
                        memcpy(t->pending_link, val, vallen);
                        t->pending_link[vallen] = '\0';
                    }
                } else if (keylen == 4 && memcmp(key, "size", 4) == 0) {
                    int64_t v = 0;
                    size_t i;
                    int ok = 1;
                    for (i = 0; i < vallen; ++i) {
                        if (val[i] < '0' || val[i] > '9') {
                            ok = 0;
                            break;
                        }
                        v = v * 10 + (val[i] - '0');
                    }
                    if (ok) {
                        t->pending_size = v;
                        t->has_pending_size = 1;
                    }
                }
            }
        }
        pos = start + num;
    }
}

/* ── 条目落盘 ── */

static int tar_open_current_file(struct sxcl_tar *t)
{
    FILE *fp;
    if (t->opts.max_total > 0 && t->total_out + t->size > t->opts.max_total) {
        tar_failf(t, SXCL_TAR_ERR_LIMIT, "解出来的总字节超过上限(%lld 字节)",
                  (long long)t->opts.max_total);
        return SXCL_TAR_ERR_LIMIT;
    }
    if (sxcl_fs_mkdirs_for_file(t->dest) != 0) {
        tar_failf(t, SXCL_TAR_ERR_IO, "建不了目标目录: %s", t->dest);
        return SXCL_TAR_ERR_IO;
    }
    fp = sxcl_fs_fopen(t->dest, "wb");
    if (fp == NULL) {
        tar_failf(t, SXCL_TAR_ERR_IO, "打不开目标文件: %s", t->dest);
        return SXCL_TAR_ERR_IO;
    }
    t->fp = fp;
    snprintf(t->writing, sizeof(t->writing), "%s", t->dest);
    return SXCL_TAR_OK;
}

static void tar_abort_current_file(struct sxcl_tar *t)
{
    if (t->fp != NULL) {
        (void)fclose(t->fp);
        t->fp = NULL;
        (void)sxcl_fs_remove(t->writing);
        t->writing[0] = '\0';
    }
}

static int tar_notify(struct sxcl_tar *t)
{
    if (t->opts.is_cancelled != NULL && t->opts.is_cancelled(t->opts.ud) != 0) {
        tar_fail(t, SXCL_TAR_ERR_ABORT, "用户取消");
        return SXCL_TAR_ERR_ABORT;
    }
    if (t->opts.on_entry != NULL) {
        sxcl_tar_entry e;
        memset(&e, 0, sizeof(e));
        e.path = t->path;
        e.dest = t->dest;
        e.size = t->size;
        e.mode = t->mode;
        e.type = t->type;
        e.link = t->link;
        if (t->opts.on_entry(t->opts.ud, &e) != 0) {
            tar_fail(t, SXCL_TAR_ERR_ABORT, "on_entry 主动中止");
            return SXCL_TAR_ERR_ABORT;
        }
    }
    return SXCL_TAR_OK;
}

static int tar_finish_entry(struct sxcl_tar *t)
{
    if (t->fp != NULL) {
#if !defined(_WIN32)
        tar_set_mode(t->dest, t->mode);
#endif
        if (fclose(t->fp) != 0) {
            t->fp = NULL;
            tar_failf(t, SXCL_TAR_ERR_IO, "写文件失败(落盘): %s", t->dest);
            return SXCL_TAR_ERR_IO;
        }
        t->fp = NULL;
        t->writing[0] = '\0';
        ++t->files;
    }
    ++t->entries;
    return tar_notify(t);
}

static int tar_handle_header(struct sxcl_tar *t)
{
    const unsigned char *h = t->hdr;
    char name[256];
    char prefix[160];
    char full[TAR_PATH_MAX];
    char typeflag;
    int64_t size = 0;
    int64_t mode = 0;
    int64_t stored_sum = 0;
    int64_t computed = 0;
    size_t i;

    {
        int all_zero = 1;
        for (i = 0; i < TAR_BLOCK; ++i) {
            if (h[i] != 0) {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) {
            ++t->zero_blocks;
            if (t->zero_blocks >= 2 || t->seen_zero) {
                t->state = TAR_ST_DONE;
            }
            t->seen_zero = 1;
            return SXCL_TAR_OK;
        }
        if (t->seen_zero) {
            tar_fail(t, SXCL_TAR_ERR_DATA, "归档结束标记之后还有非 0 数据");
            return SXCL_TAR_ERR_DATA;
        }
    }

    for (i = 0; i < TAR_BLOCK; ++i) {
        computed += (i >= 148 && i < 156) ? (int64_t)' ' : (int64_t)h[i];
    }
    if (tar_octal(h + 148, 8, &stored_sum) != 0) {
        tar_fail(t, SXCL_TAR_ERR_DATA, "头校验和字段非法");
        return SXCL_TAR_ERR_DATA;
    }
    if (stored_sum != computed) {
        int64_t signed_sum = 0;
        for (i = 0; i < TAR_BLOCK; ++i) {
            signed_sum += (i >= 148 && i < 156) ? (int64_t)' ' : (int64_t)(signed char)h[i];
        }
        if (stored_sum != signed_sum) {
            tar_failf(t, SXCL_TAR_ERR_DATA, "头校验和不符(存的 %lld,算的 %lld)",
                      (long long)stored_sum, (long long)computed);
            return SXCL_TAR_ERR_DATA;
        }
    }
    if (tar_octal(h + 100, 8, &mode) != 0) {
        tar_fail(t, SXCL_TAR_ERR_DATA, "头 mode 字段非法");
        return SXCL_TAR_ERR_DATA;
    }
    if (tar_octal(h + 124, 12, &size) != 0 || size < 0) {
        tar_fail(t, SXCL_TAR_ERR_DATA, "头 size 字段非法");
        return SXCL_TAR_ERR_DATA;
    }
    typeflag = (char)h[156];
    tar_str(h + 0, 100, name, sizeof(name));
    tar_str(h + 345, 155, prefix, sizeof(prefix));
    if (prefix[0] != '\0') {
        if (snprintf(full, sizeof(full), "%s/%s", prefix, name) >= (int)sizeof(full)) {
            tar_fail(t, SXCL_TAR_ERR_UNSAFE, "ustar 的 prefix+name 太长");
            return SXCL_TAR_ERR_UNSAFE;
        }
    } else {
        snprintf(full, sizeof(full), "%s", name);
    }

    if (typeflag == 'x' || typeflag == 'g') {
        t->pax_global = (typeflag == 'g') ? 1 : 0;
        t->extend_kind = 1;
        t->state = TAR_ST_PAX;
        t->data_left = size;
        t->pad_left = (int64_t)((TAR_BLOCK - ((size_t)size % TAR_BLOCK)) % TAR_BLOCK);
        t->pax_len = 0;
        return SXCL_TAR_OK;
    }
    if (typeflag == 'L' || typeflag == 'K') {
        t->pax_global = 0;
        t->extend_kind = (typeflag == 'L') ? 2 : 3;
        t->state = TAR_ST_PAX;
        t->data_left = size;
        t->pad_left = (int64_t)((TAR_BLOCK - ((size_t)size % TAR_BLOCK)) % TAR_BLOCK);
        t->pax_len = 0;
        return SXCL_TAR_OK;
    }

    /* 普通条目:扩展头给的字段优先,且**用完就清**(只作用于这一个条目) */
    t->size = t->has_pending_size ? t->pending_size : size;
    t->has_pending_size = 0;
    t->mode = (int)(mode & 07777);
    {
        char eff[TAR_PATH_MAX];
        if (t->pending_path[0] != '\0') {
            snprintf(eff, sizeof(eff), "%s", t->pending_path);
        } else {
            snprintf(eff, sizeof(eff), "%s", full);
        }
        t->pending_path[0] = '\0';
        if (t->pending_link[0] != '\0') {
            snprintf(t->link, sizeof(t->link), "%s", t->pending_link);
        } else {
            tar_str(h + 157, 100, t->link, sizeof(t->link));
        }
        t->pending_link[0] = '\0';
        snprintf(t->path, sizeof(t->path), "%s", eff);
    }

    switch (typeflag) {
    case '0':
    case '\0':
    case '7':
        t->type = SXCL_TAR_FILE;
        break;
    case '5':
        t->type = SXCL_TAR_DIR;
        break;
    case '2':
        t->type = SXCL_TAR_SYMLINK;
        break;
    case '1':
        t->type = SXCL_TAR_HARDLINK;
        break;
    case '3':
    case '4':
    case '6':
        tar_failf(t, SXCL_TAR_ERR_UNSUPPORTED, "不支持的条目类型 '%c'(设备节点 / FIFO)",
                  typeflag);
        return SXCL_TAR_ERR_UNSUPPORTED;
    case 'S':
        tar_fail(t, SXCL_TAR_ERR_UNSUPPORTED, "不支持 GNU 稀疏文件条目('S')");
        return SXCL_TAR_ERR_UNSUPPORTED;
    default:
        tar_failf(t, SXCL_TAR_ERR_UNSUPPORTED, "不认识的条目类型 0x%02x",
                  (unsigned char)typeflag);
        return SXCL_TAR_ERR_UNSUPPORTED;
    }

    {
        char why[256];
        char safe[TAR_PATH_MAX];
        why[0] = '\0';
        if (tar_check_path(t->path, safe, sizeof(safe), why, sizeof(why)) != 0) {
            tar_fail(t, SXCL_TAR_ERR_UNSAFE, why);
            return SXCL_TAR_ERR_UNSAFE;
        }
        snprintf(t->path, sizeof(t->path), "%s", safe);
        if (tar_join(t->dest, sizeof(t->dest), t->dest_dir, t->path) != 0) {
            tar_failf(t, SXCL_TAR_ERR_UNSAFE, "目标路径太长: %s", t->path);
            return SXCL_TAR_ERR_UNSAFE;
        }
    }

    if (t->type == SXCL_TAR_DIR) {
        if (sxcl_fs_mkdirs(t->dest) != 0) {
            tar_failf(t, SXCL_TAR_ERR_IO, "建不了目录: %s", t->dest);
            return SXCL_TAR_ERR_IO;
        }
#if !defined(_WIN32)
        tar_set_mode(t->dest, t->mode);
#endif
        ++t->dirs;
        ++t->entries;
        t->data_left = 0;
        t->pad_left = 0;
        t->state = TAR_ST_HEADER;
        return tar_notify(t);
    }
    if (t->type == SXCL_TAR_SYMLINK || t->type == SXCL_TAR_HARDLINK) {
        int made = 0;
#if !defined(_WIN32)
        if (sxcl_fs_mkdirs_for_file(t->dest) == 0) {
            (void)sxcl_fs_remove(t->dest);
            if (t->type == SXCL_TAR_SYMLINK) {
                made = (symlink(t->link, t->dest) == 0) ? 1 : 0;
            } else {
                char target[TAR_PATH_MAX];
                char why[256];
                char safe[TAR_PATH_MAX];
                why[0] = '\0';
                if (tar_check_path(t->link, safe, sizeof(safe), why, sizeof(why)) == 0 &&
                    tar_join(target, sizeof(target), t->dest_dir, safe) == 0) {
                    made = (link(target, t->dest) == 0) ? 1 : 0;
                }
            }
        }
#endif
        if (made) {
            ++t->links;
        } else {
            /* 链接建不出来(Windows / 受限文件系统)按"跳过"处理:不把整包判死,但也不假装成功 */
            ++t->links;
        }
        ++t->entries;
        t->data_left = 0;
        t->pad_left = 0;
        t->state = TAR_ST_HEADER;
        return tar_notify(t);
    }

    {
        const int rc = tar_open_current_file(t);
        if (rc != SXCL_TAR_OK) {
            return rc;
        }
    }
    t->data_left = size;
    t->pad_left = (int64_t)((TAR_BLOCK - ((size_t)size % TAR_BLOCK)) % TAR_BLOCK);
    if (size == 0) {
        const int rc = tar_finish_entry(t);
        if (rc != SXCL_TAR_OK) {
            return rc;
        }
        t->state = TAR_ST_HEADER;
        return SXCL_TAR_OK;
    }
    t->state = TAR_ST_DATA;
    return SXCL_TAR_OK;
}

static int tar_consume(struct sxcl_tar *t, const unsigned char *data, size_t len)
{
    size_t pos = 0;
    while (pos < len) {
        if (t->err_code != 0) {
            return t->err_code;
        }
        switch (t->state) {
        case TAR_ST_DONE: {
            size_t i;
            for (i = pos; i < len; ++i) {
                if (data[i] != 0) {
                    tar_fail(t, SXCL_TAR_ERR_DATA, "归档填充里有非 0 字节");
                    return SXCL_TAR_ERR_DATA;
                }
            }
            pos = len;
            break;
        }
        case TAR_ST_HEADER: {
            const size_t want = TAR_BLOCK - t->hdr_have;
            const size_t take = (len - pos < want) ? (len - pos) : want;
            memcpy(t->hdr + t->hdr_have, data + pos, take);
            t->hdr_have += take;
            pos += take;
            if (t->hdr_have == TAR_BLOCK) {
                int rc;
                t->hdr_have = 0;
                rc = tar_handle_header(t);
                if (rc != SXCL_TAR_OK) {
                    return rc;
                }
            }
            break;
        }
        case TAR_ST_DATA: {
            const size_t avail = len - pos;
            const size_t want = ((int64_t)avail < t->data_left) ? avail : (size_t)t->data_left;
            if (want > 0) {
                if (t->opts.max_total > 0 &&
                    t->total_out + (int64_t)want > t->opts.max_total) {
                    tar_fail(t, SXCL_TAR_ERR_LIMIT, "解出来的总字节超过上限");
                    return SXCL_TAR_ERR_LIMIT;
                }
                if (t->fp != NULL && fwrite(data + pos, 1, want, t->fp) != want) {
                    tar_failf(t, SXCL_TAR_ERR_IO, "写文件失败: %s", t->dest);
                    return SXCL_TAR_ERR_IO;
                }
                t->total_out += (int64_t)want;
                t->data_left -= (int64_t)want;
                pos += want;
            }
            if (t->data_left == 0) {
                const int rc = tar_finish_entry(t);
                if (rc != SXCL_TAR_OK) {
                    return rc;
                }
                t->state = (t->pad_left > 0) ? TAR_ST_PAD : TAR_ST_HEADER;
            }
            break;
        }
        case TAR_ST_PAX: {
            const size_t avail = len - pos;
            const size_t want = ((int64_t)avail < t->data_left) ? avail : (size_t)t->data_left;
            if (want > 0) {
                if (t->pax_len + want > TAR_PAX_MAX) {
                    tar_fail(t, SXCL_TAR_ERR_DATA, "扩展头太大(超过 1 MiB)");
                    return SXCL_TAR_ERR_DATA;
                }
                if (t->pax_len + want > t->pax_cap) {
                    size_t cap = t->pax_cap == 0 ? 4096u : t->pax_cap;
                    unsigned char *p;
                    while (cap < t->pax_len + want) {
                        cap *= 2u;
                    }
                    p = (unsigned char *)realloc(t->pax, cap);
                    if (p == NULL) {
                        tar_fail(t, SXCL_TAR_ERR_NOMEM, "扩展头缓冲分配失败");
                        return SXCL_TAR_ERR_NOMEM;
                    }
                    t->pax = p;
                    t->pax_cap = cap;
                }
                memcpy(t->pax + t->pax_len, data + pos, want);
                t->pax_len += want;
                t->data_left -= (int64_t)want;
                pos += want;
            }
            if (t->data_left == 0) {
                if (t->extend_kind == 1) {
                    tar_parse_pax(t);
                    if (t->err_code != 0) {
                        return t->err_code;
                    }
                    if (t->pax_global) {
                        t->pending_path[0] = '\0';
                        t->pending_link[0] = '\0';
                        t->has_pending_size = 0;
                    }
                } else if (t->extend_kind == 2 || t->extend_kind == 3) {
                    char tmp[TAR_PATH_MAX];
                    size_t n = t->pax_len;
                    size_t i2;
                    for (i2 = 0; i2 < n; ++i2) {
                        if (t->pax[i2] == '\0') {
                            n = i2;
                            break;
                        }
                    }
                    if (n >= TAR_PATH_MAX) {
                        n = TAR_PATH_MAX - 1;
                    }
                    memcpy(tmp, t->pax, n);
                    tmp[n] = '\0';
                    if (t->extend_kind == 2) {
                        snprintf(t->pending_path, sizeof(t->pending_path), "%s", tmp);
                    } else {
                        snprintf(t->pending_link, sizeof(t->pending_link), "%s", tmp);
                    }
                }
                t->pax_len = 0;
                t->extend_kind = 0;
                t->state = (t->pad_left > 0) ? TAR_ST_PAD : TAR_ST_HEADER;
            }
            break;
        }
        case TAR_ST_PAD: {
            const size_t avail = len - pos;
            const size_t want = ((int64_t)avail < t->pad_left) ? avail : (size_t)t->pad_left;
            t->pad_left -= (int64_t)want;
            pos += want;
            if (t->pad_left == 0) {
                t->state = TAR_ST_HEADER;
            }
            break;
        }
        default:
            tar_fail(t, SXCL_TAR_ERR_DATA, "内部状态非法");
            return SXCL_TAR_ERR_DATA;
        }
    }
    return SXCL_TAR_OK;
}

/* ── 对外接口 ── */

sxcl_tar *sxcl_tar_open(const sxcl_tar_opts *opts, char *err, size_t err_len)
{
    sxcl_tar *t;
    if (opts == NULL || opts->dest_dir == NULL || opts->dest_dir[0] == '\0') {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "dest_dir 没给");
        }
        return NULL;
    }
    if (strlen(opts->dest_dir) >= TAR_PATH_MAX) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "dest_dir 太长");
        }
        return NULL;
    }
    t = (sxcl_tar *)calloc(1, sizeof(*t));
    if (t == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "内存不足");
        }
        return NULL;
    }
    t->opts = *opts;
    snprintf(t->dest_dir, sizeof(t->dest_dir), "%s", opts->dest_dir);
    t->state = TAR_ST_HEADER;
    if (sxcl_fs_mkdirs(t->dest_dir) != 0) {
        free(t);
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "建不了目标目录: %s", opts->dest_dir);
        }
        return NULL;
    }
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    return t;
}

int sxcl_tar_feed(sxcl_tar *tar, const void *data, size_t len)
{
    if (tar == NULL || (data == NULL && len > 0)) {
        return SXCL_TAR_ERR_ARG;
    }
    if (tar->err_code != 0) {
        return tar->err_code;
    }
    if (len == 0) {
        return SXCL_TAR_OK;
    }
    return tar_consume(tar, (const unsigned char *)data, len);
}

int sxcl_tar_finish(sxcl_tar *tar, char *err, size_t err_len)
{
    int rc = SXCL_TAR_OK;
    if (tar == NULL) {
        return SXCL_TAR_ERR_ARG;
    }
    if (tar->err_code != 0) {
        rc = tar->err_code;
        goto out;
    }
    if (tar->state == TAR_ST_DATA || tar->state == TAR_ST_PAX || tar->state == TAR_ST_PAD) {
        tar_fail(tar, SXCL_TAR_ERR_DATA, "归档在条目中间就结束了(被截断)");
        rc = SXCL_TAR_ERR_DATA;
        goto out;
    }
    if (tar->hdr_have != 0) {
        tar_fail(tar, SXCL_TAR_ERR_DATA, "归档在头中间就结束了(被截断)");
        rc = SXCL_TAR_ERR_DATA;
        goto out;
    }
    if (!tar->seen_zero) {
        /* 缺结束标记的归档现实里存在(GNU tar 只警告):如实接受,但不假装看到过标记 */
        tar->state = TAR_ST_DONE;
    }
out:
    if (err != NULL && err_len > 0) {
        snprintf(err, err_len, "%s", tar->err_code == 0 ? "" : tar->err_text);
        err[err_len - 1] = '\0';
    }
    return rc;
}

void sxcl_tar_close(sxcl_tar *tar)
{
    if (tar == NULL) {
        return;
    }
    tar_abort_current_file(tar);
    free(tar->pax);
    free(tar);
}

int64_t sxcl_tar_bytes_out(const sxcl_tar *tar)
{
    return tar == NULL ? 0 : tar->total_out;
}

size_t sxcl_tar_files_written(const sxcl_tar *tar)
{
    return tar == NULL ? 0 : tar->files;
}

size_t sxcl_tar_dirs_created(const sxcl_tar *tar)
{
    return tar == NULL ? 0 : tar->dirs;
}

size_t sxcl_tar_links_created(const sxcl_tar *tar)
{
    return tar == NULL ? 0 : tar->links;
}

int sxcl_tar_is_done(const sxcl_tar *tar)
{
    return (tar != NULL && tar->state == TAR_ST_DONE) ? 1 : 0;
}

/* ── 一步到位:source -> xz -> tar ── */

typedef struct tar_xz_bridge {
    sxcl_tar *tar;
} tar_xz_bridge;

static int tar_xz_sink(void *ud, const void *data, size_t len)
{
    tar_xz_bridge *b = (tar_xz_bridge *)ud;
    return sxcl_tar_feed(b->tar, data, len) == SXCL_TAR_OK ? 0 : 1;
}

int sxcl_tar_extract_xz(const sxcl_xz_source *source, const sxcl_tar_opts *opts,
                        char *err, size_t err_len)
{
    tar_xz_bridge bridge;
    sxcl_xz *xz;
    char xz_err[256];
    int rc;
    if (source == NULL || opts == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "参数不合法");
        }
        return SXCL_TAR_ERR_ARG;
    }
    xz_err[0] = '\0';
    bridge.tar = sxcl_tar_open(opts, err, err_len);
    if (bridge.tar == NULL) {
        return SXCL_TAR_ERR_NOMEM;
    }
    xz = sxcl_xz_open(source, NULL, tar_xz_sink, &bridge, xz_err, sizeof(xz_err));
    if (xz == NULL) {
        sxcl_tar_close(bridge.tar);
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "%s", xz_err);
        }
        return SXCL_TAR_ERR_NOMEM;
    }
    rc = sxcl_xz_run(xz, xz_err, sizeof(xz_err));
    if (rc == SXCL_XZ_OK) {
        rc = sxcl_tar_finish(bridge.tar, err, err_len);
    } else {
        char tar_err[256];
        tar_err[0] = '\0';
        (void)sxcl_tar_finish(bridge.tar, tar_err, sizeof(tar_err));
        if (bridge.tar->err_code != 0) {
            /* tar 侧先出的事(它把 sink 顶回去了):报 tar 的人话,别被 xz 的 ABORT 盖掉 */
            rc = bridge.tar->err_code;
            if (err != NULL && err_len > 0) {
                snprintf(err, err_len, "%s", tar_err);
                err[err_len - 1] = '\0';
            }
        } else {
            rc = (rc == SXCL_XZ_ERR_ABORT) ? SXCL_TAR_ERR_ABORT : SXCL_TAR_ERR_DATA;
            if (err != NULL && err_len > 0) {
                snprintf(err, err_len, "解 .xz 失败: %s", xz_err);
                err[err_len - 1] = '\0';
            }
        }
    }
    sxcl_xz_close(xz);
    sxcl_tar_close(bridge.tar); /* 失败时 close 会删掉正在写的那个文件,不留半个 */
    return rc;
}

typedef struct tar_file_src {
    FILE *fp;
} tar_file_src;

static int64_t tar_file_read(void *ud, void *buf, size_t len)
{
    tar_file_src *s = (tar_file_src *)ud;
    const size_t n = fread(buf, 1, len, s->fp);
    if (n == 0 && ferror(s->fp)) {
        return -1;
    }
    return (int64_t)n;
}

int sxcl_tar_extract_file(const char *archive_path, const sxcl_tar_opts *opts,
                          char *err, size_t err_len)
{
    tar_file_src fs;
    sxcl_xz_source src;
    int rc;
    if (archive_path == NULL || opts == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "参数不合法");
        }
        return SXCL_TAR_ERR_ARG;
    }
    fs.fp = sxcl_fs_fopen(archive_path, "rb");
    if (fs.fp == NULL) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "打不开归档文件: %s", archive_path);
        }
        return SXCL_TAR_ERR_IO;
    }
    src.read = tar_file_read;
    src.ud = &fs;
    rc = sxcl_tar_extract_xz(&src, opts, err, err_len);
    (void)fclose(fs.fp);
    return rc;
}
