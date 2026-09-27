/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 当前版本大卡(docs/27 §3 HeroCard / §10.3):版本名 + 元信息一行 + 主按钮 启动游戏
// + 次按钮 切换版本;版本设置(齿轮)挪到卡片右上角、**悬停才出现**(与版本行同一套语义)。
//
// 一屏只有一个文字主按钮(§11.1),所以这张卡里 启动游戏 是 primary,切换版本 是 ghost ——
// 它们都是"猜不到的动作"(用户口径明确写着这两个词),其余动作一律图标。
#pragma once

#include <QString>
#include <QWidget>

class QLabel;
class QToolButton;

namespace sxcl::ui2 {

class IconButton;

class HeroCard : public QWidget {
    Q_OBJECT
public:
    explicit HeroCard(QWidget *parent = nullptr);

    /** 大卡上的三个事实:版本名 / 元信息一行(加载器 + Java)/ 能不能启动。 */
    void setVersion(const QString &name, const QString &meta, bool launchable);

    QToolButton *swapButton() const { return m_swap; }
    QToolButton *launchButton() const { return m_launch; }
    IconButton *gearButton() const { return m_gear; }

signals:
    void launchRequested();
    void swapRequested();
    void gearRequested();

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QLabel *m_caption = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    QToolButton *m_launch = nullptr;
    QToolButton *m_swap = nullptr;
    IconButton *m_gear = nullptr;
};

} // namespace sxcl::ui2
