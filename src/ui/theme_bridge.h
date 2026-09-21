/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include <QColor>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>

#include "libqf.h" // fluent::Theme / FluentStyle / FluentBackgroundTheme

class QWidget;

namespace sxcl::ui {

class ThemeBridge {
public:
    static ThemeBridge &instance();

    // ---- 主题模式:全部转发给 libqf 单例 ----
    bool isDark() const;
    fluent::Theme mode() const;
    void setMode(fluent::Theme t); // 立即生效(libqf reapplyAll + 调色板 + 广播)
    void toggleMode();

    // ---- 语义令牌 ----
    QColor token(const QString &name) const;
    QColor accent() const;    // = libqf 当前主题色(FluentStyle::themeColor)
    QColor iconColor() const; // 导航/正文图标着色(= 当前主题文字色)

    // ---- 窗口登记(主题变化时自动重刷;不重建窗口) ----
    void attach(QWidget *root);
    void detach(QWidget *root);
    void refreshAll();

    // 令牌名清单(报告/自检用)
    static QStringList tokenNames();

private:
    ThemeBridge();
    void applyTo(QWidget *root);

    QVector<QPointer<QWidget>> m_roots;
};

} // namespace sxcl::ui
