/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "player_card.h"

#include <QApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "avatar.h"
#include "icon_button.h"
#include "icons.h"

namespace sxcl::ui2 {
namespace {
constexpr int kActionGap = 8; // §11.1:图标 20px、间距 8
} // namespace

/* ── 玩家名那一行 ─────────────────────────────────────────────────────── */

HoverNameRow::HoverNameRow(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sxcl2NameRow"));
    setFixedHeight(32);
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    m_name = new QLabel(this);
    m_name->setObjectName(QStringLiteral("sxcl2PlayerName"));
    h->addWidget(m_name);

    struct ActionSpec {
        const char *file;
        const char *object;
        const char *tip;
        int signalId;
    };
    const ActionSpec specs[] = {
        {"tshirt.svg", "sxcl2NameActionSkin", "换皮肤", 0},
        {"swap.svg", "sxcl2NameActionAccount", "切换账号", 1},
        {"pencil.svg", "sxcl2NameActionRename", "更改名字", 2},
    };
    for (const ActionSpec &spec : specs) {
        auto *btn = new IconButton(uiIconPath(QString::fromLatin1(spec.file)), 20, this);
        btn->setObjectName(QString::fromLatin1(spec.object));
        btn->setToolTip(QString::fromUtf8(spec.tip));
        btn->setVisible(false); // 平时不占地方(隐藏 = 布局不参与;这几枚本来就不在布局里)
        connect(btn, &IconButton::clicked, this, [this, signalId = spec.signalId] {
            if (signalId == 0)
                emit skinClicked();
            else if (signalId == 1)
                emit accountClicked();
            else
                emit renameClicked();
        });
        m_actions.append(btn);
    }
}

void HoverNameRow::setName(const QString &name) { m_name->setText(name); }

QString HoverNameRow::name() const { return m_name->text(); }

bool HoverNameRow::actionsVisible() const {
    for (IconButton *btn : m_actions) {
        if (btn == nullptr || !btn->isVisible())
            return false;
    }
    return !m_actions.isEmpty();
}

void HoverNameRow::setActionsShown(bool shown) {
    for (IconButton *btn : m_actions)
        btn->setVisible(shown);
    if (shown)
        layoutActions();
}

void HoverNameRow::layoutActions() {
    // 三枚图标贴在行尾(手动摆位 —— 不进布局,所以"出现"不会把名字顶走)
    const int n = m_actions.size();
    if (n <= 0)
        return;
    const int w = m_actions.first()->width();
    const int total = n * w + (n - 1) * kActionGap;
    const int x0 = width() - total;
    const int y = (height() - m_actions.first()->height()) / 2;
    for (int i = 0; i < n; ++i)
        m_actions.at(i)->move(x0 + i * (w + kActionGap), y);
}

void HoverNameRow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    layoutActions();
}

void HoverNameRow::enterEvent(QEnterEvent *event) {
    setActionsShown(true);
    QWidget::enterEvent(event);
}

void HoverNameRow::leaveEvent(QEvent *event) {
    /* Qt 在"光标进了子控件"时**不会**给父控件发 Leave(子控件是后代,enter/leave 链会跳过),
     * 所以这里不需要再拿 QCursor::pos() 兜一道 —— 兜了反而会让验收钩子的模拟离开失效。 */
    setActionsShown(false);
    QWidget::leaveEvent(event);
}

void HoverNameRow::simulateHover(bool enter) {
    if (enter) {
        const QPointF local(width() / 2.0, height() / 2.0);
        QEnterEvent event(local, local, mapToGlobal(local.toPoint()));
        QApplication::sendEvent(this, &event);
        return;
    }
    QEvent event(QEvent::Leave);
    QApplication::sendEvent(this, &event);
}

/* ── 卡片本体 ─────────────────────────────────────────────────────────── */

PlayerCard::PlayerCard(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sxcl2PlayerCard"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(20, 16, 20, 18);
    v->setSpacing(10);

    auto *top = new QHBoxLayout;
    top->setSpacing(8);
    auto *caption = new QLabel(QStringLiteral("玩家"), this);
    caption->setObjectName(QStringLiteral("sxcl2PlayerCaption"));
    top->addWidget(caption);
    top->addStretch(1);

    // 右上角两枚图标:微软 LOGO(正版) + 断线图标(离线),各自可点,当前态高亮(§11.2 / §13.2)
    auto *editionRow = new QWidget(this);
    editionRow->setObjectName(QStringLiteral("sxcl2EditionRow"));
    auto *eh = new QHBoxLayout(editionRow);
    eh->setContentsMargins(0, 0, 0, 0);
    eh->setSpacing(8);
    m_premiumBtn = new IconButton(uiIconPath(QStringLiteral("microsoft.svg")), 20, editionRow);
    m_premiumBtn->setObjectName(QStringLiteral("sxcl2EditionPremium"));
    m_premiumBtn->setTint(IconButton::Tint::Original); // 微软 LOGO 保留官方四色
    m_premiumBtn->setToolTip(QStringLiteral("正版"));
    eh->addWidget(m_premiumBtn);
    m_offlineBtn = new IconButton(uiIconPath(QStringLiteral("disconnected.svg")), 20, editionRow);
    m_offlineBtn->setObjectName(QStringLiteral("sxcl2EditionOffline"));
    m_offlineBtn->setToolTip(QStringLiteral("离线"));
    eh->addWidget(m_offlineBtn);
    connect(m_premiumBtn, &IconButton::clicked, this, [this] { emit editionPicked(true); });
    connect(m_offlineBtn, &IconButton::clicked, this, [this] { emit editionPicked(false); });
    top->addWidget(editionRow);
    v->addLayout(top);

    m_avatar = new AvatarWidget(this);
    m_avatar->setObjectName(QStringLiteral("sxcl2Avatar"));
    auto *avatarRow = new QHBoxLayout;
    avatarRow->setContentsMargins(0, 0, 0, 0);
    avatarRow->addWidget(m_avatar);
    avatarRow->addStretch(1);
    v->addLayout(avatarRow);

    m_nameRow = new HoverNameRow(this);
    connect(m_nameRow, &HoverNameRow::skinClicked, this, &PlayerCard::skinRequested);
    connect(m_nameRow, &HoverNameRow::accountClicked, this, &PlayerCard::accountRequested);
    connect(m_nameRow, &HoverNameRow::renameClicked, this, &PlayerCard::renameRequested);
    v->addWidget(m_nameRow);
    v->addStretch(1);

    refreshIdentity();
    setPremium(true);
}

void PlayerCard::setPremium(bool premium) {
    m_premium = premium;
    m_premiumBtn->setSelected(premium);
    m_offlineBtn->setSelected(!premium);
    refreshIdentity();
}

void PlayerCard::setAccount(bool loggedIn, const QString &premiumName) {
    m_loggedIn = loggedIn;
    m_premiumName = premiumName;
    refreshIdentity();
}

void PlayerCard::setOfflineName(const QString &name) {
    m_offlineName = name;
    refreshIdentity();
}

void PlayerCard::refreshIdentity() {
    /* 显示哪条身份:**正版可用就用正版 ID**(名字本身就是"登录了"的证据),
     * 否则用离线名。两处都不加解释性文字(§11.2 / §11.5)。 */
    const QString name = (m_premium && m_loggedIn && !m_premiumName.isEmpty())
                             ? m_premiumName
                             : (m_offlineName.isEmpty() ? QStringLiteral("Steve") : m_offlineName);
    m_nameRow->setName(name);
    m_avatar->setName(name);
}

} // namespace sxcl::ui2
