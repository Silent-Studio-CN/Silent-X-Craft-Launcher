/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 版本选择页的**排序与分组**规则(纯函数,不碰磁盘 —— 单测直接调它)。
//
// 用户 2026-09-27 原话(两轮):
//   「关于版本选择,按照 MC 的版本号排,哪个是最新版哪个就放最前面。这个什么逻辑,你自己写。」
//   「错误版不参与大小排序,错误版自己单列一个,错误版在下面按再按照版本号排序。」
//
// 所以这里只有两条规则:
//   1) 能启动的按 **MC 版本号**从新到旧(不是按目录名、不是按安装时间、不是按字母);
//   2) 残缺/起不来的**单独一组放到最下面**,组内仍旧按版本号从新到旧 —— 它们不参与上面那组的排序
//      (把 114514 这种目录名混进版本号队列里,会把整列的顺序搅乱)。
//
// 版本号从哪来:instance_scan 的核心库口径(instance.c 的 base_version/base_reliable)已经
// 把"版本文件里写着的那个原版号"取出来了 —— 这里只认它,认不出来才退到目录名。
// **绝不猜**(用户 2026-09-26 就点名过"认不出来就什么都不写")。
#pragma once

#include <QString>
#include <QVector>

#include "instance_scan.h"

namespace sxcl::ui {

/** 一个 MC 版本号的可比较形态。
 *
 * parts 是**逐段数字**的比较向量(从左到右,第一个不相等的段定胜负),段不够补 0:
 *   1.21.4        -> [0, 1, 21, 4, 3, 0]      (0 = 现代正式版这一纪;3 = 正式版档)
 *   1.21.4-rc1    -> [0, 1, 21, 4, 2, 1]      (候选版排在同名正式版**下面**)
 *   1.21.4-pre2   -> [0, 1, 21, 4, 1, 2]      (预览版再下面)
 *   26.3          -> [0, 2026, 3, 3, 0]       (年份式命名换算到 2000+年份,与快照同一根轴)
 *   25w14a        -> [0, 2025, 14, 1]         (快照:年 + 周 + 字母)
 *   b1.7.3        -> [-1, 1, 7, 3, 3, 0]      (Beta 排在所有 1.x 之前)
 * 只有"看着像版本号"的才认(必须带小数点,或是快照/老版本前缀):目录名 114514、424242
 * 这类**不认** —— known=false 的那些排在认得出的后面,组内按名字倒序。 */
struct McVersionKey {
    bool known = false; /**< false = 这不是一个版本号 */
    QVector<int> parts; /**< 逐段比较用(见上) */
    QString text;       /**< 原始文本(取证/排错用,不参与比较) */
};

/** 解析一个版本号。认不出来时返回 known=false 的空键(**不抛、不猜**)。 */
McVersionKey parseMcVersion(const QString &text);

/** 两个版本号的大小:>0 = a 比 b 新,<0 = a 比 b 旧,0 = 一样。 */
int compareMcVersion(const McVersionKey &a, const McVersionKey &b);

/** 两段版本文本的大小(先解析再比;都认不出来时按名字倒序,保证顺序**确定**)。 */
int compareVersionText(const QString &a, const QString &b);

/** 版本选择页要显示的全部行:正常的一组 + 残缺/错误的一组(各组内部都已经排好序)。 */
struct VersionDisplay {
    QVector<InstalledInstance> healthy; /**< 能启动的,MC 版本号从新到旧 */
    QVector<InstalledInstance> broken;  /**< 残缺/错误的,组内同样从新到旧 */
};

/** 把一次扫描结果切成"正常一组 + 有问题一组",两组各自按版本号从新到旧排好。 */
VersionDisplay orderInstancesForDisplay(const QVector<InstalledInstance> &instances);

/** 一个实例用来排序的版本号:核心库认出来的原版号 > 目录名(**不猜**)。
 *  空串 = 既没有版本文件里写的号,目录名也不像版本号。 */
QString instanceSortKey(const InstalledInstance &inst);

} // namespace sxcl::ui
