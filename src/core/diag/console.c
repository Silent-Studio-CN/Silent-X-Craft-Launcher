/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 控制台代码页切 UTF-8（声明与理由见 sxcl/console.h）。
// 这里**才**include <windows.h>：头文件里带上它会把宏污染给 libqf 的头（实测会炸）。

#define _CRT_SECURE_NO_WARNINGS 1

#include "sxcl/console.h"

#if defined(_WIN32)
#include <windows.h>
#endif

int sxcl_console_set_utf8(void)
{
#if defined(_WIN32)
    const UINT before = GetConsoleOutputCP();
    if (before != 0 && before != CP_UTF8) {
        (void)SetConsoleOutputCP(CP_UTF8);
    }
    (void)SetConsoleCP(CP_UTF8);
    return (int)before;
#else
    return 0;
#endif
}
