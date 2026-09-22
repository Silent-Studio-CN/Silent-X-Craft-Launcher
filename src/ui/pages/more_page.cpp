/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 更多页(用户 2026-09-22 口述):原来的"按键映射/任务"这类页面从侧边栏收进这里。
// 用户原话:「原来的手机键位归到"更多"，同时因为电脑端用户通常不用，
//            但是我们支持端游制作键位，一键复制键位码。可以导入」。

#include "page_factory.h"
#include "page_shell.h"

#include "icon_registry.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "fluent/fluent_cards.h"
#include "fluent/fluent_controls.h"
#include "fluent/fluent_labels.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QHBoxLayout>
#include <QMetaObject>
#include <QVBoxLayout>
#include <QWidget>

namespace sxcl::ui {
namespace {

// 让主窗口换页:与各页里"返回"那套同一个写法(直接调 MainWindow::switchToRoute)。
void goRoute(QWidget *host, const QString &route) {
    QMetaObject::invokeMethod(host->window(), "switchToRoute", Qt::DirectConnection,
                              Q_ARG(QString, route));
}

// 一行入口:图标 + 标题 + 一句说明 + 右侧"打开"。
CardWidget *buildEntryCard(QWidget *host, IconRegistry::Semantic icon, const QString &title,
                           const QString &body, const QString &routeClickable, QWidget *parent) {
    auto *card = new CardWidget(parent);
    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(20, 16, 20, 16);
    row->setSpacing(16);

    auto *iconLabel = new QLabel(card);
    iconLabel->setPixmap(IconRegistry::instance().themedIcon(icon).pixmap(28, 28));
    iconLabel->setFixedSize(28, 28);
    row->addWidget(iconLabel, 0, Qt::AlignVCenter);

    auto *text = new QVBoxLayout();
    text->setSpacing(4);
    auto *head = new StrongBodyLabel(title, card);
    text->addWidget(head);
    auto *desc = new BodyLabel(body, card);
    desc->setWordWrap(true);
    const QColor secondary = pageTokenColor("textSecondary");
    desc->setTextColor(secondary, secondary);
    text->addWidget(desc);
    row->addLayout(text, 1);

    auto *open = new PushButton(QStringLiteral("打开"), card);
    QObject::connect(open, &QPushButton::clicked, card,
                     [host, routeClickable] { goRoute(host, routeClickable); });
    row->addWidget(open, 0, Qt::AlignVCenter);
    return card;
}

} // namespace

QWidget *createMorePage(QWidget *parent) {
    auto *page = new PageShell(QStringLiteral("更多"), QStringLiteral("按键映射 · 任务 · 取证 · 关于"),
                               QStringLiteral("sxclPage_more"), parent);

    page->addContent(buildEntryCard(
        page, IconRegistry::Personalize, QStringLiteral("按键映射"),
        QStringLiteral("手机端能用的触屏键位；电脑端一般用不上，但**端游也能自己做一套**，"
                       "做好了一键复制键位码发给别人、别人导入即用。"),
        QStringLiteral("keymap"), page->view()));

    page->addContent(buildEntryCard(
        page, IconRegistry::Tasks, QStringLiteral("下载任务"),
        QStringLiteral("正在跑与跑完的下载/安装任务：阶段、进度、速度、取消。"),
        QStringLiteral("tasks"), page->view()));

    page->addContent(buildEntryCard(
        page, IconRegistry::Help, QStringLiteral("崩溃与日志"),
        QStringLiteral("游戏退出后的取证结论与日志导出（崩溃报告 / latest.log / 启动器日志）。"
                       "入口在「设置 → 日志」，这里给个快捷跳转。"),
        QStringLiteral("settings"), page->view()));

    page->addStretch();
    return page;
}

} // namespace sxcl::ui
