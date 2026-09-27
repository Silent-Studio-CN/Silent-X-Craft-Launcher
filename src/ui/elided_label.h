/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

// 单行标签的"装不下就省略号"守门人(从 settings_page.cpp 的 ElidedLabelFilter 提出来共用,
// 主页改成"左右两栏"之后左栏会被窗口挤窄,路径那一行必须先能压缩 —— 见下面的三条口径)。
//
// 三条口径(设置页当初踩过的坑,docs/27 §12):
//   1. **绝不换行**:卡片高度是固定的,开 wordWrap 的后果是那行被挤成两行、与上下叠在一起;
//   2. **文字绝不消失**:只把横向策略改成 Ignored,布局会把标签压到 0 宽(字没了)——
//      所以必须同时给一个**显式最小宽度**;窗口再挤也留这么多;
//   3. **装不下才省略**:resize 时按当前宽度重写文案(Qt::ElideRight),宽度够时**一字不改**。
//
// 注意:settings_page.cpp 里还留着一份等价的私有实现(那一页的验收刚过,不在这轮里动它)。
// 新页面用这里的;将来谁再合并一次,把那里换成 include 本文即可。

#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QObject>
#include <QSizePolicy>
#include <QString>

namespace sxcl::ui {

/** 单个标签的守门人:resize/show 时按当前宽度重写文案。
 *  文案会被外部改(路径 / 版本名 / 状态都会 setText),所以这里认"我们上次写进去的那一版" ——
 *  当前文本与它不一致 = 外部刚 setText 了新文案,把它记成新的"完整版"。 */
class ElidedLabelFilter : public QObject {
public:
    explicit ElidedLabelFilter(QLabel *label) : QObject(label), m_label(label) {
        if (m_label != nullptr)
            m_label->installEventFilter(this); // 挂上才收得到 resize(父对象关系不管事件)
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Show)
            apply();
        return QObject::eventFilter(watched, event);
    }

private:
    void apply() {
        if (m_busy || m_label == nullptr)
            return;
        const QString current = m_label->text();
        if (current != m_shown)
            m_full = current; // 外部 setText 了新文案
        const int available = m_label->width();
        if (available <= 0 || m_full.isEmpty()) {
            m_shown.clear();
            return;
        }
        const QString shown =
            QFontMetrics(m_label->font()).elidedText(m_full, Qt::ElideRight, available);
        m_shown = shown;
        if (shown == current)
            return;
        m_busy = true;
        m_label->setText(shown); // 只有真的变了才写回,避免与布局互相触发
        m_busy = false;
    }

    QLabel *m_label = nullptr;
    QString m_full;  // 完整文案
    QString m_shown; // 我们最后写进去的那一版(用来区分"外部改的"与"我们改的")
    bool m_busy = false;
};

/** 把一个单行标签变成"可压缩 + 省略号"的(上面三条口径一次做全)。 */
inline void makeLabelElide(QLabel *label, int minWidth) {
    if (label == nullptr)
        return;
    label->setWordWrap(false);                   // ① 不换行
    label->setMinimumWidth(minWidth);            // ② 文字不消失
    QSizePolicy policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Ignored); // ③ sizeHint 不再当最小宽度
    label->setSizePolicy(policy);
    new ElidedLabelFilter(label);
}

} // namespace sxcl::ui
