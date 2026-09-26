/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 已安装版本扫描(设计理由见 instance_scan.h)。

#include "instance_scan.h"

#include <cstring>

#include "sxcl/instance.h"

namespace sxcl::ui {

QVector<InstalledInstance> scanInstalledInstances(const QString &gameDir, QString *errorOut) {
    QVector<InstalledInstance> out;
    if (errorOut != nullptr)
        errorOut->clear();
    const QByteArray dir = gameDir.toUtf8();
    if (dir.isEmpty()) {
        if (errorOut != nullptr)
            *errorOut = QStringLiteral("游戏目录为空(还没选过目录)");
        return out;
    }
    sxcl_instance_list list;
    std::memset(&list, 0, sizeof(list));
    char err[SXCL_INSTANCE_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_instance_scan(dir.constData(), nullptr, &list, err, sizeof(err));
    if (rc != SXCL_INSTANCE_OK) {
        if (errorOut != nullptr)
            *errorOut = QString::fromUtf8(err[0] != '\0' ? err : "扫描本地实例失败");
        return out;
    }
    out.reserve(static_cast<int>(list.count));
    for (size_t i = 0; i < list.count; ++i) {
        const sxcl_instance &inst = list.items[i];
        InstalledInstance item;
        item.id = QString::fromUtf8(inst.id);
        item.type = QString::fromUtf8(inst.version_type[0] != '\0' ? inst.version_type : "release");
        item.summary = QString::fromUtf8(inst.summary);
        item.problem = QString::fromUtf8(inst.problem);
        item.launchable = inst.launchable != 0;
        item.hasJar = inst.has_jar != 0;
        item.problemCode = inst.problem_code;
        item.baseVersion = QString::fromUtf8(inst.base_version);
        item.baseReliable = inst.base_reliable != 0;
        for (size_t k = 0; k < inst.loader_count; ++k) {
            const sxcl_instance_loader &ld = inst.loaders[k];
            // 内层是 QStringList{kind_id, version} —— 与模型/委托的约定一致
            // (委托用 item.toStringList() 读它;写成 QVariantList 会让加载器小标签**静默消失**)
            item.loaders.append(QStringList{
                QString::fromUtf8(sxcl_instance_kind_id(ld.kind)),
                QString::fromUtf8(ld.version),
            });
        }
        out.append(item);
    }
    sxcl_instance_list_free(&list);
    return out;
}

} // namespace sxcl::ui
