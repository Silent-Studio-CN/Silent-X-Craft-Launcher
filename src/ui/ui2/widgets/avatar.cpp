/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "avatar.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>

#include "tokens.h"

namespace sxcl::ui2 {

AvatarWidget::AvatarWidget(QWidget *parent) : QWidget(parent) {
    setFixedSize(56, 56);
}

void AvatarWidget::setName(const QString &name) {
    m_name = name;
    update();
}

void AvatarWidget::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Tokens &t = tokens();
    // 圆底:强调色压到很低的一档 —— 装饰不用强调色(§1),这里只是给字母一块底色
    QColor base = tokenColor(t.surfaceHover);
    QPainterPath circle;
    circle.addEllipse(QRectF(0, 0, width(), height()));
    p.fillPath(circle, base);

    QString letter = m_name.trimmed().left(1).toUpper();
    if (letter.isEmpty())
        letter = QStringLiteral("?");
    QFont f = font();
    f.setPixelSize(20);
    f.setBold(true);
    p.setFont(f);
    p.setPen(tokenColor(t.text));
    p.drawText(rect(), Qt::AlignCenter, letter);
}

} // namespace sxcl::ui2
