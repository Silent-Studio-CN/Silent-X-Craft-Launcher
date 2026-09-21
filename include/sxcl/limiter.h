/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_LIMITER_H
#define SXCL_LIMITER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sxcl_limiter sxcl_limiter;

/** 不限速的速率值(字节/秒)。 */
#define SXCL_LIMITER_UNLIMITED 0.0

/** 创建限速器;rate_bps <= 0 表示不限速。失败返回 NULL。 */
sxcl_limiter *sxcl_limiter_create(double rate_bps);

/** 销毁限速器;传 NULL 安全。 */
void sxcl_limiter_destroy(sxcl_limiter *lim);

/** 运行时改速(线程安全)。 */
void sxcl_limiter_set_rate(sxcl_limiter *lim, double rate_bps);

/** 当前速率(字节/秒),0 = 不限速。 */
double sxcl_limiter_rate(const sxcl_limiter *lim);

/** 当前桶容量(字节),0 = 不限速。 */
double sxcl_limiter_burst(const sxcl_limiter *lim);

/** 尝试取 bytes 字节令牌:返回 0 表示已取到;>0 表示还需等待的秒数(未取到)。
 *  未取到时不动桶内令牌,调用者应等待返回的秒数后重试。 */
double sxcl_limiter_take(sxcl_limiter *lim, uint64_t bytes);

/** 阻塞式消费 bytes 字节令牌(内部按 200ms 上限切片轮询)。 */
void sxcl_limiter_consume(sxcl_limiter *lim, uint64_t bytes);

/** 把桶重新装填到容量上限(新任务开始时调用)。 */
void sxcl_limiter_reset(sxcl_limiter *lim);

/** 解析设置页输入("", "b", "k/kb/kib", "m/mb/mib", "g/gb/gib";大小写不敏感)。
 *  非法输入或未知单位返回 0(不限速)。 */
double sxcl_limiter_parse_rate(const char *text);

/** 单调时钟(秒),供引擎与测试共用。 */
double sxcl_limiter_now(void);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_LIMITER_H */
