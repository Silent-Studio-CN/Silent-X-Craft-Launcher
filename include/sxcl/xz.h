/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_XZ_H
#define SXCL_XZ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── XZ(.xz)流式解压:本项目**自己实现**的解码器(不引第三方源码) ──
 *
 * 为什么需要它:安卓 arm64 的 JRE 自托管包按 FCL 的**目录思路**做成
 * `universal.tar.xz` + `bin-<abi>.tar.xz`(见 docs/19),而仓库里只有 DEFLATE
 * (src/core/inflate.c)与 ZIP(src/services/archive/zip.c),**没有任何 LZMA/XZ 能力**。
 * 为了不把 GPL/公开实现整段抄进来,这里按 .xz 容器格式与 LZMA2 规范自己写一份。
 *
 * 支持的子集(**如实说明**,不支持的宁可报错也不静默解出垃圾):
 *   * 容器:.xz 流格式(头/块/索引/尾),支持**多流串联**与流之间的 4 字节对齐填充;
 *   * 校验:none(0)/ CRC32(1)/ CRC64(4),逐个块校验 + 校验索引与尾;
 *   * 过滤器:**只有 LZMA2(Filter ID = 0x21)**;出现 BCJ/Delta 等其它过滤器一律
 *     返回 SXCL_XZ_ERR_UNSUPPORTED 并点名;
 *   * 数据块:未压缩块(control 0x01/0x02)与 LZMA 块(control 0x80..0xFF)全支持,
 *     含字典重置 / 状态重置 / 重新给属性三种 reset 模式;
 *   * 字典:必须在 opts.dict_limit 之内(默认 64 MiB),超了报 SXCL_XZ_ERR_MEMLIMIT ——
 *     这是防"解压炸弹"的一道闸。
 *
 * **不做**的事(不做也不假装做过):
 *   * 不支持 BCJ 系列过滤器(现实中 `xz`/`tar -J` 压普通数据时不会用,`--x86` 才用);
 *   * 不解 .lzma(alone 格式)与 .lz(旧 lzip);
 *   * 不做多线程解压(单线程;瓶颈在 I/O,不在 CPU)。
 *
 * 用法:给一个"拉字节"的 source(xz 自己按需拉,不要求调用方倒推状态机),
 * 给一个"收字节"的 sink(解出来的字节按顺序给它)。返回码见下面。
 */

/* 返回码(负数为错) */
#define SXCL_XZ_OK               0  /**< 解完且每一道校验都过 */
#define SXCL_XZ_ERR_DATA       (-1) /**< 数据损坏(魔数/CRC/索引/尾不一致、块结构不对) */
#define SXCL_XZ_ERR_UNSUPPORTED (-2)/**< 用到了不支持的东西(非 LZMA2 过滤器/未知校验类型) */
#define SXCL_XZ_ERR_IO         (-3) /**< source 报错 */
#define SXCL_XZ_ERR_ABORT      (-4) /**< sink 返回非 0(调用方主动中止,例如用户取消) */
#define SXCL_XZ_ERR_NOMEM      (-5) /**< 内存不足 */
#define SXCL_XZ_ERR_ARG        (-6) /**< 参数不合法 */
#define SXCL_XZ_ERR_MEMLIMIT   (-7) /**< 字典超过 dict_limit(防解压炸弹) */
#define SXCL_XZ_ERR_LIMIT      (-8) /**< 解出来的总量超过 out_limit */

/** 返回码的稳定名字("ok"/"data"/"unsupported"/"io"/"abort"/"nomem"/"arg"/"memlimit"/"limit")。 */
const char *sxcl_xz_code_name(int code);

/** 输入来源:拉字节。read 返回 >0 = 读到的字节数;0 = 到底了;<0 = 出错(整个解码以 SXCL_XZ_ERR_IO 收场)。
 *  实现**必须**是"要么读满要么说明白"的语义:允许少于 len(短读),下一次接着读。 */
typedef struct sxcl_xz_source {
    int64_t (*read)(void *ud, void *buf, size_t len);
    void *ud;
} sxcl_xz_source;

/** 解码配置。全 0 = 默认(dict_limit = 64 MiB,out_limit = 不限)。 */
typedef struct sxcl_xz_opts {
    /** 字典上限(字节):LZMA2 过滤器属性里声明的字典超过它就拒解。<= 0 = 64 MiB。 */
    int64_t dict_limit;
    /** 解压总量上限(字节):>0 时超过就报 SXCL_XZ_ERR_LIMIT。<= 0 = 不限。 */
    int64_t out_limit;
} sxcl_xz_opts;

/** 不透明句柄。 */
typedef struct sxcl_xz sxcl_xz;

/** 建一个解码器。sink 每收到一段解出来的字节就被调一次(同一段可能被切多次);
 *  sink 返回非 0 -> 立刻以 SXCL_XZ_ERR_ABORT 收场。sink 允许为 NULL(只校验,不取数据)。
 *  失败返回 NULL 并写人话 err。 */
sxcl_xz *sxcl_xz_open(const sxcl_xz_source *source, const sxcl_xz_opts *opts,
                      int (*sink)(void *ud, const void *data, size_t len), void *ud,
                      char *err, size_t err_len);

/** 跑到流结束。成功返回 SXCL_XZ_OK;失败返回负错误码并写人话 err(err 可空)。
 *  同一个句柄重复调用:第一次跑到结束之后再调返回第一次的结果(粘性)。 */
int sxcl_xz_run(sxcl_xz *xz, char *err, size_t err_len);

/** 释放(允许 NULL)。 */
void sxcl_xz_close(sxcl_xz *xz);

/** 诊断:解出来的字节总数(含已交给 sink 的)。 */
int64_t sxcl_xz_out_bytes(const sxcl_xz *xz);
/** 诊断:流用的校验类型(0 = none,1 = CRC32,4 = CRC64)。 */
int sxcl_xz_check_type(const sxcl_xz *xz);

/* ── 便捷入口(测试/小文件用) ── */

/** 内存 -> 内存。out_cap 不够返回 SXCL_XZ_ERR_MEMLIMIT? 不:返回 SXCL_XZ_ERR_NOMEM 语义的
 *  SXCL_XZ_ERR_LIMIT,并把实际需要的字节数写进 *out_len。 */
int sxcl_xz_decode_memory(const void *in, size_t in_len, void *out, size_t out_cap,
                          size_t *out_len, char *err, size_t err_len);

/** 内存 -> 堆缓冲(malloc,调用方 free)。成功把首地址写进 *out、长度写进 *out_len。 */
int sxcl_xz_decode_alloc(const void *in, size_t in_len, unsigned char **out, size_t *out_len,
                         char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_XZ_H */
