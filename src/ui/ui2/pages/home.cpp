/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "home.h"

#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QToolButton>
#include <QVBoxLayout>

#include <cstdio>
#include <memory>

#include "task.h"
#include "widgets/accordion.h"
#include "widgets/hero_card.h"
#include "widgets/notice_card.h"
#include "widgets/player_card.h"

namespace sxcl::ui2 {
namespace {

/** 大卡那一行的元信息:**能被猜到的信息一个字都不写**(§11.5)。
 *  有加载器 -> "Forge 47.2.0 · 原版 1.20.1";纯原版 -> 版本形态(正式版/快照);
 *  版本名本身已经说过的话不再重复(原版 1.20.1 那一行里,版本名就是 1.20.1)。 */
QString metaFor(const VersionItem &item) {
    const QString raw = item.summary.trimmed();
    const bool loader = !raw.isEmpty() && raw != QLatin1String("原版");
    QString text = raw;
    if (!loader) {
        const QString type = item.type.trimmed();
        text = type.isEmpty() || type == QLatin1String("release") ? QStringLiteral("正式版") : type;
    }
    if (!item.base.isEmpty() && !text.contains(item.base) && (loader || item.base != item.id))
        text += QStringLiteral(" · 原版 ") + item.base;
    return text;
}

} // namespace

HomePage::HomePage(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sxcl2HomePage"));

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 24);
    root->setSpacing(16);

    auto *left = new QVBoxLayout;
    left->setSpacing(16);
    m_hero = new HeroCard(this);
    left->addWidget(m_hero);

    m_accordion = new AccordionSection(this);
    m_accordion->setVisible(false); // 收起态整块不出现(点「切换版本」才拉出来)
    left->addWidget(m_accordion);

    // 公告卡:有内容才建(§11.4:没有内容时整块不出现,绝不写"暂无公告")
    const QStringList notice = readNoticeLines();
    if (!notice.isEmpty()) {
        m_notice = new NoticeCard(notice, this);
        left->addWidget(m_notice);
    }
    left->addStretch(1);
    root->addLayout(left, 1);

    auto *right = new QVBoxLayout;
    right->setSpacing(16);
    m_player = new PlayerCard(this);
    m_player->setFixedWidth(300);
    right->addWidget(m_player);
    right->addStretch(1);
    root->addLayout(right, 0);

    connect(m_hero, &HeroCard::swapRequested, this,
            [this] { m_accordion->setVisible(true); m_accordion->toggle(); });
    connect(m_hero, &HeroCard::launchRequested, this, &HomePage::onLaunchClicked);
    connect(m_hero, &HeroCard::gearRequested, this,
            [] { std::fprintf(stderr, "[sxcl-ui2] 版本设置(齿轮,M3 接版本设置页)\n"); });
    connect(m_accordion, &AccordionSection::allRequested, this,
            &HomePage::allVersionsRequested);
    connect(m_player, &PlayerCard::editionPicked, this, &HomePage::pickEdition);
    connect(m_player, &PlayerCard::skinRequested, this, &HomePage::onSkinClicked);
    connect(m_player, &PlayerCard::accountRequested, this, &HomePage::onAccountClicked);
    connect(m_player, &PlayerCard::renameRequested, this, &HomePage::onRenameClicked);

    m_offlineName = readOfflineName();
    m_player->setOfflineName(m_offlineName);
}

HomePage::~HomePage() = default;

void HomePage::startLoad() {
    /* 两个工作线程任务。页面持句柄 -> 页面析构先取消再回收(task.h 的口径)。
     * 结果走 shared_ptr 进 done,页面不持有半成品数据。 */
    auto home = std::make_shared<HomeData>();
    m_taskHome = new Task(this, QStringLiteral("home-instances"), Task::Kind::Fs);
    m_taskHome->start([home] { *home = loadHomeData(); },
                      [this, home] { applyHomeData(*home); });

    auto player = std::make_shared<PlayerData>();
    loadAccount();
}

void HomePage::loadAccount() {
    if (m_taskPlayer != nullptr && m_taskPlayer->running())
        return; // 已经在读,不重复起线程
    auto player = std::make_shared<PlayerData>();
    m_taskPlayer = new Task(this, QStringLiteral("home-account"), Task::Kind::Fs);
    m_taskPlayer->start([player] { *player = loadPlayerData(); },
                        [this, player] { applyPlayerData(*player); });
}

void HomePage::applyHomeData(const HomeData &home) {
    // 参数名不能叫 data:QWidget 有一个受保护的成员 data(QWidgetData*),/W4 下是 C4458 警告(/WX 直接失败)
    m_versions = home.versions;
    std::fprintf(stderr,
                 "[sxcl-ui2] home: 已安装=%d 个 游戏目录=%s(%s) 扫不动=%s\n",
                 static_cast<int>(m_versions.size()), home.gameDir.toUtf8().constData(),
                 home.gameDirReason.toUtf8().constData(),
                 home.error.isEmpty() ? "无" : home.error.toUtf8().constData());

    int currentIndex = -1;
    for (int i = 0; i < m_versions.size(); ++i) {
        if (m_versions.at(i).launchable) {
            currentIndex = i;
            break;
        }
    }
    if (currentIndex < 0 && !m_versions.isEmpty())
        currentIndex = 0;

    QVector<VersionRow> rows;
    rows.reserve(m_versions.size());
    for (const VersionItem &item : m_versions) {
        VersionRow row;
        row.name = item.id;
        row.meta = item.launchable ? metaFor(item) : item.problem;
        rows.append(row);
    }
    m_accordion->setRows(rows);

    if (currentIndex < 0) {
        m_currentVersion.clear();
        m_hero->setVersion(QStringLiteral("没有已安装的版本"),
                           home.gameDir.isEmpty() ? QStringLiteral("没找到游戏目录")
                                                  : QStringLiteral("去下载页装一个"),
                           false);
        return;
    }
    const VersionItem &item = m_versions.at(currentIndex);
    m_currentVersion = item.id;
    m_hero->setVersion(item.id, metaFor(item), item.launchable);
    m_hero->launchButton()->setToolTip(item.problem);
}

void HomePage::applyPlayerData(const PlayerData &account) {
    m_loggedIn = account.loggedIn;
    m_premiumName = account.name;
    std::fprintf(stderr, "[sxcl-ui2] home: 账户 读到=%s 正版=%s 名字=%s 失败=%s\n",
                 account.read ? "是" : "否", account.loggedIn ? "是" : "否",
                 account.name.isEmpty() ? "(无)" : account.name.toUtf8().constData(),
                 account.error.isEmpty() ? "无" : account.error.toUtf8().constData());
    m_player->setAccount(m_loggedIn, m_premiumName);
}

void HomePage::setPremium(bool premium) {
    m_premium = premium;
    m_player->setPremium(premium);
}

void HomePage::pickEdition(bool premium) {
    m_premium = premium;
    m_player->setPremium(premium); // 高亮转移
    savePremiumEdition(premium);   // 落盘(game.edition)
    std::fprintf(stderr, "[sxcl-ui2] 版本形态: %s(M2:账户面板在 M3 接)\n",
                 premium ? "正版" : "离线");
}

void HomePage::onLaunchClicked() {
    /* M2 只把大卡与数据接通;**启动层接线在 M5**(docs/27 §7)。
     * 这里如实打一行,不往界面上写"功能未接入"那类解释性文案(§11.5)。 */
    std::fprintf(stderr, "[sxcl-ui2] 启动游戏: 版本=%s(M2 未接启动层,见 docs/27 §7 M5)\n",
                 m_currentVersion.isEmpty() ? "(无)" : m_currentVersion.toUtf8().constData());
}

void HomePage::onSkinClicked() {
    /* 换皮肤:重新读一次账户(头像/名字的来源)。皮肤选择器属于账户那一轮(M3)。 */
    std::fprintf(stderr, "[sxcl-ui2] 玩家卡:换皮肤 -> 重新读取账户来源\n");
    loadAccount();
}

void HomePage::onAccountClicked() {
    /* 切换账号:同样先重新读一次;设备码登录面板在 M3 接(旧界面的 AuthLoginDialog 不搬进 ui2)。 */
    std::fprintf(stderr, "[sxcl-ui2] 玩家卡:切换账号 -> 重新读取账户状态\n");
    loadAccount();
}

void HomePage::onRenameClicked() {
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("更改名字"),
                                               QStringLiteral("离线玩家名"), QLineEdit::Normal,
                                               m_offlineName, &ok);
    if (!ok)
        return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return;
    m_offlineName = trimmed;
    saveOfflineName(trimmed);
    m_player->setOfflineName(trimmed);
    std::fprintf(stderr, "[sxcl-ui2] 玩家卡:离线名改为 %s\n", trimmed.toUtf8().constData());
}

} // namespace sxcl::ui2
