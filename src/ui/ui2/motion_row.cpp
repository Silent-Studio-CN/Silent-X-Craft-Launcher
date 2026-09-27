/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "motion_row.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QStyle>
#include <QStyleOption>

namespace sxcl::ui2 {

MotionRow::MotionRow(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, true); /* QSS 的背景/圆角/悬停照旧生效 */
}

MotionRow::MotionRow(const QString &text, QWidget *parent) : MotionRow(parent) {
    setText(text);
}

void MotionRow::setReveal(qreal reveal) {
    const qreal r = reveal < 0.0 ? 0.0 : (reveal > 1.0 ? 1.0 : reveal);
    if (qFuzzyCompare(m_reveal, r))
        return;
    m_reveal = r;
    update(); /* 只重画这一行:没有布局、没有样式重算 */
}

void MotionRow::setText(const QString &text) {
    if (m_text == text)
        return;
    m_text = text;
    updateGeometry(); /* 文字变了才谈尺寸 */
    update();
}

void MotionRow::setLiftPx(int px) {
    m_lift = px < 0 ? 0 : px;
}

QSize MotionRow::sizeHint() const {
    const QFontMetrics fm(font());
    return QSize(fm.horizontalAdvance(m_text) + 24, qMax(30, fm.height() + 16));
}

QSize MotionRow::minimumSizeHint() const {
    return QSize(40, qMax(24, QFontMetrics(font()).height() + 8));
}

void MotionRow::paintEvent(QPaintEvent *event) {
    Q_UNUSED(event);
    /* 1) 背景走样式表(唯一的颜色来源:主题令牌 -> QSS);2) 文字是自绘的,带 reveal。 */
    QStyleOption opt;
    opt.initFrom(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);

    if (m_text.isEmpty() || m_reveal <= 0.002)
        return;

    QColor ink = palette().color(QPalette::WindowText); /* QSS 的 color 落在这里 */
    ink.setAlphaF(ink.alphaF() * m_reveal);

    const int inset = 10;
    const qreal dy = (1.0 - m_reveal) * static_cast<qreal>(m_lift);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(ink);
    p.setFont(font());
    p.drawText(QRectF(inset, dy, width() - 2.0 * inset, height() - dy),
               Qt::AlignLeft | Qt::AlignVCenter, m_text);
}

} // namespace sxcl::ui2
