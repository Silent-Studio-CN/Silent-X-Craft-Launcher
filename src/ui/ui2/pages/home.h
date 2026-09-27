/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 主页(docs/27 §10.3 / §11 的四块):当前版本大卡 + 就地手风琴的收藏版本 + 玩家卡 + 公告卡。
//
// 页面里**只有数据与槽**(§3):布局、样式、取数都在别处 ——
//   * 取数走 Task(工作线程)+ sources(阻塞源),页面一行阻塞调用都没有;
//   * 颜色全在 tokens/QSS 里,页面里没有一处 setStyleSheet。
#pragma once

#include <QWidget>

#include "sources.h"

namespace sxcl::ui2 {

class AccordionSection;
class HeroCard;
class NoticeCard;
class PlayerCard;
class Task;

class HomePage : public QWidget {
    Q_OBJECT
public:
    explicit HomePage(QWidget *parent = nullptr);
    ~HomePage() override;

    /** 起两个工作线程任务(已安装版本 + 账户)。构造期不碰磁盘。 */
    void startLoad();

    HeroCard *hero() const { return m_hero; }
    PlayerCard *player() const { return m_player; }
    AccordionSection *accordion() const { return m_accordion; }
    /** 没有公告内容时是 nullptr —— 那块**整块不出现**(§11.4)。 */
    NoticeCard *notice() const { return m_notice; }

    /** 当前版本名(大卡上那个;空 = 一个都没装)。 */
    QString currentVersion() const { return m_currentVersion; }
    QVector<VersionItem> versions() const { return m_versions; }

    /** 版本形态:true = 正版,false = 离线。外壳读了设置之后告诉页面。 */
    void setPremium(bool premium);
    /** 点某一枚图标:高亮转移 + 落盘(game.edition)。 */
    void pickEdition(bool premium);

signals:
    void allVersionsRequested(); // "查看全部版本" —— 去版本选择页(M3)

private:
    void applyHomeData(const HomeData &data);
    void applyPlayerData(const PlayerData &data);
    /** 只起账户那个任务(换皮肤 / 切换账号 = 重新读一次来源)。 */
    void loadAccount();
    void onLaunchClicked();
    void onSkinClicked();
    void onAccountClicked();
    void onRenameClicked();

    HeroCard *m_hero = nullptr;
    PlayerCard *m_player = nullptr;
    AccordionSection *m_accordion = nullptr;
    NoticeCard *m_notice = nullptr;
    Task *m_taskHome = nullptr;
    Task *m_taskPlayer = nullptr;
    QString m_currentVersion;
    QVector<VersionItem> m_versions;
    bool m_premium = true;
    bool m_loggedIn = false;
    QString m_premiumName;
    QString m_offlineName;
};

} // namespace sxcl::ui2
