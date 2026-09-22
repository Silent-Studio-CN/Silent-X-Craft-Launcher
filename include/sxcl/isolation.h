/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 「版本隔离」（PCL 的叫法；docs/22 的 A2、docs/24 的 P3 前置）。
//
// 用户的概念基础是"版本 = versions/ 下的文件夹名"（见 docs/23）。开了隔离以后，
// **这一个版本的数据目录就是它自己的文件夹**：
//
//   <游戏目录>/versions/<版本名>/          ← --gameDir 指这儿（mods / saves / config / options.txt …）
//   <游戏目录>/assets/                     ← 仍然共用（--assetsDir）
//   <游戏目录>/libraries/                  ← 仍然共用（classpath）
//   <游戏目录>/versions/<版本名>/<版本名>.jar / -natives   ← 客户端 jar 与原生库还在原地
//
// 于是"两份 1.20.1 各自装各自的模组"才成立 —— 这是模组页（A1）的前置条件。
// 关着（默认）时一切照旧：所有版本共用一个根目录。
//
// 这一份接口只管**目录怎么算、要建哪些子目录**；把 --gameDir 换成隔离目录是驱动层的事
// （driver.c），界面只是显示/开关。
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 隔离目录 = <game_dir>/versions/<版本名>。成功 0，拼不出来/参数非法返回 -1。 */
int sxcl_launch_isolated_dir(const char *game_dir, const char *version_name, char *out,
                             size_t out_len);

/** 隔离目录里那些"游戏自己要写"的标准子目录（mods / saves / config …）。
 *  写进 out（最多 cap 条），返回**实际条数**（可能大于 cap，此时只写了前 cap 条）。
 *  out 可为 NULL（只问条数）。 */
size_t sxcl_launch_isolated_subdirs(const char **out, size_t cap);

/** 把隔离目录与它的标准子目录建出来（已存在不动，**不复制、不删除**任何东西）。
 *  game_dir 下的其它版本、根目录的 assets/libraries 一个字节都不碰。
 *  成功返回 0；失败把原因写进 err 并返回 -1。 */
int sxcl_launch_prepare_isolated(const char *game_dir, const char *version_name, char *err,
                                 size_t err_len);

#ifdef __cplusplus
}
#endif
