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
    int problemCode = 0;
    QString baseVersion;   // 它继承的原版(核心库给;baseReliable=0 时别当权威显示)
    bool baseReliable = false;
    QVariantList loaders;  // QVariantList<QStringList{kind_id, version}>,与模型/委托的约定一致
};

/** 扫 <gameDir>/versions。**阻塞**;失败时填 errorOut 并返回空表。
 *  (调用方自己决定空表是"没装版本"还是"扫不出来" —— 看 errorOut 是不是空。) */
QVector<InstalledInstance> scanInstalledInstances(const QString &gameDir, QString *errorOut);

} // namespace sxcl::ui
