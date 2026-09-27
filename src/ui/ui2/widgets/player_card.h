/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 玩家卡(docs/27 §10.3 / §11.2 交互原则 V3)。
//
// 卡上自上而下:左上角一行小标题 + **右上角两枚图标**(微软 LOGO = 正版 / 断线图标 = 离线,
// 当前态高亮)、头像(圆形,首字母)、**玩家名那一行**(悬停时行尾浮出三枚图标)。
//
// 三条口径直接落在结构里:
//   1. 不写"正版 · 已登录"这类废话 —— 拿到正版 ID 就只显示那个 ID;
//   2. 正版/离线是**两枚各自可点的图标**,不是滑块、不是带文字的按钮(§13.2 的默认排版);
//   3. 换皮肤 / 切换账号 / 更改名字 平时**不占地方**(隐藏 = 布局不参与,悬停才出现,§11.1)。
#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;

namespace sxcl::ui2 {

class AvatarWidget;
class IconButton;

/** 玩家名那一行:平时只有名字;鼠标进来,行尾浮出三枚图标(HoverActions)。 */
class HoverNameRow : public QWidget {
    Q_OBJECT
public:
    explicit HoverNameRow(QWidget *parent = nullptr);

    void setName(const QString &name);
    QString name() const;

    QVector<IconButton *> actions() const { return m_actions; }
    bool actionsVisible() const;
    /** 验收钩子:模拟鼠标进入/离开 —— 真的发 Enter/Leave 事件(不是直接改状态)。 */
    void simulateHover(bool enter);

signals:
    void skinClicked();
    void accountClicked();
    void renameClicked();

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void layoutActions();
    void setActionsShown(bool shown);

    QLabel *m_name = nullptr;
    QVector<IconButton *> m_actions;
};

class PlayerCard : public QWidget {
    Q_OBJECT
public:
    explicit PlayerCard(QWidget *parent = nullptr);

    void setPremium(bool premium);
    bool premium() const { return m_premium; }

    /** 账户状态(正版):loggedIn + 正版 ID。**只显示那个 ID**。 */
    void setAccount(bool loggedIn, const QString &premiumName);
    /** 离线玩家名(空 = Steve)。 */
    void setOfflineName(const QString &name);

    IconButton *premiumButton() const { return m_premiumBtn; }
    IconButton *offlineButton() const { return m_offlineBtn; }
    HoverNameRow *nameRow() const { return m_nameRow; }
    AvatarWidget *avatar() const { return m_avatar; }

signals:
    void editionPicked(bool premium); // 用户点了某一枚图标(落盘由页面做)
    void skinRequested();
    void accountRequested();
    void renameRequested();

private:
    void refreshIdentity();

    AvatarWidget *m_avatar = nullptr;
    HoverNameRow *m_nameRow = nullptr;
    IconButton *m_premiumBtn = nullptr;
    IconButton *m_offlineBtn = nullptr;
    bool m_premium = true;
    bool m_loggedIn = false;
    QString m_premiumName;
    QString m_offlineName;
};

} // namespace sxcl::ui2
