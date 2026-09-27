/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "skin_image.h"

#include "theme_bridge.h"

#include <QColor>
#include <QPainter>
#include <QRectF>

namespace sxcl::ui {
namespace {

// 64x64(1.8 格式)里各部位的**正面**贴图区 —— 坐标是贴图自己的像素,不是界面的。
const QRect kHeadFront(8, 8, 8, 8);
const QRect kHeadLayer(40, 8, 8, 8); // 第二层(帽子)
const QRect kBodyFront(20, 20, 8, 12);
const QRect kArmRightFront(44, 20, 4, 12);
const QRect kArmLeftFront(36, 52, 4, 12);
const QRect kLegRightFront(4, 20, 4, 12);
const QRect kLegLeftFront(20, 52, 4, 12);

// 贴图上的"右手/右腿"区(旧格式里左手/左腿就是它们镜像出来的)
const QRect kArmMirrorSource(44, 20, 4, 12);
const QRect kArmMirrorTarget(36, 52, 4, 12);
const QRect kLegMirrorSource(4, 20, 4, 12);
const QRect kLegMirrorTarget(20, 52, 4, 12);

void blit(QPainter &p, const QImage &skin, const QRectF &dst, const QRect &src) {
    p.drawImage(dst, skin, QRectF(src));
}

} // namespace

QImage normalizedSkin(const QImage &skin) {
    if (skin.isNull())
        return QImage();
    QImage src = skin.convertToFormat(QImage::Format_ARGB32);
    // 高清皮肤(128x128 那类)先缩回官方网格:下面所有坐标都是按 64 网格写死的。
    if (src.width() != 64 && src.width() > 0) {
        src = src.scaled(64, src.height() * 64 / src.width(), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
    }
    if (src.width() != 64)
        return QImage();
    const bool legacy = src.height() == 32;
    if (!legacy && src.height() != 64)
        return QImage();

    QImage out(64, 64, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    {
        QPainter p(&out);
        p.drawImage(0, 0, src);
    }
    if (!legacy)
        return out;

    // 旧格式:左手/左腿没有自己的贴图区 —— 水平镜像右手/右腿补上(游戏里就是这么画的)。
    QPainter p(&out);
    p.drawImage(kArmMirrorTarget, src.copy(kArmMirrorSource).flipped(Qt::Horizontal));
    p.drawImage(kLegMirrorTarget, src.copy(kLegMirrorSource).flipped(Qt::Horizontal));
    return out;
}

QPixmap skinHeadPixmap(const QImage &skin, int side) {
    const QImage tex = normalizedSkin(skin);
    if (tex.isNull() || side <= 0)
        return QPixmap();
    QPixmap pm(side, side);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRectF box(0, 0, side, side);
    blit(p, tex, box, kHeadFront);
    blit(p, tex, box, kHeadLayer); // 第二层压在脸上(半透明像素照常透出底层)
    return pm;
}

QPixmap skinBodyPixmap(const QImage &skin, int height) {
    const QImage tex = normalizedSkin(skin);
    if (tex.isNull() || height <= 0)
        return QPixmap();
    // 模型比例(游戏自己那套,单位 = 皮肤像素):宽 16(臂4 + 身8 + 臂4)、高 32(头8 + 身12 + 腿12)
    const qreal u = height / 32.0;
    const int w = qMax(1, int(qRound(16 * u)));
    QPixmap pm(w, height);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    blit(p, tex, QRectF(4 * u, 8 * u, 8 * u, 12 * u), kBodyFront);
    blit(p, tex, QRectF(0 * u, 8 * u, 4 * u, 12 * u), kArmRightFront);
    blit(p, tex, QRectF(12 * u, 8 * u, 4 * u, 12 * u), kArmLeftFront);
    blit(p, tex, QRectF(4 * u, 20 * u, 4 * u, 12 * u), kLegRightFront);
    blit(p, tex, QRectF(8 * u, 20 * u, 4 * u, 12 * u), kLegLeftFront);
    blit(p, tex, QRectF(4 * u, 0 * u, 8 * u, 8 * u), kHeadFront);
    blit(p, tex, QRectF(4 * u, 0 * u, 8 * u, 8 * u), kHeadLayer);
    return pm;
}

QPixmap skinHeadPlaceholder(int side) {
    if (side <= 0)
        return QPixmap();
    QPixmap pm(side, side);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor ink = ThemeBridge::instance().token(QStringLiteral("textSecondary"));
    ink.setAlpha(190);
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    const qreal s = side;
    p.drawEllipse(QRectF(s * 0.28, s * 0.15, s * 0.44, s * 0.44)); // 头
    p.drawRoundedRect(QRectF(s * 0.14, s * 0.66, s * 0.72, s * 0.6), s * 0.34, s * 0.34); // 肩
    return pm;
}

QPixmap skinBodyPlaceholder(int height) {
    if (height <= 0)
        return QPixmap();
    const qreal u = height / 32.0;
    const int w = qMax(1, int(qRound(16 * u)));
    QPixmap pm(w, height);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QColor ink = ThemeBridge::instance().token(QStringLiteral("textSecondary"));
    ink.setAlpha(150);
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    // 就是一个剪影：比例与真模型一致(头 8 / 身 12 / 腿 12)，不写字。
    p.drawRoundedRect(QRectF(4 * u, 8 * u, 8 * u, 12 * u), 2 * u, 2 * u);
    p.drawRoundedRect(QRectF(0 * u, 8 * u, 4 * u, 12 * u), 1.6 * u, 1.6 * u);
    p.drawRoundedRect(QRectF(12 * u, 8 * u, 4 * u, 12 * u), 1.6 * u, 1.6 * u);
    p.drawRoundedRect(QRectF(4 * u, 20 * u, 4 * u, 12 * u), 1.6 * u, 1.6 * u);
    p.drawRoundedRect(QRectF(8 * u, 20 * u, 4 * u, 12 * u), 1.6 * u, 1.6 * u);
    p.drawRoundedRect(QRectF(4 * u, 0 * u, 8 * u, 8 * u), 2 * u, 2 * u);
    return pm;
}

} // namespace sxcl::ui
