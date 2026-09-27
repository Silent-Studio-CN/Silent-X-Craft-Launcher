/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "accordion.h"

#include <QEasingCurve>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>

#include "icon_button.h"
#include "icons.h"
#include "tokens.h"

namespace sxcl::ui2 {
namespace {
constexpr int kRowHeight = 34; // 一行版本的高度(逻辑像素)
constexpr int kRowGap = 4;
constexpr int kStaggerMs = 25; // §11.3:错峰 25ms/行
constexpr int kDurMs = 220;    // §4:过渡 150–220ms
} // namespace

AccordionSection::AccordionSection(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("sxcl2VersionAccordion"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(24, 18, 24, 18);
    v->setSpacing(10);

    m_title = new QLabel(QStringLiteral("收藏版本"), this);
    m_title->setObjectName(QStringLiteral("sxcl2AccordionTitle"));
    v->addWidget(m_title);

    /* 列表容器:高度由动画驱动(0 = 收起)。minimumHeight 必须显式写 0 ——
     * 否则子控件的"固定高度"会把最小高度顶上去,动画收不到 0(实测踩过)。 */
    m_body = new QWidget(this);
    m_body->setObjectName(QStringLiteral("sxcl2VersionAccordionBody"));
    m_body->setMinimumHeight(0);
    m_body->setMaximumHeight(0);
    m_bodyLay = new QVBoxLayout(m_body);
    m_bodyLay->setContentsMargins(0, 0, 0, 0);
    m_bodyLay->setSpacing(kRowGap);
    v->addWidget(m_body);

    m_all = new LinkRow(QStringLiteral("查看全部版本"), this);
    m_all->setVisible(false); // 收起时整块不占地方
    connect(m_all, &LinkRow::clicked, this, &AccordionSection::allRequested);
    v->addWidget(m_all);
}

int AccordionSection::bodyTargetHeight() const {
    const int n = m_rows.size();
    if (n <= 0)
        return 0;
    return n * kRowHeight + (n - 1) * kRowGap;
}

void AccordionSection::setRows(const QVector<VersionRow> &rows) {
    for (QWidget *row : m_rows)
        row->deleteLater();
    m_rows.clear();
    for (int i = 0; i < rows.size(); ++i) {
        auto *row = new QWidget(m_body);
        row->setObjectName(QStringLiteral("sxcl2VersionRow"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        row->setFixedHeight(kRowHeight);
        row->setProperty("index", i);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(10, 0, 10, 0);
        h->setSpacing(8);
        auto *name = new QLabel(rows[i].name, row);
        name->setObjectName(QStringLiteral("sxcl2VersionRowName"));
        h->addWidget(name);
        h->addStretch(1);
        auto *meta = new QLabel(rows[i].meta, row);
        meta->setObjectName(QStringLiteral("sxcl2VersionRowMeta"));
        h->addWidget(meta);
        row->setVisible(false); // 收起态:一行都不占地方
        m_bodyLay->addWidget(row);
        m_rows.append(row);
    }
    m_body->setMaximumHeight(m_expanded ? bodyTargetHeight() : 0);
    std::fprintf(stderr, "[sxcl-ui2] accordion: 行数=%d 目标高度=%dpx(展开态=%s)\n",
                 static_cast<int>(m_rows.size()), bodyTargetHeight(), m_expanded ? "是" : "否");
}

void AccordionSection::staggerRows() {
    QStringList delays;
    for (int i = 0; i < m_rows.size(); ++i) {
        QWidget *row = m_rows.at(i);
        const int delay = i * kStaggerMs;
        delays.append(QString::number(delay));
        if (delay == 0)
            row->setVisible(true);
        else
            QTimer::singleShot(delay, row, [row] { row->setVisible(true); }); // 一条延迟表
    }
    /* 判据:错峰真的按 25ms/行 排的(脚本按 delays=[0,25,50,...] 断言)。
     * TODO(动画基座落地后):换 Anim::stagger(m_rows, 100, kStaggerMs) —— 它是一条时间线。 */
    std::fprintf(stderr,
                 "[sxcl-ui2] accordion: 展开 %dms curve=OutCubic 行数=%d 错峰=%dms/行 "
                 "delays=[%s]\n",
                 kDurMs, static_cast<int>(m_rows.size()), kStaggerMs,
                 delays.join(QLatin1Char(',')).toUtf8().constData());
}

void AccordionSection::hideRows() {
    for (QWidget *row : m_rows)
        row->setVisible(false);
}

void AccordionSection::setExpanded(bool on) {
    if (m_expanded == on && m_anim == nullptr)
        return;
    m_expanded = on;
    if (m_anim != nullptr) {
        m_anim->stop();
        m_anim->deleteLater();
        m_anim = nullptr;
    }
    const int from = m_body->maximumHeight();
    const int to = on ? bodyTargetHeight() : 0;
    m_all->setVisible(on);
    if (on)
        staggerRows();

    m_anim = new QPropertyAnimation(m_body, "maximumHeight", this);
    m_anim->setStartValue(from);
    m_anim->setEndValue(to);
    m_anim->setDuration(kDurMs);
    m_anim->setEasingCurve(QEasingCurve(QEasingCurve::OutCubic)); // 全站唯一曲线语义:Out
    connect(m_anim, &QPropertyAnimation::finished, this, [this, on] {
        if (!on)
            hideRows();
        if (m_anim != nullptr) {
            m_anim->deleteLater();
            m_anim = nullptr;
        }
    });
    m_anim->start();
    emit expandedChanged(on);
}

/* ── "查看全部版本 ->" 那一行 ─────────────────────────────────────────── */

LinkRow::LinkRow(const QString &text, QWidget *parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setFixedHeight(28);
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    auto *label = new QLabel(text, this);
    label->setObjectName(QStringLiteral("sxcl2AccordionAllText"));
    h->addWidget(label);
    h->addWidget(new IconGlyph(uiIconPath(QStringLiteral("arrow_right.svg")), 16, this));
    h->addStretch(1);
}

void LinkRow::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton)
        emit clicked();
    QWidget::mouseReleaseEvent(event);
}

void LinkRow::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return ||
        event->key() == Qt::Key_Enter) {
        emit clicked();
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace sxcl::ui2
