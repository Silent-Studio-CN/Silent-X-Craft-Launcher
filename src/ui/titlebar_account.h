/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

// 顶栏那枚「玩家」标记:头像 + 玩家名,点它进账户管理页。
//
// 位置由外壳定(标题右边、最小化/最大化/关闭左边,见 main_window.cpp 的 buildUi)。
// 这里只管自己那一块怎么画:
//   * 头像 = **玩家自己的皮肤**裁出来的头(取图见 skin_store.h);拿不到就画剪影占位,
//     **不写任何解释文字**(用户口径);
//   * 名字 = 正版档案里的玩家名(拿不到就不占版面 —— 只留头像);
//   * 颜色按主题**现取**(深色白字 / 浅色黑字,与 qf 的 #titleLabel 同一档),
//     所以自绘而不是挂 QLabel:主题一切就跟着变,不用再登记一遍刷新。
//
// 账户状态**不在这里解密**:refresh() 先比令牌文件的时间戳,只有真变了才读一次快照。

#include <QString>
#include <QWidget>

class QMouseEvent;

namespace sxcl::ui {

class TitlebarAccount : public QWidget {
    Q_OBJECT
public:
    explicit TitlebarAccount(QWidget *parent = nullptr);

    /** 重读账户(便宜:时间戳没变就只重画头像)并刷新。 */
    void refresh();
    /** 头像边长(逻辑像素)。 */
    static constexpr int kAvatarSide = 24;
    /** 这一枚的整体高度(逻辑像素)。 */
    static constexpr int kHeight = 32;

    QString playerName() const { return m_playerName; }
    bool hasSkin() const { return m_hasSkin; }
    QSize sizeHint() const override;

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void syncAvatar();
    void setPlayerName(const QString &name);

    QString m_playerName;
    QString m_uuid;    // 当前头像对应的账户(空 = 没登录)
    QString m_tokenPath;
    qint64 m_tokenStamp = -1; // 令牌文件 (存在|修改时间);-1 = 还没读过
    bool m_hasSkin = false;
    bool m_hover = false;
};

} // namespace sxcl::ui
