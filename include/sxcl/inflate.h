/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_INFLATE_H
#define SXCL_INFLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 返回值约定(三个接口共用)。 */
#define SXCL_INFLATE_OK         0    /**< 成功 */
#define SXCL_INFLATE_ERR_DATA  (-1)  /**< 数据损坏 / 截断 */
#define SXCL_INFLATE_ERR_SPACE (-2)  /**< 输出缓冲不够(只有 sxcl_inflate_raw 会返回) */
#define SXCL_INFLATE_ERR_ABORT (-3)  /**< sink 返回非 0 主动中止 */

/** DEFLATE 滑动窗口大小(字节)。匹配距离上限也是这个值。 */
#define SXCL_INFLATE_WINDOW 32768u

/** 一次性解压 raw deflate 到内存。
 *
 * 返回:
 *   0   成功,*out_len = 解压后的字节数;
 *  -1   数据损坏或截断;
 *  -2   输出缓冲不够:*out_len 写入"实际需要多少字节"(完整解压后的长度),
 *       调用方可以据此扩容后重试。此时仍会把前 out_cap 字节填进 out。
 *
 * 参数约定:in/out_len 允许为 NULL(out_len 为 NULL 只是拿不到长度);
 * out == NULL 时要求 out_cap == 0(纯粹用来问长度)。
 */
int sxcl_inflate_raw(const void *in, size_t in_len, void *out, size_t out_cap, size_t *out_len);

/** 流式解压器(不透明句柄)。一次只能处理一条 raw deflate 流。 */
typedef struct sxcl_inflate sxcl_inflate;

/** 创建解压器;失败(内存不足)返回 NULL。 */
sxcl_inflate *sxcl_inflate_open(void);

/** 喂一段压缩数据。
 *
 * sink 会按顺序收到解出来的字节(每段可能被切成多次调用);sink 返回非 0 表示
 * 调用方要中止,feed 立即返回 SXCL_INFLATE_ERR_ABORT。sink 允许为 NULL(只丢弃输出)。
 *
 * 返回:
 *   0   这一段吃完了,流还没结束 —— 继续 feed 下一段;
 *   1   本段数据已经解完整条 deflate 流(BFINAL 块结束),后续数据会被忽略;
 *   -1  数据损坏;
 *   -3  sink 主动中止。
 *  出错后状态是"粘"的,后续 feed 一律返回同一个错误码。
 */
int sxcl_inflate_feed(sxcl_inflate *inf, const void *in, size_t in_len,
                      int (*sink)(void *ud, const void *data, size_t len), void *ud);

/** 收尾:确认整条流已经完整解完。
 *  返回 0 = 数据完整; -1 = 截断(或此前出过错)。 */
int sxcl_inflate_finish(sxcl_inflate *inf);

/** 销毁解压器(允许传 NULL)。 */
void sxcl_inflate_close(sxcl_inflate *inf);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_INFLATE_H */
