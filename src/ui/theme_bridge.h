#pragma once
// theme_bridge — 主题桥接(阶段 6 地基)
//
// 硬性规则(踩过的坑,别再犯):本层**不定义 themeMode / themeColor**,也**不存**自己的
// 主题状态。主题模式与主题色一律读/写 libqf 的单例 fluent::FluentStyle
// (对应 Python 版的 qconfig.themeMode / qconfig.themeColor)。
//
// 本层只做两件事:
//   1) 把界面层要用的颜色收敛成"语义令牌"(bg / card / nav / text / border / accent ...),
//      其中画布色取自 libqf FluentBackgroundTheme,强调色取自 FluentStyle::themeColor();
//   2) 主题一变(或窗口刚建好)就把令牌重新套到登记过的窗口上 —— 对应 Python
//      src/app/theme.py 的 apply_theme()/refresh_widgets(),但只刷新不重建窗口。
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
