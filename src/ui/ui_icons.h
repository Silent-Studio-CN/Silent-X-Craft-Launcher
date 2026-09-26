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

} // namespace sxcl::ui
