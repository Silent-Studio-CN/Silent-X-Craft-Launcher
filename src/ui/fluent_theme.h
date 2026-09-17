#pragma once
// fluent_theme —— 1:1 主题层（对应 Python 版 src/app/theme.py + qfluentwidgets 的样式机制）
//
// 三条硬规矩（依据见 docs/05-UI-1to1规格.md）：
//   1) 颜色令牌逐字等于 Python _LIGHT / _DARK，不许自己挑颜色；
//   2) 强调色按 qf 的 ThemeColor 算法从主色派生（HSV 变换照抄 qf common/style_sheet.py:463-504）；
//   3) 样式表直接用 qf 原版 QSS（assets/theme/qf_exact/<theme>/*.qss，从 Python 端同版本的
//      _rc/resource.py 抽出），只替换 --FontFamilies / --ThemeColor* 占位符。
// 因此：本层不发明任何视觉，只负责"把 Python 端的数据搬过来并套上"。
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
    QColor accent() const;
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
