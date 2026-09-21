/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_NATIVES_H
#define SXCL_NATIVES_H

#include <stddef.h>

#include "sxcl/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 错误信息/路径缓冲的建议大小。 */
#define SXCL_NATIVES_PATH_MAX 1024

/** 由版本 JSON 文件路径准备 natives(内部自己解析 JSON)。 */
int sxcl_natives_prepare(const char *version_json_path, const char *game_dir,
                         const char *natives_dir, char *err, size_t err_len);

/** 同上,但直接吃已经解析好的版本 JSON(启动驱动手里就有一份,不必重复解析)。 */
int sxcl_natives_prepare_json(const sxcl_json *version_json, const char *game_dir,
                              const char *natives_dir, char *err, size_t err_len);

/** 上一次 sxcl_natives_prepare* 之后 natives_dir 里**可用**的原生库文件数
 *  (本次新解的 + 上次解过且仍然有效的)。失败时是 0。
 *  语义是"就绪了几个",不是"这次写了几个" —— 幂等跳过的那次也会照常报同样的数量,
 *  界面可以拿它显示"原生库 12 个文件已就绪"。
 *  线程安全:**不做任何承诺**(与 settings/options 一致,启动器里这条线是单线程的)。 */
int sxcl_natives_last_count(void);

#ifdef __cplusplus
}
#endif
#endif /* SXCL_NATIVES_H */
