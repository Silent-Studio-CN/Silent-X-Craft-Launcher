/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "notice_card.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "icon_button.h"
#include "icons.h"

namespace sxcl::ui2 {

NoticeCard::NoticeCard(const QStringList &lines, QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sxcl2NoticeCard"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(24, 18, 24, 18);
    v->setSpacing(8);

    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    auto *title = new QLabel(QStringLiteral("公告"), this);
    title->setObjectName(QStringLiteral("sxcl2NoticeTitle"));
    top->addWidget(title);
    top->addStretch(1);
    // 「查看全部」= 一枚箭头图标(不是按钮:这张卡上没有按钮,§11.4)
    auto *more = new IconGlyph(uiIconPath(QStringLiteral("arrow_right.svg")), 16, this);
    more->setObjectName(QStringLiteral("sxcl2NoticeMore"));
    more->setToolTip(QStringLiteral("查看全部公告"));
    top->addWidget(more);
    v->addLayout(top);

    auto *body = new QLabel(this);
    body->setObjectName(QStringLiteral("sxcl2NoticeBody"));
    body->setWordWrap(true);
    // 只露两三行:完整公告在"查看全部"那边(§11.4)
    QStringList shown = lines;
    if (shown.size() > 3)
        shown = shown.mid(0, 3);
    body->setText(shown.join(QLatin1Char('\n')));
    v->addWidget(body);
}

} // namespace sxcl::ui2
