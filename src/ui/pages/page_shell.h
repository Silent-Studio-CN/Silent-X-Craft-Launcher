/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

// ── 页面外壳 ──
// 与各页里那份匿名 namespace 的 PageShell **同一套几何**(标题 + 副标题 + 可滚动内容区,
// 内容区底色钉令牌 bg)。新页面(下载/团队/更多/版本选择,见 docs/25)共用这一份;
// 老页面各自还留着自己的副本 —— 迁移是**逐页**做的,一次全改风险太大。
//
// 对应 Python src/app/common/base_page.py:BasePage(ScrollArea 子类)。

#include "fluent_theme.h"
#include "theme_bridge.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "fluent/fluent_labels.h"
#include "fluent/fluent_scroll.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QColor>
#include <QLatin1String>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace sxcl::ui {

class PageShell : public ScrollArea {
public:
    PageShell(const QString &title, const QString &subtitle, const QString &objectName,
              QWidget *parent)
        : ScrollArea(parent) {
        setObjectName(objectName);                            // base_page.py:42
        setWidgetResizable(true);                             // base_page.py:43
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // base_page.py:44
        // 页面底色钉令牌 bg(#202020):参考图的内容区就是它,不钉会露出内容栈那层半透明白。
        setStyleSheet(QStringLiteral("QScrollArea { background: %1; }")
                          .arg(FluentTheme::instance().tokens().bg.name()));

        m_view = new QWidget(this);                                          // :46
        m_view->setStyleSheet(QStringLiteral("background: transparent;"));   // :47
        setWidget(m_view);                                                   // :48

        m_box = new QVBoxLayout(m_view);                                     // :50-53
        m_box->setContentsMargins(28, 24, 28, 24);
        m_box->setSpacing(16);
        m_box->setAlignment(Qt::AlignTop);

        m_title = new TitleLabel(title, m_view);              // :55
        m_subtitle = new SubtitleLabel(subtitle, m_view);     // :56
        m_subtitle->setTextColor(QColor(0x60, 0x60, 0x60), QColor(0xAA, 0xAA, 0xAA)); // :57

        m_box->addWidget(m_title);                            // :59
        m_box->addWidget(m_subtitle);                         // :60
    }

    QWidget *view() const { return m_view; }
    void addContent(QWidget *w) { m_box->addWidget(w); }      // :62-63
    void addStretch() { m_box->addStretch(1); }               // :65-66
    // 把整块内容一次装进一个横向容器(下载页那种"左栏 + 右内容区"用得上)
    QVBoxLayout *box() const { return m_box; }

private:
    QWidget *m_view = nullptr;
    QVBoxLayout *m_box = nullptr;
    TitleLabel *m_title = nullptr;
    SubtitleLabel *m_subtitle = nullptr;
};

// 令牌取色(与各页里那对 tokenText/tokenColor 同义,名字加前缀避免与匿名 namespace 撞)
inline QString pageTokenText(const char *name) {
    return FluentTheme::instance().tokenText(QLatin1String(name));
}
inline QColor pageTokenColor(const char *name) {
    return ThemeBridge::instance().token(QLatin1String(name));
}

} // namespace sxcl::ui
