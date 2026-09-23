/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#ifndef SXCL_CONSOLE_H
#define SXCL_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 控制台中文别变成"鐣岄潰灏辩华"（用户 2026-09-23 截图报障）。
 *
 * 原因：我们所有输出（日志、[sxcl-ui] 那些行、CLI 的结果）都是 **UTF-8 字节**，
 * 而 Windows 控制台默认按本机 ANSI 代码页（简中是 936/GBK）解释这些字节 ——
 * 于是每个汉字都糊成两三个别的字。文件里的日志是对的（UTF-8），只有**看**的时候糊。
 *
 * 修法：进程一起来就把这个控制台的输出代码页切成 UTF-8（65001）。这是**控制台**的属性，
 * 只影响"往这个控制台打"的显示，不改我们写出去的字节，也不影响重定向/管道
 * （管道里本来就是 UTF-8 字节，读的人自己决定怎么解）。
 *
 * **头文件里绝不 include <windows.h>**：它会把 min/max、GetObject 之类宏带进来，
 * 后面再 include libqf 的头就炸（实测 fluent_icon.h 直接语法错误）。
 * 实现放 src/core/diag/console.c，这里只留声明。
 *
 * 返回值：切之前的代码页（拿不到控制台时是 0）—— 调用方把它写进日志，
 * 用户报障时我们能一眼看出"当时控制台是 936 还是已经 65001"。
 * 非 Windows 上是空操作，返回 0。
 */
int sxcl_console_set_utf8(void);

#ifdef __cplusplus
}
#endif

#endif /* SXCL_CONSOLE_H */
