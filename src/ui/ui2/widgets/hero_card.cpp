/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "hero_card.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include "icon_button.h"
#include "icons.h"

namespace sxcl::ui2 {

HeroCard::HeroCard(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sxcl2HeroCard"));
    setAttribute(Qt::WA_StyledBackground, true); // QSS 的圆角/底色要真的画出来

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(24, 20, 24, 20);
    v->setSpacing(10);

    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    m_caption = new QLabel(QStringLiteral("当前版本"), this);
    m_caption->setObjectName(QStringLiteral("sxcl2HeroCaption"));
    top->addWidget(m_caption);
    top->addStretch(1);
    m_gear = new IconButton(uiIconPath(QStringLiteral("gear.svg")), 18, this);
    m_gear->setObjectName(QStringLiteral("sxcl2HeroGear"));
    m_gear->setToolTip(QStringLiteral("版本设置"));
    m_gear->setVisible(false); // 平时不占地方(§11.1)
    connect(m_gear, &IconButton::clicked, this, &HeroCard::gearRequested);
    top->addWidget(m_gear);
    v->addLayout(top);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("sxcl2HeroTitle"));
    v->addWidget(m_title);

    m_meta = new QLabel(this);
    m_meta->setObjectName(QStringLiteral("sxcl2HeroMeta"));
    v->addWidget(m_meta);

    auto *row = new QHBoxLayout;
    row->setSpacing(12);
    m_launch = new QToolButton(this);
    m_launch->setObjectName(QStringLiteral("sxcl2Primary"));
    m_launch->setText(QStringLiteral("启动游戏"));
    m_launch->setCursor(Qt::PointingHandCursor);
    connect(m_launch, &QToolButton::clicked, this, &HeroCard::launchRequested);
    row->addWidget(m_launch);

    m_swap = new QToolButton(this);
    m_swap->setObjectName(QStringLiteral("sxcl2Ghost"));
    m_swap->setText(QStringLiteral("切换版本"));
    m_swap->setCursor(Qt::PointingHandCursor);
    connect(m_swap, &QToolButton::clicked, this, &HeroCard::swapRequested);
    row->addWidget(m_swap);
    row->addStretch(1);
    v->addLayout(row);

    setVersion(QStringLiteral("—"), QStringLiteral("正在读取已安装的版本…"), false);
}

void HeroCard::setVersion(const QString &name, const QString &meta, bool launchable) {
    m_title->setText(name);
    m_meta->setText(meta);
    m_launch->setEnabled(launchable);
}

void HeroCard::enterEvent(QEnterEvent *event) {
    if (m_gear != nullptr)
        m_gear->setVisible(true);
    QWidget::enterEvent(event);
}

void HeroCard::leaveEvent(QEvent *event) {
    if (m_gear != nullptr)
        m_gear->setVisible(false);
    QWidget::leaveEvent(event);
}

} // namespace sxcl::ui2
