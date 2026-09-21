/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include <QColor>
#include <QObject>
#include <QString>
#include <QStringList>

class QApplication;

namespace sxcl::ui {

// 令牌表：字段与 Python theme.py 的键一一对应
struct ThemeTokens {
    QColor bg;
    QColor bgNav;
    QColor card;
    QColor cardHover;
    QColor border;
    QColor borderStrong;
    QColor separator;
    QString hover;       // rgba(...) 原样字符串
    QString hoverStrong;
    QString hoverBg;
    QString hoverBgStrong;
    QColor text;
    QColor textSecondary;
    QColor textTertiary;
    QColor textDisabled;
    QColor inputBg;
    QColor inputBorder;
    QString track;       // rgba(...) 原样字符串
    QColor success;
    QColor warning;
    QColor danger;
    QColor info;
    QColor onAccent;
    QColor accent;       // = qf themeColor()（可配置，默认 #0067c0）
};

class FluentTheme : public QObject {
    Q_OBJECT
public:
    static FluentTheme &instance();

    // ---- 主题模式 ----
    bool isDark() const;
    void setDark(bool dark); // 立即重套样式并 emit changed()
    void toggle();

    // ---- 强调色（对应 qconfig.themeColor + qf ThemeColor 派生）----
    // 推导后的强调色 = Python theme.py 的 themeColor()(页面/自绘/QSS 都用它)
    QColor accent() const;
    // 配置里的原始色 = Python qconfig.themeColor(设置页的取色器显示/保存用这个)
    QColor rawAccent() const { return m_accent; }
    void setAccent(const QColor &c);
    // index: 0=Primary 1=Dark1 2=Dark2 3=Dark3 4=Light1 5=Light2 6=Light3
    QColor accentVariant(int index) const;

    // ---- 令牌 ----
    const ThemeTokens &tokens() const;
    QString tokenText(const QString &name) const; // 令牌名 -> "#rrggbb" 或 "rgba(...)"
    QStringList tokenNames() const;
    static QColor parseCssColor(const QString &raw);

    // ---- 资产 ----
    void setThemeDir(const QString &dir);
    QString themeDir() const;
    bool resolveThemeDir(); // 编译期目录 -> 环境变量 -> exe 旁的 assets/theme

    bool loadQss();                        // 读 qf 原版 QSS + 替换占位符
    bool qssLoaded() const;
    int qssFileCount() const;
    QString appStyleSheet() const;         // qf QSS + 应用级 QSS
    void apply(QApplication *app);         // 设调色板 + 全局样式表
    static QString fontFamiliesQss();      // --FontFamilies 的替换值

    // ---- 应用级样式（逐条对应 Python src/app/styles.py）----
    QString cardQss(int radius = 8, const QString &padding = QStringLiteral("16px 18px")) const;
    QString sectionCardQss(int radius = 8) const;
    QString mutedQss(int size = 12, const QString &margin = QStringLiteral("0")) const;
    QString ghostButtonQss() const;
    QString chipQss(const QString &colorToken) const;
    QColor badgeColor(const QString &state) const;

signals:
    void changed();

private:
    FluentTheme();
    void rebuild();

    bool m_dark = true;
    QColor m_accent;
    QString m_dir;
    QString m_qfQss;
    int m_qssFiles = 0;
};

} // namespace sxcl::ui
