/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 「已安装版本」扫描的界面层唯一实现(核心库 sxcl_instance_scan 的包装)。
//
// 为什么要单独一份(以前版本页/版本选择页各写一遍,而且**都跑在界面线程上**):
//   * 扫描是**磁盘活**(每个 <gameDir>/versions/<id>/ 都要看 JSON/jar,几十上百个实例时
//     几百毫秒起),界面线程上做它 = 用户看到"未响应";
//   * 判据必须只有一处(Python scan_installed 的 C 版口径:versions/<id>/ 下只要有版本 JSON
//     就算装好了 —— Forge 1.13+/Fabric 的实例没有自己的 jar),两页写两份迟早走岔。
//
// **只能在界面层的 worker(BgTask/工作线程)里调** —— 界面线程一次都不许直接调它。
#pragma once

#include <QString>
#include <QVariantList>
#include <QVector>

namespace sxcl::ui {

struct InstalledInstance {
    QString id;
    QString type;      // release / snapshot / old_beta …(空 = release)
    QString summary;   // "原版" / "Forge 47.2.0 + OptiFine I6"
    QString problem;   // 不能启动时的**人话**原因(空 = 没问题)
    bool launchable = true;
    bool hasJar = true;
    bool hasJson = true;   // 有没有可用(可解析)的版本 JSON(核心库 has_json)
    int problemCode = 0;
    QString baseVersion;   // 它继承的原版(核心库给;baseReliable=0 时别当权威显示)
    bool baseReliable = false;
    QString jsonId;        // 版本 JSON 里写的 id(可能和目录名不同 —— 那正是 id 冲突那条判据)
    QString inheritsFrom;  // JSON 的 inheritsFrom(空 = 没有继承)
    QString missingParent; // 缺哪个前置版本(空 = 不缺)
    QString jsonPath;      // 实际读到的那份版本 JSON 的完整路径(空 = 没有)
    QVariantList loaders;  // QVariantList<QStringList{kind_id, version}>,与模型/委托的约定一致
};

/** 版本行(版本选择页的一张卡 / 版本页的一行)**唯一一份**展示口径。
 *
 * 为什么要有它:同一个实例以前两页各拼一次文案,想改一句话要改两处,改漏一处就是
 * "两页说法不一样"。现在取数在核心库、**摆法在这里**,两页只负责画。
 *
 * 三条硬口径(逐条对应用户 2026-09-26 的原话):
 *   1) 状态图标只有两个:能启动 = 草方块(grass),不能启动 = 我们自己画的警告符(warn)。
 *      **不用 emoji**,也**不用 PCL 的红石块**(用户:"显得有点太雷同了")。
 *   2) 行内只放**一句话 + 一个动作**:问题一句话(note)、动作词(action = "去下载"),
 *      完整原因(reason)与路径(path)只进 tooltip(tip)—— 主界面上不倒报错。
 *   3) 猜出来的版本号**一个字都不写**(base 空、info 里也就不出现"原版 x"):
 *      只有**版本文件里写着的**才认(核心库 base_reliable,字段顺序见
 *      instance.c:920-984:clientVersion -> patches[game].version -> inheritsFrom ->
 *      --fml.mcVersion -> jar;核心库认不出时,再看 JSON 自己的 id —— 但与目录名相同的
 *      id 不算新信息,那是"按目录名猜",照旧不写)。 */
struct VersionRowInfo {
    QString state;     // "grass" / "warn"(ui_icons.h 的 version_state 两个 id)
    QString info;      // 能启动行的信息行:"原版 1.12.2" / "Forge 47.2.0 · 原版 1.12.2";空 = 一个字都不写
    QString note;      // 不能启动行的那一句话(能启动时空)
    QString action;    // 不能启动行的动作词(能启动时空)
    QString reason;    // 核心库给的完整人话原因(能启动时空)
    QString path;      // 这个实例的目录 <gameDir>/versions/<id>
    QString tip;       // tooltip 全文(多行;详细原因与路径只在这里)
    QString base;      // 认出来的原版版本号;空 = **不写**(既不写"未知"、也不写"猜")
    QString baseFrom;  // core:inheritsFrom / core:json-other / json:id / none —— 取证用
    bool coreReliable = false; // 核心库 base_reliable 原值(取证用,别拿它当"能不能显示"的开关)
};

/** 把一个实例算成一行要显示的东西(纯函数,不碰磁盘;两页共用)。 */
VersionRowInfo versionRowInfo(const InstalledInstance &inst, const QString &gameDir);

/** 扫 <gameDir>/versions。**阻塞**;失败时填 errorOut 并返回空表。
 *  (调用方自己决定空表是"没装版本"还是"扫不出来" —— 看 errorOut 是不是空。) */
QVector<InstalledInstance> scanInstalledInstances(const QString &gameDir, QString *errorOut);

} // namespace sxcl::ui
