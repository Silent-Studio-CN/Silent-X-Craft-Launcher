/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "icons.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPainter>
#include <QSvgRenderer>

#include <cstdio>

#ifndef SXCL_UI2_ICON_DIR
#define SXCL_UI2_ICON_DIR ""
#endif

namespace sxcl::ui2 {
namespace {

QString &iconDirSlot() {
    static QString dir;
    return dir;
}

QString resolveIconDir() {
    const QString compiled = QString::fromUtf8(SXCL_UI2_ICON_DIR);
    if (!compiled.isEmpty() && QFileInfo::exists(compiled))
        return compiled;
    // 打包后(源码树不在机器上):exe 旁边的 assets/icons/ui —— 与老界面主题资产同一条回退纪律。
    const QString beside = QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("assets/icons/ui"));
    if (QFileInfo::exists(beside))
        return beside;
    return compiled;
}

QHash<QString, QPixmap> &cache() {
    static QHash<QString, QPixmap> map;
    return map;
}

} // namespace

QString uiIconPath(const QString &file) {
    if (iconDirSlot().isEmpty())
        iconDirSlot() = resolveIconDir();
    return QDir(iconDirSlot()).filePath(file);
}

QPixmap iconPixmap(const QString &file, int size, const QColor &tint, qreal dpr) {
    if (size <= 0)
        return QPixmap();
    if (dpr <= 0.0)
        dpr = 1.0;
    const QString path = uiIconPath(file);
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(path)
                            .arg(size)
                            .arg(tint.isValid() ? tint.name(QColor::HexArgb) : QStringLiteral("-"))
                            .arg(dpr);
    const auto hit = cache().constFind(key);
    if (hit != cache().constEnd())
        return hit.value();

    QPixmap pm(qRound(size * dpr), qRound(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QSvgRenderer renderer(path);
    if (!renderer.isValid()) {
        std::fprintf(stderr, "[sxcl-ui2] 图标缺失: %s(界面用空白图标继续跑)\n",
                     path.toUtf8().constData());
        return QPixmap();
    }
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        renderer.render(&p, QRectF(0, 0, size, size));
        if (tint.isValid()) {
            // 单色线稿:用画出来的 alpha 当蒙版整枚染色(素材本身的颜色不参与)。
            p.setCompositionMode(QPainter::CompositionMode_SourceIn);
            p.fillRect(QRectF(0, 0, size, size), tint);
        }
    }
    cache().insert(key, pm);
    return pm;
}

} // namespace sxcl::ui2
