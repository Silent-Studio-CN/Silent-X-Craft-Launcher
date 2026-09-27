/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// ui2 的令牌表(docs/27 §1:一个底色、两层浮起;强调色只出现在能动手的地方)。
//
// 为什么从 ui2.cpp 里搬出来:§0 第 2 条踩过的坑是"颜色被烤死在控件里",所以颜色只能有**一处**
// 来源 —— 外壳与每个控件都从这里现取,换主题 = 重跑一遍 QSS + 重画图标,没有第二个色表。
#pragma once

#include <QColor>

namespace sxcl::ui2 {

struct Tokens {
    const char *bg;           // 窗口底
    const char *nav;          // 导航/标题行
    const char *surface;      // 卡片
    const char *surfaceHover; // 卡片提亮一档
    const char *text;         // 正文
    const char *text2;        // 次要文字 / 未选中图标
    const char *accent;       // 强调色(主按钮 / 选中指示 / 进度)
    const char *line;         // 线
};

const Tokens &tokens();      // 当前主题的令牌
bool darkTheme();
void setDarkTheme(bool dark);
QColor tokenColor(const char *hex);

} // namespace sxcl::ui2
