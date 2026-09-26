/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 界面自绘小图标的实现（理由见 ui_icons.h）。

#include "ui_icons.h"

#include "fluent_theme.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QSvgRenderer>

namespace sxcl::ui {
namespace {

/** 资产根目录：编译期钉死的 assets/icons/blocks 往上一层就是 assets/icons。 */
QString uiIconPath(const QString &file) {
    static QString dir;
    if (dir.isEmpty()) {
#ifdef SXCL_UI_BLOCK_DIR
        QString blocks = QString::fromLatin1(SXCL_UI_BLOCK_DIR); // .../assets/icons/blocks
        const int cut = blocks.lastIndexOf(QLatin1Char('/'));
        dir = (cut > 0 ? blocks.left(cut) : blocks) + QStringLiteral("/ui");
#else
        dir = QDir(QGuiApplication::applicationDirPath()).filePath(QStringLiteral("assets/icons/ui"));
#endif
    }
    return dir + QLatin1Char('/') + file;
}

qreal dpr() {
    if (const QScreen *s = QGuiApplication::primaryScreen())
        return s->devicePixelRatio();
    return 1.0;
}

/** 读一份 assets/icons/ui/*.svg，把 currentColor 换成本次要用的颜色。 */
QByteArray uiSvgBytes(const QString &file, const QColor &color) {
    QFile handle(uiIconPath(file));
    if (!handle.open(QIODevice::ReadOnly))
        return QByteArray();
    QByteArray bytes = handle.readAll();
    const QByteArray hex = color.name(QColor::HexRgb).toLatin1();
    bytes.replace("currentColor", hex);
    return bytes;
}

/** 把一份 SVG 按逻辑尺寸画成位图（DPR 参与，缩放后不糊）。 */
QPixmap uiSvgPixmap(const QByteArray &svg, int size) {
    if (svg.isEmpty() || size <= 0)
        return QPixmap();
    const qreal scale = dpr();
    QPixmap pm(int(size * scale), int(size * scale));
    pm.setDevicePixelRatio(scale);
    pm.fill(Qt::transparent);
    QSvgRenderer renderer(svg);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&p, QRectF(0, 0, size, size));
    return pm;
}

} // namespace

QByteArray uiWarningSvg(const QColor &color) {
    return uiSvgBytes(QStringLiteral("warning.svg"), color);
}

QPixmap uiWarningPixmap(int size, const QColor &color) {
    return uiSvgPixmap(uiWarningSvg(color), size);
}

QByteArray uiCrossSvg(const QColor &color) {
    return uiSvgBytes(QStringLiteral("cross.svg"), color);
}

QPixmap uiCrossPixmap(int size, const QColor &color) {
    return uiSvgPixmap(uiCrossSvg(color), size);
}

QIcon uiCrossIcon(int size, const QColor &color) {
    return QIcon(uiCrossPixmap(size, color));
}

QPixmap uiBedrockLogoPixmap(int height) {
    if (height <= 0)
        return QPixmap();
    static QPixmap src;
    if (src.isNull()) {
        QString blocks = QString::fromLatin1(
#ifdef SXCL_UI_BLOCK_DIR
            SXCL_UI_BLOCK_DIR
#else
            ""
#endif
        );
        const int cut = blocks.lastIndexOf(QLatin1Char('/'));
        const QString dir = (cut > 0 ? blocks.left(cut) : blocks) + QStringLiteral("/bedrock");
        src.load(dir + QStringLiteral("/title.png"));
    }
    if (src.isNull())
        return QPixmap();
    const qreal scale = dpr();
    const int w = qMax(1, int(qRound(double(src.width()) * height / double(src.height()))));
    QPixmap pm(int(w * scale), int(height * scale));
    pm.setDevicePixelRatio(scale);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawPixmap(QRect(0, 0, w, height), src);
    return pm;
}

QIcon uiWarningIcon(int size) {
    // 主题警告色（跟随明暗主题；画的时候取，主题一换图标就换）
    const QColor warning = FluentTheme::instance().tokens().warning;
    return QIcon(uiWarningPixmap(size, warning));
}

} // namespace sxcl::ui
