#include "theme_bridge.h"

#include "fluent_theme.h" // 令牌的唯一来源(docs/05-UI-1to1规格.md §2)

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QWidget>

namespace sxcl::ui {

namespace {

// 中性色板:与 Python src/app/theme.py 的 _LIGHT / _DARK 同源。
// 注意 bg/bgNav 不在这里写死 —— 它们来自 libqf FluentBackgroundTheme,
// accent 来自 libqf FluentStyle::themeColor(),避免"两套主题色打架"。
struct Row {
    const char *name;
    const char *light;
    const char *dark;
};

const Row kRows[] = {
    {"card", "#ffffff", "#2b2b2b"},
    {"cardHover", "#f7f7f7", "#333333"},
    {"border", "#e1e1e1", "#3d3d3d"},
    {"borderStrong", "#c8c8c8", "#555555"},
    {"separator", "#e8e8e8", "#383838"},
    {"text", "#1a1a1a", "#ffffff"},
    {"textSecondary", "#5d5d5d", "#c7c7c7"},
    {"textTertiary", "#8a8a8a", "#9a9a9a"},
    {"textDisabled", "#a6a6a6", "#6b6b6b"},
    {"hover", "rgba(0, 0, 0, 0.05)", "rgba(255, 255, 255, 0.06)"},
    {"hoverStrong", "rgba(0, 0, 0, 0.09)", "rgba(255, 255, 255, 0.11)"},
    {"danger", "#c42b1c", "#ff99a4"},
    {"success", "#0f7b0f", "#6ccb5f"},
};

QColor parseCss(const QString &raw) {
    if (!raw.startsWith(QLatin1String("rgba")))
        return QColor(raw);
    const int l = raw.indexOf(QLatin1Char('(')), r = raw.indexOf(QLatin1Char(')'));
    if (l < 0 || r <= l)
        return QColor();
    const QStringList parts = raw.mid(l + 1, r - l - 1).split(QLatin1Char(','));
    if (parts.size() < 3)
        return QColor();
    const int a = parts.size() > 3 ? int(parts[3].trimmed().toDouble() * 255.0 + 0.5) : 255;
    return QColor(parts[0].trimmed().toInt(), parts[1].trimmed().toInt(),
                  parts[2].trimmed().toInt(), a);
}

} // namespace

ThemeBridge &ThemeBridge::instance() {
    static ThemeBridge inst;
    return inst;
}

ThemeBridge::ThemeBridge() {
    // 主题一变(libqf 侧 setTheme/setThemeColor)就重刷登记过的窗口
    fluent::FluentStyle::instance()->subscribe([this] { refreshAll(); });
}

bool ThemeBridge::isDark() const { return FluentTheme::instance().isDark(); }

fluent::Theme ThemeBridge::mode() const { return fluent::FluentStyle::instance()->theme(); }

void ThemeBridge::setMode(fluent::Theme t) {
    // 唯一权威是 FluentTheme:它会对齐 libqf 引擎(主题/主题色/调色板/reapplyAll),
    // 再广播给登记过的窗口。不再直接调用 libqf 的 setTheme —— 那会绕过令牌层。
    FluentTheme::instance().setDark(t == fluent::Theme::Dark);
    refreshAll();
}

void ThemeBridge::toggleMode() { setMode(isDark() ? fluent::Theme::Light : fluent::Theme::Dark); }

QColor ThemeBridge::accent() const { return FluentTheme::instance().accent(); }

QColor ThemeBridge::token(const QString &name) const {
    // 全部令牌(含 bg/bgNav/accent/onAccent)只从 FluentTheme 取 —— 那里的值与
    // Python src/app/theme.py 逐字一致。本层不再自带色表(避免第二次"两套颜色打架")。
    return FluentTheme::parseCssColor(FluentTheme::instance().tokenText(name));
}

QColor ThemeBridge::iconColor() const { return token(QStringLiteral("text")); }

QStringList ThemeBridge::tokenNames() {
    QStringList out{QStringLiteral("bg"), QStringLiteral("bgNav"), QStringLiteral("accent"),
                    QStringLiteral("onAccent")};
    for (const Row &r : kRows)
        out << QLatin1String(r.name);
    return out;
}

void ThemeBridge::attach(QWidget *root) {
    if (!root)
        return;
    for (const QPointer<QWidget> &w : m_roots) {
        if (w == root)
            return;
    }
    m_roots.push_back(QPointer<QWidget>(root));
    applyTo(root);
}

void ThemeBridge::detach(QWidget *root) {
    for (int i = 0; i < m_roots.size(); ++i) {
        if (m_roots[i].isNull() || m_roots[i] == root) {
            m_roots.remove(i);
            --i;
        }
    }
}

void ThemeBridge::refreshAll() {
    for (int i = 0; i < m_roots.size();) {
        if (m_roots[i].isNull())
            m_roots.remove(i);
        else {
            applyTo(m_roots[i].data());
            ++i;
        }
    }
}

void ThemeBridge::applyTo(QWidget *root) {
    if (!root)
        return;
    // 窗口底色(唯一来源:令牌)
    root->setStyleSheet(QStringLiteral("QWidget#%1{background:%2;}")
                            .arg(root->objectName(), token(QStringLiteral("bg")).name()));
    // 带 sxclCard 动态属性的容器(占位页/后续卡片)按令牌重上底色
    const QColor card = token(QStringLiteral("card"));
    const QColor border = token(QStringLiteral("border"));
    const QList<QWidget *> cards = root->findChildren<QWidget *>();
    for (QWidget *w : cards) {
        if (!w->property("sxclCard").toBool())
            continue;
        w->setStyleSheet(QStringLiteral("QWidget#%1{background:%2;border:1px solid %3;"
                                        "border-radius:8px;}")
                             .arg(w->objectName(), card.name(), border.name()));
    }
    // QSS 改的是外观,自绘控件(导航按钮/图标引擎)要重新 polish + 重画
    const QList<QWidget *> all = root->findChildren<QWidget *>();
    for (QWidget *w : all) {
        w->style()->unpolish(w);
        w->style()->polish(w);
        w->update();
    }
    root->update();
}

} // namespace sxcl::ui
