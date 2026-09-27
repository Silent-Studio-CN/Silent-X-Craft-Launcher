/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

// 玩家皮肤贴图 -> 界面要的两张图:头像(头)与全身正面像。
//
// 贴图格式(与 FCL / 官方启动器同一套,不是我们编的):
//   * 1.8 格式 64x64:左手/左腿各自有独立贴图区(36,52) / (20,52);
//   * 旧格式   64x32:**没有**左手/左腿区,它们由右手(44,20)/右腿(4,20)**水平镜像**得到;
//   * 64x64 之上还有第二层("帽子"/外套):头部在 (40,8),画在原图之上。
//
// 这一层只做"把贴图上该抠的那几块拼成一张图",不联网、不碰磁盘 —— 取图那一段在
// skin_store.h(它才碰网络与缓存)。所以纯函数、可单测。

#include <QImage>
#include <QPixmap>

namespace sxcl::ui {

/** 皮肤贴图规整成 64x64:旧格式(64x32)的左手/左腿由右手/右腿镜像补上。
 *  返回空图 = 这张图不是能认的皮肤(尺寸对不上)。 */
QImage normalizedSkin(const QImage &skin);

/** 头(带第二层帽子)。side = 逻辑像素边长。皮肤为空 -> 返回空图。 */
QPixmap skinHeadPixmap(const QImage &skin, int side);

/** 全身正面像:头 + 身体 + 双臂 + 双腿(比例 = 游戏里那套 8/12/12)。
 *  height = 逻辑像素总高(头顶到脚底)。皮肤为空 -> 返回空图。 */
QPixmap skinBodyPixmap(const QImage &skin, int height);

/** 拿不到皮肤时的占位(**不写字**):一个剪影。 */
QPixmap skinHeadPlaceholder(int side);
QPixmap skinBodyPlaceholder(int height);

} // namespace sxcl::ui
