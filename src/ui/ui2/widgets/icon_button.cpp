/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "icon_button.h"

#include <QPainter>
#include <QPen>

#include "icons.h"
#include "tokens.h"

namespace sxcl::ui2 {

IconButton::IconButton(const QString &iconFile, int glyphSize, QWidget *parent)
    : QAbstractButton(parent), m_file(iconFile), m_glyph(glyphSize > 0 ? glyphSize : 20) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus); // 键盘可达(§4):Tab 能到、空格/回车能按
    setFixedSize(sizeHint());
}

void IconButton::setTint(Tint tint) {
    m_tint = tint;
    update();
}

void IconButton::setSelected(bool on) {
    if (m_selected == on)
        return;
    m_selected = on;
    update();
}

QSize IconButton::sizeHint() const { return QSize(m_glyph + 12, m_glyph + 12); }

QColor IconButton::tintColor() const {
    if (m_tint == Tint::Original)
        return QColor(); // 无效色 = 保留素材本色
    return tokenColor(m_selected ? tokens().accent : tokens().text2);
}

void IconButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    if (m_selected) {
        // 选中 = 主题令牌的 accent 环(颜色现取,构造期一个色都不烤)
        QPen pen(tokenColor(tokens().accent), 2.0);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box, 8, 8);
    } else if (m_hover) {
        p.setPen(Qt::NoPen);
        p.setBrush(tokenColor(tokens().surfaceHover));
        p.drawRoundedRect(box, 8, 8);
    }
    const QPixmap pm = iconPixmap(m_file, m_glyph, tintColor(), devicePixelRatioF());
    if (!pm.isNull()) {
        const QPointF topLeft((width() - m_glyph) / 2.0, (height() - m_glyph) / 2.0);
        p.drawPixmap(topLeft, pm);
    }
}

void IconButton::enterEvent(QEnterEvent *event) {
    m_hover = true;
    update();
    QAbstractButton::enterEvent(event);
}

void IconButton::leaveEvent(QEvent *event) {
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(event);
}

/* ── 只画一枚图标的小控件 ─────────────────────────────────────────────── */

IconGlyph::IconGlyph(const QString &iconFile, int size, QWidget *parent)
    : QWidget(parent), m_file(iconFile), m_size(size > 0 ? size : 16) {
    setFixedSize(m_size, m_size);
}

QSize IconGlyph::sizeHint() const { return QSize(m_size, m_size); }

void IconGlyph::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const QPixmap pm = iconPixmap(m_file, m_size, tokenColor(tokens().text2), devicePixelRatioF());
    if (!pm.isNull())
        p.drawPixmap(0, 0, pm);
}

} // namespace sxcl::ui2
