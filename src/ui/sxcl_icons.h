/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include <QHash>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace sxcl::ui {

class SxclIcons {
public:
    static SxclIcons &instance();

    // 资产目录:编译期 SXCL_UI_BLOCK_DIR -> 环境变量 SXCL_BLOCK_DIR -> exe 旁 assets/icons/blocks
    void setBlockDir(const QString &dir);
    QString blockDir() const;
    bool resolveBlockDir();

    bool hasAsset(const QString &kind) const;
    static QStringList kinds();

    // 逻辑尺寸 size,内部按 DPR 放大
    QPixmap blockPixmap(const QString &kind, int size = 24);
    QIcon blockIcon(const QString &kind, int size = 24);
    QPixmap grassBlockPixmap(int size = 24);
    QIcon grassBlockIcon(int size = 24);

    // 状态 -> 图标种类(原版/快照/旧版/各加载器/出错)
    static QString stateIconKind(const QString &versionType, const QStringList &loaders, bool broken = false);
    // kind -> PNG 文件名(对应 Python BLOCK_FILES)
    static QString blockFile(const QString &kind);

private:
    SxclIcons() = default;
    QPixmap source(const QString &kind); // 原图,带缓存

    QString m_dir;
    QHash<QString, QPixmap> m_sourceCache;
    QHash<QString, QPixmap> m_scaledCache;
};

} // namespace sxcl::ui
