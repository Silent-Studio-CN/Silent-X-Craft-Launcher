/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// assets/icons/ui/* 的取图(微软 LOGO / 断线图标)。出处见 assets/icons/ui/NOTICE.md。
//
// 为什么单独一层:§0 第 2 条的坑是"构造期把颜色烤死" —— 图标要么自带官方色(微软 LOGO),
// 要么按**当前主题令牌**现染(断线图标这类单色线稿),所以取图时必须把"染什么色"作为参数,
// 而不是在控件构造时算一次。
#pragma once

#include <QColor>
#include <QPixmap>
#include <QString>

namespace sxcl::ui2 {

/** assets/icons/ui/<file> 的绝对路径(编译期把目录写死,运行时按需回退 exe 旁)。 */
QString uiIconPath(const QString &file);

/** 把 <file> 渲染成 size x size 逻辑像素的位图(dpr = 缩放比)。
 *  tint 有效 = 用 alpha 蒙版整枚染成该色(单色线稿图标);tint 无效 = 保留素材本色(微软 LOGO)。
 *  文件缺失/读不出来 -> 返回空 QPixmap 并打一行"图标缺失",界面继续跑(与老界面同一条纪律)。 */
QPixmap iconPixmap(const QString &file, int size, const QColor &tint, qreal dpr);

} // namespace sxcl::ui2
