/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 启动页上的小贴士(用户 2026-09-27:「下面换成一些动态切换的小知识小 tips,不要给用户看代码」)。
//
// 单列一个文件而不是写死在页面里:这张表是**文案**,验收要能直接把它整表读出来逐条检查
// (有没有代码/网址/术语/emoji),而不用去 dump 界面。
#pragma once

#include <QStringList>

namespace sxcl::ui {

/** 启动页轮播的小贴士(**顺序固定**,页面按顺序一条条换 —— 顺序随机的话验收与截图都没法复现)。
 *  铁律:一条都不许出现代码、网址、命令、我们自己的专业词(JSON / 依赖库 / 版本隔离…),
 *  也不许出现 emoji。写的都是玩家真正用得上的游戏小知识。 */
QStringList launchTips();

/** 启动页那一句状态用什么词(阶段下标 -> 人话)。
 *
 *  为什么单独一个函数:核心库那 5 个阶段名里有两个是**技术词**("检测 Java 运行时" /
 *  "构建启动命令"),用户 2026-09-27 点名"命令行、Java 版本全去掉" —— 页面上只留人话。
 *  第 4 段("等待游戏")是用户点名要保留的那一句,谁改谁负责(单测按它断言)。 */
QString launchPhaseText(int index);

} // namespace sxcl::ui
