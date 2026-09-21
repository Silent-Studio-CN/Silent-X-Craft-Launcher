/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include <QFrame>
#include <QHash>
#include <QString>
#include <QVector>

#include "libqf.h"

#include "icon_registry.h"
class QVBoxLayout;
class QPropertyAnimation;

namespace sxcl::ui {

// 一个导航项的声明(路由键 / 图标 / 标题 / 是否在底部)
// 图标两种来源,对应 Python 版 main_window.py:105-119 的写法:
//   * qfIcon 非空   -> qf 原生图标(FIF.HOME / FIF.UPDATE / FIF.LAYOUT / FIF.GLOBE / FIF.SETTING)
//   * blockKind 非空 -> 方块图(grass_block_icon(24),来自 src/app/icons.py)
struct NavItem {
    QString routeKey;
    QString qfIcon;    // qf 图标名(与 Python FluentIcon 枚举同名,如 "Home")
    QString blockKind; // 方块种类(BLOCK_FILES 的键,如 "vanilla")
    QString title;
    bool bottom = false;
};

// 纯图标工具按钮(qf python NavigationToolButton):
//   * 尺寸恒为 40x36(setCompacted 被覆写,不随面板展开变化)
//   * 不可选中(isSelectable=False -> 不画选中态/指示条)
//   * 只画图标,不画文字(isCompacted 恒为 True)
class NavToolButton : public NavigationWidget {
    Q_OBJECT
public:
    // svgName:qf 图标名,如 "Menu" / "Return"
    explicit NavToolButton(const QString &svgName, QWidget *parent = nullptr);

    void setSvgName(const QString &svgName);
    QString svgName() const { return m_svgName; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_svgName;
};

// 导航条目:libqf 的 NavigationPushButton,绘制按 qf navigation_widget.py:176-214 逐句复刻
class NavButton : public NavigationPushButton {
    Q_OBJECT
public:
    NavButton(const QIcon &icon, const QString &qfIconName, const QString &blockKind,
              const QString &text, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *) override;
    QRect indicatorRect() const override; // 选中指示条:QRectF(0,10,3,16)

private:
    QString m_qfIconName;
    QString m_blockKind;
};

// 导航面板:顶部组(返回 + 菜单 + 导航项)+ 弹性 + 底部组(设置)
class NavPanel : public QFrame {
    Q_OBJECT
public:
    // qf navigation_panel.py:96-97,108,121-122 —— 全部照抄,不许改
    static constexpr int kExpandedWidth = 322;  // expandWidth
    static constexpr int kCollapsedWidth = 48;  // resize(48, height) / setMinimumWidth(48)
    static constexpr int kExpandDurationMs = 150; // expandAni.setDuration(150)
    static constexpr int kMinimumExpandWidth = 1008; // 窗口更宽才是 EXPAND 模式(否则 MENU 覆盖层)

    explicit NavPanel(QWidget *parent = nullptr);

    NavigationPushButton *addItem(const NavItem &item);
    void setCurrent(const QString &routeKey);
    QString currentRouteKey() const { return m_current; }
    const QVector<NavItem> &items() const { return m_items; }
    NavigationPushButton *button(const QString &routeKey) const;

    // qf navigation_panel.py:83 menuButton 对应的汉堡按钮(与条目的间距、位置同 qf)
    NavToolButton *menuButton() const { return m_menuBtn; }
    NavToolButton *returnButton() const { return m_returnBtn; }

    bool collapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed); // 汉堡按钮切换;折叠后仅图标(库的 compacted 态)

signals:
    void routeChanged(const QString &routeKey);

private:
    void setWidgetsCompacted(bool compacted);

    QVBoxLayout *m_top = nullptr;
    QVBoxLayout *m_bottom = nullptr;
    NavToolButton *m_returnBtn = nullptr;
    NavToolButton *m_menuBtn = nullptr;
    QPropertyAnimation *m_widthAni = nullptr; // 展开/折叠:150ms OutQuad(qf navigation_panel.py:121-122)
    QVector<NavItem> m_items;
    QHash<QString, NavigationPushButton *> m_buttons;
    QString m_current;
    bool m_collapsed = true; // qf 默认 displayMode = COMPACT(折叠,48 宽)
};

} // namespace sxcl::ui
