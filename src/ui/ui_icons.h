/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 界面自己画的小图标（**不借第三方资产**，全部是我们手写的 SVG）。
//
// 为什么要有它：用户 2026-09-22 晚点名 ——「又出现了黄色感叹号 emoji 去掉。改成 svg」。
// 以前界面里有几处直接把 "⚠" 当文字画/贴（版本行的"不能启动"提示、键位冲突列表、
// 下载配置页的提醒……），那玩意儿在不同字体下大小颜色都不受控，还带着 emoji 的观感。
// 现在统一走这里的 QIcon / 绘制字节：颜色取当前主题（warning 色），尺寸由调用方定。
#pragma once

#include <QByteArray>
#include <QColor>
#include <QIcon>
#include <QPixmap>

namespace sxcl::ui {

/** 警示三角的原始 SVG 字节（currentColor 已替换成 color）。 */
QByteArray uiWarningSvg(const QColor &color);
/** 警示三角位图（给"画上去"的场合用，如自绘的行内 chip）。 */
QPixmap uiWarningPixmap(int size /*逻辑像素*/, const QColor &color);
/** 警示三角图标（给 QListWidgetItem::setIcon 这类要 QIcon 的场合用）。 */
QIcon uiWarningIcon(int size);

/** 叉号（错误标记）—— 同样是我们手写的 SVG（assets/icons/ui/cross.svg）。
 *  起因：键位页的错误行以前拿 "✗ " 这个**字符**当图标（用户 2026-09-26：emoji/符号全换成 SVG）。
 *  颜色由调用方给（错误用 danger 令牌），尺寸按逻辑像素。 */
QByteArray uiCrossSvg(const QColor &color);
QPixmap uiCrossPixmap(int size, const QColor &color);
QIcon uiCrossIcon(int size, const QColor &color);

/** 基岩版标识（assets/icons/bedrock/title.png，从用户自己那份 APK 里取出来的）。
 *  按给定高度等比缩放（原图 1937x333 的长条 wordmark）。取不到时返回空图，调用方自己兜底。 */
QPixmap uiBedrockLogoPixmap(int height);

/** ── 版本行的状态标记（**一套两个状态**，版本页与版本选择页共用）────────────────
 *
 * 用户 2026-09-26 原话：「PCL 的采取方式是不正常的版本用左侧放一个红石块来展示，但咱们也
 * 这样显得有点太雷同了，我推荐的是在不使用 emoji 的情况下使用 SVG，或者你自己画图标：
 * ……以及正常版本能启动的用草方块。」所以：
 *   * grass = 草方块（assets/icons/blocks/Grass.png，与 Python 版/PCL 的"原版"同一个方块素材）
 *   * warn  = **我们自己画的**实心圆角警告三角（assets/icons/ui/version_warn.svg，
 *             感叹号是 fill-rule="evenodd" 挖出来的洞），颜色取调用方给的令牌（版本行取
 *             danger = 红）。**不用 emoji，也不用 PCL 的红石块**（"太雷同"那条点名要避开的）。
 *
 * 为什么只有两个状态：核心库 sxcl_instance_scan 的判据就是一个布尔 launchable + 一个
 * problem_code（instance.c:1413-1427），没有"能启动但有隐患"的第三态；硬造一个黄色警告符
 * 去标"能正常启动"的版本只是噪音（详见最终报告）。 */
namespace version_state {
/** 能正常启动（草方块）。 */
extern const char *const kGrass;
/** 不能启动（红色警告符）。 */
extern const char *const kWarn;
} // namespace version_state

/** 状态标记位图（size = 逻辑像素）。state 取 version_state 那两个 id 之一；认不出返回空图。 */
QPixmap uiVersionStatePixmap(const char *state, int size);
/** 同上，给要 QIcon 的场合用。颜色在**调用时**按当前主题令牌现取（切主题跟着变）。 */
QIcon uiVersionStateIcon(const char *state, int size);

} // namespace sxcl::ui
