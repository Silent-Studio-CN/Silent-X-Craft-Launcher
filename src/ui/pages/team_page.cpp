/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 团队页(用户 2026-09-22 口述的侧边栏第 3 项)。**语义待用户定义** ——
// 见 docs/25-界面重构规格.md §5:三种可能(账号/团队、团队服联机分组、团队实例共享),
// 现在只把位置占住,把三种可能摆在页面上,不做任何实现(不假装功能存在)。

#include "page_shell.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "fluent/fluent_cards.h"
#include "fluent/fluent_labels.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QWidget>

namespace sxcl::ui {
namespace {

CardWidget *buildOptionCard(const QString &index, const QString &title, const QString &body,
                           QWidget *parent) {
    auto *card = new CardWidget(parent);
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(20, 16, 20, 16);
    lay->setSpacing(6);
    auto *head = new StrongBodyLabel(QStringLiteral("%1 · %2").arg(index, title), card);
    head->setStyleSheet(QStringLiteral("color: %1; font-size: 15px;")
                            .arg(pageTokenText("accent")));
    lay->addWidget(head);
    auto *text = new BodyLabel(body, card);
    text->setWordWrap(true);
    const QColor secondary = pageTokenColor("textSecondary");
    text->setTextColor(secondary, secondary);
    lay->addWidget(text);
    return card;
}

} // namespace

QWidget *createTeamPage(QWidget *parent) {
    auto *page = new PageShell(QStringLiteral("团队"), QStringLiteral("位置先占住 · 语义待定"),
                               QStringLiteral("sxclPage_team"), parent);

    auto *note = new BodyLabel(
        QStringLiteral("这个页面是你 2026-09-22 点名的侧边栏第 3 项，但「团队」具体指什么还没定。"
                       "下面三种是常见的解释，选一个（或者说个别的），我再照着做。"),
        page->view());
    note->setWordWrap(true);
    const QColor secondary = pageTokenColor("textSecondary");
    note->setTextColor(secondary, secondary);
    page->addContent(note);

    page->addContent(buildOptionCard(
        QStringLiteral("A"), QStringLiteral("账号 / 团队"),
        QStringLiteral("多账号管理：正版账号列表、切换、每个账号独立的离线名与皮肤。"
                       "（现在只有 dialogs/account.* 那一个账号的登录/续期/注销。）"),
        page->view()));
    page->addContent(buildOptionCard(
        QStringLiteral("B"), QStringLiteral("联机分组"),
        QStringLiteral("把「联机」再往上包一层：一份团队成员列表 + 一键开房/加入，"
                       "房间与成员都在这页管理。"),
        page->view()));
    page->addContent(buildOptionCard(
        QStringLiteral("C"), QStringLiteral("团队实例"),
        QStringLiteral("把整合包/实例在团队内共享：导出团队包、别人导入即用（含模组与配置）。"),
        page->view()));

    page->addStretch();
    return page;
}

} // namespace sxcl::ui
