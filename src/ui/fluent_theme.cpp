/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "fluent_theme.h"

#include "libqf.h" // libqf = 本工程的 qf 引擎(主题/样式/控件),统一由本层驱动

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QIODevice>
#include <QPalette>
#include <QStringList>

#include <algorithm>

namespace sxcl::ui {
namespace {

// qf 默认字体族（qfluentwidgets/common/config.py:271 fontFamilies）
const char *kFontFamiliesQss = "'Segoe UI','Microsoft YaHei','PingFang SC'";

struct TokenDef {
    const char *name;
    const char *light;
    const char *dark;
};

// 逐字抄自 Python src/app/theme.py:66-116
const TokenDef kTokens[] = {
    {"bg", "#f3f3f3", "#202020"},
    {"bgNav", "#f0f4f9", "#19212a"},
    {"card", "#ffffff", "#2b2b2b"},
    {"cardHover", "#f7f7f7", "#333333"},
    {"border", "#e1e1e1", "#3d3d3d"},
    {"borderStrong", "#c8c8c8", "#555555"},
    {"separator", "#e8e8e8", "#383838"},
    {"hover", "rgba(0, 0, 0, 0.05)", "rgba(255, 255, 255, 0.06)"},
    {"hoverStrong", "rgba(0, 0, 0, 0.09)", "rgba(255, 255, 255, 0.11)"},
    {"hoverBg", "rgba(0, 0, 0, 0.05)", "rgba(255, 255, 255, 0.05)"},
    {"hoverBgStrong", "rgba(0, 0, 0, 0.10)", "rgba(255, 255, 255, 0.10)"},
    {"text", "#1a1a1a", "#ffffff"},
    {"textSecondary", "#5d5d5d", "#c7c7c7"},
    {"textTertiary", "#8a8a8a", "#9a9a9a"},
    {"textDisabled", "#a6a6a6", "#6b6b6b"},
    {"inputBg", "#ffffff", "#2d2d2d"},
    {"inputBorder", "#d0d0d0", "#4a4a4a"},
    {"track", "rgba(0, 0, 0, 0.10)", "rgba(255, 255, 255, 0.14)"},
    {"success", "#0f7b0f", "#6ccb5f"},
    {"warning", "#9d5d00", "#fce100"},
    {"danger", "#c42b1c", "#ff99a4"},
    {"info", "#005fb8", "#60cdff"},
    {"onAccent", "#ffffff", "#000000"},
};

// qf ThemeColor.color()：HSV 空间的缩放（深色分支先 s *= 0.84、v = 1）
QColor accentByIndex(const QColor &base, int index, bool dark) {
    float h = 0, s = 0, v = 0, a = 1;
    base.getHsvF(&h, &s, &v, &a);
    double ns = s, nv = v;
    if (dark) {
        ns = s * 0.84;
        nv = 1.0;
        switch (index) {
        case 1: nv *= 0.9; break;                                   // Dark1
        case 2: ns *= 0.977; nv *= 0.82; break;                     // Dark2
        case 3: ns *= 0.95; nv *= 0.7; break;                       // Dark3
        case 4: ns *= 0.92; break;                                  // Light1
        case 5: ns *= 0.78; break;                                  // Light2
        case 6: ns *= 0.65; break;                                  // Light3
        default: break;                                             // Primary
        }
    } else {
        switch (index) {
        case 1: nv *= 0.75; break;
        case 2: ns *= 1.05; nv *= 0.5; break;
        case 3: ns *= 1.1; nv *= 0.4; break;
        case 4: nv *= 1.05; break;
        case 5: ns *= 0.75; nv *= 1.05; break;
        case 6: ns *= 0.65; nv *= 1.05; break;
        default: break;
        }
    }
    QColor out = QColor::fromHsvF(h, std::min(ns, 1.0), std::min(nv, 1.0));
    out.setAlpha(base.alpha());
    return out;
}

QString readTextFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    const QString text = QString::fromUtf8(f.readAll());
    f.close();
    return text;
}

} // namespace

FluentTheme::FluentTheme()
    : m_dark(true), m_accent(QStringLiteral("#0067c0")) {
    // 默认强调色：Python 端把历史遗留的 #009faa 修正为 #0067c0（launcher_config.py:317-327）
    setAccent(m_accent);
}

FluentTheme &FluentTheme::instance() {
    static FluentTheme inst;
    return inst;
}

bool FluentTheme::isDark() const { return m_dark; }

void FluentTheme::setDark(bool dark) {
    if (m_dark == dark)
        return;
    m_dark = dark;
    rebuild();
}

void FluentTheme::toggle() { setDark(!m_dark); }

QColor FluentTheme::accent() const {
    // Python theme.py:135/168 —— `data["accent"] = themeColor().name()`:
    // 页面拿到的 accent 是 qf **推导后**的颜色,而配置里存的是**原始色**。
    // 参考机配置原始色 #c044a3 → 暗色推导 → #ff75df(参考图上的主色底)。
    // libqf 已按 qf style_sheet.py:463-504 实现推导,这里直接取它的结果,保证
    // libqf 自己画的控件 与 我们 QSS/自绘 用的强调色完全同源。
    if (fluent::FluentStyle *st = fluent::FluentStyle::instance())
        return st->themeColor();
    return accentVariant(0);
}

void FluentTheme::setAccent(const QColor &c) {
    if (c.isValid())
        m_accent = c;
}

QColor FluentTheme::accentVariant(int index) const {
    return accentByIndex(m_accent, index, m_dark);
}

QColor FluentTheme::parseCssColor(const QString &raw) {
    const QString v = raw.trimmed();
    if (v.startsWith(QLatin1String("rgba"), Qt::CaseInsensitive) ||
        v.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive)) {
        const int l = v.indexOf(QLatin1Char('('));
        const int r = v.indexOf(QLatin1Char(')'));
        if (l < 0 || r <= l)
            return QColor();
        const QStringList parts = v.mid(l + 1, r - l - 1).split(QLatin1Char(','));
        if (parts.size() < 3)
            return QColor();
        const int rr = qRound(parts[0].trimmed().toDouble());
        const int gg = qRound(parts[1].trimmed().toDouble());
        const int bb = qRound(parts[2].trimmed().toDouble());
        double alpha = 1.0;
        if (parts.size() >= 4) {
            const QString a = parts[3].trimmed();
            alpha = a.contains(QLatin1Char('.')) ? a.toDouble() : a.toDouble() / 255.0;
        }
        return QColor(rr, gg, bb, int(qRound(alpha * 255.0)));
    }
    return QColor(v);
}

const ThemeTokens &FluentTheme::tokens() const {
    static ThemeTokens dark;
    static ThemeTokens light;
    static bool inited = false;
    ThemeTokens &t = m_dark ? dark : light;
    if (!inited) {
        auto fill = [](ThemeTokens &out, bool isDark) {
            for (const TokenDef &d : kTokens) {
                const QString raw = QString::fromLatin1(isDark ? d.dark : d.light);
                const QString name = QString::fromLatin1(d.name);
                if (name == QLatin1String("hover"))
                    out.hover = raw;
                else if (name == QLatin1String("hoverStrong"))
                    out.hoverStrong = raw;
                else if (name == QLatin1String("hoverBg"))
                    out.hoverBg = raw;
                else if (name == QLatin1String("hoverBgStrong"))
                    out.hoverBgStrong = raw;
                else if (name == QLatin1String("track"))
                    out.track = raw;
                else {
                    const QColor c = parseCssColor(raw);
                    if (name == QLatin1String("bg")) out.bg = c;
                    else if (name == QLatin1String("bgNav")) out.bgNav = c;
                    else if (name == QLatin1String("card")) out.card = c;
                    else if (name == QLatin1String("cardHover")) out.cardHover = c;
                    else if (name == QLatin1String("border")) out.border = c;
                    else if (name == QLatin1String("borderStrong")) out.borderStrong = c;
                    else if (name == QLatin1String("separator")) out.separator = c;
                    else if (name == QLatin1String("text")) out.text = c;
                    else if (name == QLatin1String("textSecondary")) out.textSecondary = c;
                    else if (name == QLatin1String("textTertiary")) out.textTertiary = c;
                    else if (name == QLatin1String("textDisabled")) out.textDisabled = c;
                    else if (name == QLatin1String("inputBg")) out.inputBg = c;
                    else if (name == QLatin1String("inputBorder")) out.inputBorder = c;
                    else if (name == QLatin1String("success")) out.success = c;
                    else if (name == QLatin1String("warning")) out.warning = c;
                    else if (name == QLatin1String("danger")) out.danger = c;
                    else if (name == QLatin1String("info")) out.info = c;
                    else if (name == QLatin1String("onAccent")) out.onAccent = c;
                }
            }
        };
        fill(dark, true);
        fill(light, false);
        inited = true;
    }
    t.accent = accent(); // 推导后的强调色(与 theme.py 的 tokens()["accent"] 同义)
    return t;
}

namespace {

// theme.py 的键名是 snake_case(text_tertiary),本层结构体字段是 CamelCase(textTertiary)。
// 页面作者两边混着写时,旧实现会**静默返回空串**(颜色丢失、悬停/卡片底不画)——
// 实测:versions 页因此有 4.5 个百分点的差异被吃掉。这里做归一化,两种写法都认。
QString normalizeTokenName(const QString &name) {
    static const struct { const char *snake; const char *camel; } kAliases[] = {
        {"bg_nav", "bgNav"},         {"card_hover", "cardHover"},
        {"border_strong", "borderStrong"}, {"hover_strong", "hoverStrong"},
        {"hover_bg", "hoverBg"},     {"hover_bg_strong", "hoverBgStrong"},
        {"text_secondary", "textSecondary"}, {"text_tertiary", "textTertiary"},
        {"text_disabled", "textDisabled"},   {"input_bg", "inputBg"},
        {"input_border", "inputBorder"},     {"on_accent", "onAccent"},
    };
    for (const auto &a : kAliases) {
        if (name == QLatin1String(a.snake))
            return QString::fromLatin1(a.camel);
    }
    return name;
}

// Qt 的 QSS 解析器**吃不下来带空格的 rgba**:`rgba(255, 255, 255, 0.05)` 会让整条声明被丢弃
// (实测:卡片不画底、悬停无反应)。令牌字符串本身是带空格的写法(theme.py 原文如此),
// 所以在**进 QSS 之前**去掉函数参数里的空格,颜色值一个比特都不变。
QString qssSafe(const QString &raw) {
    const int l = raw.indexOf(QLatin1Char('('));
    const int r = raw.lastIndexOf(QLatin1Char(')'));
    if (l < 0 || r <= l)
        return raw;
    const QString head = raw.left(l + 1);
    if (!head.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive))
        return raw;
    QString inner = raw.mid(l + 1, r - l - 1);
    inner.remove(QLatin1Char(' '));
    return head + inner + raw.mid(r);
}

} // namespace

QString FluentTheme::tokenText(const QString &name) const {
    const ThemeTokens &t = tokens();
    const QString key = normalizeTokenName(name); // snake_case 也认(页面作者常照 theme.py 写)
    QString raw;
    if (key == QLatin1String("hover")) raw = t.hover;
    else if (key == QLatin1String("hoverStrong")) raw = t.hoverStrong;
    else if (key == QLatin1String("hoverBg")) raw = t.hoverBg;
    else if (key == QLatin1String("hoverBgStrong")) raw = t.hoverBgStrong;
    else if (key == QLatin1String("track")) raw = t.track;
    else if (key == QLatin1String("bg")) raw = t.bg.name();
    else if (key == QLatin1String("bgNav")) raw = t.bgNav.name();
    else if (key == QLatin1String("card")) raw = t.card.name();
    else if (key == QLatin1String("cardHover")) raw = t.cardHover.name();
    else if (key == QLatin1String("border")) raw = t.border.name();
    else if (key == QLatin1String("borderStrong")) raw = t.borderStrong.name();
    else if (key == QLatin1String("separator")) raw = t.separator.name();
    else if (key == QLatin1String("text")) raw = t.text.name();
    else if (key == QLatin1String("textSecondary")) raw = t.textSecondary.name();
    else if (key == QLatin1String("textTertiary")) raw = t.textTertiary.name();
    else if (key == QLatin1String("textDisabled")) raw = t.textDisabled.name();
    else if (key == QLatin1String("inputBg")) raw = t.inputBg.name();
    else if (key == QLatin1String("inputBorder")) raw = t.inputBorder.name();
    else if (key == QLatin1String("success")) raw = t.success.name();
    else if (key == QLatin1String("warning")) raw = t.warning.name();
    else if (key == QLatin1String("danger")) raw = t.danger.name();
    else if (key == QLatin1String("info")) raw = t.info.name();
    else if (key == QLatin1String("onAccent")) raw = t.onAccent.name();
    else if (key == QLatin1String("accent")) raw = t.accent.name();
    else return QString();
    // 进 QSS 前统一成 Qt 认的写法(rgba 内不能有空格),颜色值不变
    return qssSafe(raw);
}

QStringList FluentTheme::tokenNames() const {
    QStringList out;
    for (const TokenDef &d : kTokens)
        out << QString::fromLatin1(d.name);
    out << QStringLiteral("accent");
    return out;
}

void FluentTheme::setThemeDir(const QString &dir) { m_dir = dir; }

QString FluentTheme::themeDir() const { return m_dir; }

bool FluentTheme::resolveThemeDir() {
    QStringList candidates;
#ifdef SXCL_UI_THEME_DIR
    candidates << QString::fromLatin1(SXCL_UI_THEME_DIR);
#endif
    const QString env = qEnvironmentVariable("SXCL_THEME_DIR");
    if (!env.isEmpty())
        candidates << env;
    candidates << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/theme"));
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c + QStringLiteral("/qf_exact/dark")) ||
            QFileInfo::exists(c + QStringLiteral("/qf/dark"))) {
            m_dir = c;
            return true;
        }
    }
    return false;
}

QString FluentTheme::fontFamiliesQss() { return QString::fromLatin1(kFontFamiliesQss); }

bool FluentTheme::loadQss() {
    m_qfQss.clear();
    m_qssFiles = 0;
    if (m_dir.isEmpty() && !resolveThemeDir())
        return false;

    const QString theme = m_dark ? QStringLiteral("dark") : QStringLiteral("light");
    QString dir = m_dir + QStringLiteral("/qf_exact/") + theme;
    if (!QFileInfo::exists(dir))
        dir = m_dir + QStringLiteral("/qf/") + theme;

    QDir d(dir);
    if (!d.exists())
        return false;

    const QStringList files = d.entryList(QStringList() << QStringLiteral("*.qss"), QDir::Files, QDir::Name);
    QString all;
    for (const QString &name : files) {
        const QString qss = readTextFile(d.filePath(name));
        if (qss.isEmpty())
            continue;
        all += QStringLiteral("/* ==== ") + name + QStringLiteral(" ==== */\n") + qss + QStringLiteral("\n");
        ++m_qssFiles;
    }
    if (all.isEmpty())
        return false;

    // 占位符替换（qf common/style_sheet.py:84-100 的等价物）
    all.replace(QLatin1String("--FontFamilies"), fontFamiliesQss());
    static const char *kVariants[] = {"--ThemeColorPrimary", "--ThemeColorDark1", "--ThemeColorDark2",
                                      "--ThemeColorDark3", "--ThemeColorLight1", "--ThemeColorLight2",
                                      "--ThemeColorLight3"};
    for (int i = 0; i < 7; ++i)
        all.replace(QLatin1String(kVariants[i]), accentVariant(i).name());
    m_qfQss = all;
    return true;
}

bool FluentTheme::qssLoaded() const { return !m_qfQss.isEmpty(); }

int FluentTheme::qssFileCount() const { return m_qssFiles; }

QString FluentTheme::appStyleSheet() const {
    // 逐条对应 Python src/app/theme.py:191-228 global_qss() + main_window.py:298。
    // 少一条都会让整片区域偏色(踩过:#272727 就是漏了 QStackedWidget transparent 那条)。
    const ThemeTokens &t = tokens();
    const QString card = t.card.name();
    const QString border = t.border.name();
    const QString text = t.text.name();
    QString qss;
    qss += QStringLiteral("FluentWindow, FluentWindowBase { background-color: %1; }\n").arg(t.bg.name());
    qss += QStringLiteral("QWidget { color: %1; }\n").arg(text);
    qss += QStringLiteral("QScrollArea, QStackedWidget, QWidget#qt_scrollarea_viewport { background: transparent; }\n");
    qss += QStringLiteral("QToolTip { background: %1; color: %2; border: 1px solid %3; padding: 4px 6px; }\n")
               .arg(card, text, border);
    qss += QStringLiteral(
               "QLineEdit, QPlainTextEdit, QTextEdit { background: %1; color: %2; border: 1px solid %3;"
               " border-radius: 6px; padding: 6px 10px; selection-background-color: %4;"
               " selection-color: %5; }\n"
               "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus { border-color: %4; }\n")
               .arg(t.inputBg.name(), text, t.inputBorder.name(), t.accent.name(), t.onAccent.name());
    qss += QStringLiteral(
               "QListView, QListWidget, QTreeView, QTableView { background: transparent; color: %1;"
               " border: none; selection-background-color: %2; selection-color: %1; }\n")
               .arg(text, t.hoverStrong);
    qss += QStringLiteral(
               "QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }\n"
               "QScrollBar::handle:vertical { background: %1; border-radius: 4px; min-height: 28px; }\n"
               "QScrollBar::handle:vertical:hover { background: %2; }\n"
               "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }\n"
               "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }\n"
               "QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }\n"
               "QScrollBar::handle:horizontal { background: %1; border-radius: 4px; min-width: 28px; }\n")
               .arg(t.borderStrong.name(), t.textTertiary.name());
    qss += QStringLiteral(
               "QProgressBar { background: %1; border: none; border-radius: 3px; color: %2; }\n"
               "QProgressBar::chunk { background: %3; border-radius: 3px; }\n")
               .arg(t.track, text, t.accent.name());
    qss += QStringLiteral("QMenu { background: %1; color: %2; border: 1px solid %3; }\n"
                          "QMenu::item:selected { background: %4; }\n")
               .arg(card, text, border, t.hoverStrong);
    return qss;
}
void FluentTheme::apply(QApplication *app) {
    loadQss();

    // 先把 libqf 的引擎对齐(它负责 qf 组件的样式与原生调色板),再套 SXCL 令牌
    if (fluent::FluentStyle *st = fluent::FluentStyle::instance()) {
        st->setTheme(m_dark ? fluent::Theme::Dark : fluent::Theme::Light);
        st->setThemeColor(m_accent);
        st->applyAppPalette();
        st->reapplyAll();
    }

    const ThemeTokens &t = tokens();
    QPalette pal;
    pal.setColor(QPalette::Window, t.bg);
    pal.setColor(QPalette::WindowText, t.text);
    pal.setColor(QPalette::Base, t.inputBg);
    pal.setColor(QPalette::AlternateBase, t.card);
    pal.setColor(QPalette::Text, t.text);
    pal.setColor(QPalette::PlaceholderText, t.textTertiary);
    pal.setColor(QPalette::Button, t.card);
    pal.setColor(QPalette::ButtonText, t.text);
    pal.setColor(QPalette::Highlight, t.accent);
    pal.setColor(QPalette::HighlightedText, t.onAccent);
    pal.setColor(QPalette::ToolTipBase, t.card);
    pal.setColor(QPalette::ToolTipText, t.text);
    pal.setColor(QPalette::Disabled, QPalette::Text, t.textDisabled);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, t.textDisabled);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, t.textDisabled);

    // **不要**设应用字体:Python main.py 只做 app.setStyle("Fusion"),从不 setFont;
    // qf 的字体只通过 QSS 的 --FontFamilies 影响 qf 控件,原生控件用系统默认
    // (Microsoft YaHei UI 9pt)。我们以前在这里设 Segoe UI,导致原生控件高 1px、
    // 页面自上而下累计错位 1~2px(临时页代理实测:返回键 33→34、输入框 36→37)。

    if (app) {
        app->setPalette(pal);
        app->setStyleSheet(appStyleSheet());
    }
    emit changed();
}

void FluentTheme::rebuild() { apply(qApp); }

QString FluentTheme::cardQss(int radius, const QString &padding) const {
    return QStringLiteral("QWidget { background: %1; border: none; border-radius: %2px; padding: %3; }")
        .arg(tokenText(QStringLiteral("card")))
        .arg(radius)
        .arg(padding);
}

QString FluentTheme::sectionCardQss(int radius) const {
    return QStringLiteral("QWidget#SectionCard { background: %1; border: 1px solid %2; border-radius: %3px; }"
                          "QWidget#SectionCard:hover { border-color: %4; }")
        .arg(tokenText(QStringLiteral("card")), tokenText(QStringLiteral("border")))
        .arg(radius)
        .arg(tokenText(QStringLiteral("accent")));
}

QString FluentTheme::mutedQss(int size, const QString &margin) const {
    return QStringLiteral("color: %1; font-size: %2px; margin: %3;")
        .arg(tokenText(QStringLiteral("textTertiary")))
        .arg(size)
        .arg(margin);
}

QString FluentTheme::ghostButtonQss() const {
    return QStringLiteral("QPushButton { border: none; background: transparent; color: %1; font-weight: bold; }"
                          "QPushButton:hover { color: %2; }")
        .arg(tokenText(QStringLiteral("textTertiary")), tokenText(QStringLiteral("danger")));
}

QString FluentTheme::chipQss(const QString &colorToken) const {
    const QString c = tokenText(colorToken);
    return QStringLiteral("QLabel { color: %1; border: 1px solid %1; border-radius: 5px; padding: 1px 8px; }").arg(c);
}

QColor FluentTheme::badgeColor(const QString &state) const {
    if (state == QLatin1String("running")) return tokens().accent;
    if (state == QLatin1String("done")) return tokens().success;
    if (state == QLatin1String("failed")) return tokens().danger;
    if (state == QLatin1String("warn")) return tokens().warning;
    return tokens().textTertiary;
}

} // namespace sxcl::ui