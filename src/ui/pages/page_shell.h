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
#include <QHBoxLayout>
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
        /* **去掉 ScrollArea 那 1 逻辑像素的原生 frame**(QScrollArea 默认 QFrame::StyledPanel,
         * frameWidth()==1 -> viewport 从 (1,1) 起算)。为什么必须去:
         *   页面里的两栏版式(beginSideLayout)要求侧2 栏**贴着页面左边缘**,而页面自己的
         *   那 1px frame 会把它整体推到 +1 —— 真机 dump 实测(sxcl-ui、dpr=1.5、1100x750、
         *   SXCL_UI_ROUTE=select):ScrollArea #sxclPage_select (49,49) 而里面的
         *   NavPanel #sxclVersionFolderNav (50,50),差的正是这 1px;截图扫列也能量到
         *   页面左缘那条更深的 1px 线(浅色 243/219/187,那条 187 就是 frame)。
         *   去掉之后两条栏的左边线才是同一条(用户 2026-09-26:「侧1 与侧2 要严丝合缝」)。
         * 分隔线**不靠控件**:由外壳(MainWindow::paintEvent)用 QPen(宽度 0 = cosmetic)
         * 画 1 个**设备**像素 —— docs/27 §12:1 逻辑像素的控件线在 dpr=1.5 上会占 1~2 个
         * 物理像素(实测"一粗一细"),QFrame::VLine 还会因 Fusion 的浅色调色板变白线。 */
        setFrameShape(QFrame::StyledPanel); // TEMP EXPERIMENT
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
        /* 空副标题**不占版面**:用户 2026-09-26 把"官方 / 镜像双路 · 静默安装"那句废话删了,
         * 不隐藏的话页面上会留一行空白把内容整体顶下去(设置页早就是这么做的:m_subtitle->hide())。 */
        if (subtitle.isEmpty())
            m_subtitle->hide();

        m_box->addWidget(m_title);                            // :59
        m_box->addWidget(m_subtitle);                         // :60
    }

    QWidget *view() const { return m_view; }
    void addContent(QWidget *w) { m_box->addWidget(w); }      // :62-63
    void addStretch() { m_box->addStretch(1); }               // :65-66
    // 把整块内容一次装进一个横向容器(下载页那种"左栏 + 右内容区"用得上)
    QVBoxLayout *box() const { return m_box; }

    /** **两栏版式**:左栏(页面自己的侧边栏)**从内容区顶部开始**,标题/副标题挪到右列上方。
     *
     *  用户 2026-09-22 晚连着两次点名:「侧2 还是被上方文字顶的向下移动了」——
     *  以前整页是"标题 + 副标题 + 内容"竖着排,左栏自然被上面那两行文字顶下去十几到上百像素,
     *  与窗口那条主侧边栏(侧1)怎么都对不齐。现在:
     *    * 左外边距给 **0** —— 左栏贴着页面左边缘,视觉上与侧1 连成一条;
     *    * 标题只在右列上方(右列自己留 24px 顶距),左栏不跟着动。
     *  返回右列的竖布局:后续内容加进它(**不要再调 addContent / addStretch**)。
     *  side 可空(只要版式不要左栏)。 */
    QVBoxLayout *beginSideLayout(QWidget *side) {
        m_box->removeWidget(m_title);
        m_box->removeWidget(m_subtitle);
        m_box->setContentsMargins(0, 0, 28, 24);
        auto *row = new QHBoxLayout();
        /* **零间距**:左栏(侧2)与右内容之间不留缝 —— 用户 2026-09-26 一直在说
         * 「侧1 与侧2 要严丝合缝」;以前这里 spacing=12,加上外层的 margin,
         * 就是那道"肉眼可见的隔阂"。分隔线改由外壳用 QPen(width=0) 画 1 个设备像素。 */
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        if (side != nullptr) {
            row->addWidget(side, 0);
        }
        auto *right = new QWidget(m_view);
        right->setStyleSheet(QStringLiteral("background: transparent;"));
        auto *rightLay = new QVBoxLayout(right);
        /* 顶距 24 -> 0:内容列与侧2 **同一个顶线**开始(用户 2026-09-26 反复说
         * 「侧2 被那行字压下去」;标题仍在,只是不再给内容列再加一层顶距)。 */
        rightLay->setContentsMargins(0, 0, 0, 0);
        rightLay->setSpacing(16);
        rightLay->setAlignment(Qt::AlignTop);
        rightLay->addWidget(m_title);
        rightLay->addWidget(m_subtitle);
        row->addWidget(right, 1);
        m_box->addLayout(row, 1);
        return rightLay;
    }

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
