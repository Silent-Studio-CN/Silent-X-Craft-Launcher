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

/** 取"这条库在当前平台上的原生库分类器"(老形态:library.natives.<os> 指向 downloads.classifiers)。
 *
 *  一次实测事故催生的函数:启动 PCL 装的 1.8.9-Forge+OptiFine 实例时报
 *  「库 tv.twitch:twitch-platform:6.5 的 natives 分类器 natives-windows-${arch} 没有下载路径」——
 *  1.8.x/1.9.x 的 natives.windows 写的是 **natives-windows-${arch}**,而
 *  downloads.classifiers 里的键是 natives-windows-64 / -32。不展开 ${arch} 就查不到,
 *  老的原版版本于是**一个都启动不了**。
 *
 *  规则:① 原样能找到就用原样的(绝大多数版本);② 找不到且键里有 ${arch} 时按 **64 → 32** 展开再找
 *  (绝大多数机器是 64 位;64 不在才退 32);③ 都没有 → 返回 NULL,并把**原始键**回填到 key_out,
 *  调用方据此报错(报错里要看得见原样,不能只看到展开后的)。
 *  新形态(库名自带 :natives-<os> 分类器、jar 在 downloads.artifact)不走这里。 */
const sxcl_json_value *sxcl_natives_classifier_of(const sxcl_json_value *lib, const char *os_name,
                                                  char *key_out, size_t key_cap);



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
