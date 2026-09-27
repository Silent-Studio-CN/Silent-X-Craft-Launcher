/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 流式布局的实现（口径见 flow_layout.h）。

#include "flow_layout.h"

#include <QStyle>
#include <QVector>
#include <QWidget>

namespace sxcl::ui {
namespace {

int smartSpacing(const QLayout *layout, QStyle::PixelMetric pm) {
    QObject *parent = layout->parent();
    if (parent == nullptr) {
        return -1;
    }
    if (parent->isWidgetType()) {
        auto *widget = static_cast<QWidget *>(parent);
        return widget->style()->pixelMetric(pm, nullptr, widget);
    }
    return static_cast<QLayout *>(parent)->spacing();
}

/** 这一项要占的宽：自己的 sizeHint 与 minimumSize 里**大的那个** —— 永远不比自己需要的窄。 */
QSize itemNatural(const QLayoutItem *item) {
    return item->sizeHint().expandedTo(item->minimumSize());
}

bool expandingH(const QLayoutItem *item) {
    return (item->expandingDirections() & Qt::Horizontal) != 0;
}

} // namespace

FlowLayout::FlowLayout(QWidget *parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout() {
    while (QLayoutItem *item = takeAt(0)) {
        delete item;
    }
}

void FlowLayout::addItem(QLayoutItem *item) { m_items.append(item); }

int FlowLayout::horizontalSpacing() const {
    if (m_hSpace >= 0) {
        return m_hSpace;
    }
    const int smart = smartSpacing(this, QStyle::PM_LayoutHorizontalSpacing);
    return smart >= 0 ? smart : 8;
}

int FlowLayout::verticalSpacing() const {
    if (m_vSpace >= 0) {
        return m_vSpace;
    }
    const int smart = smartSpacing(this, QStyle::PM_LayoutVerticalSpacing);
    return smart >= 0 ? smart : 8;
}

Qt::Orientations FlowLayout::expandingDirections() const { return Qt::Horizontal; }

bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int width) const {
    return doLayout(QRect(0, 0, width, 0), true);
}

int FlowLayout::count() const { return int(m_items.size()); }

QLayoutItem *FlowLayout::itemAt(int index) const {
    return (index >= 0 && index < m_items.size()) ? m_items.at(index) : nullptr;
}

QLayoutItem *FlowLayout::takeAt(int index) {
    if (index < 0 || index >= m_items.size()) {
        return nullptr;
    }
    return m_items.takeAt(index);
}

QSize FlowLayout::minimumSize() const {
    /* 最小宽 = **最宽的那一项**（不是所有项之和）：这样窗口再窄也只是换行，
     * 容器自己不会比任何一个控件还窄 —— "独立元素保证能完全显示"就落在这一条上。 */
    int widest = 0;
    int tallest = 0;
    for (const QLayoutItem *item : m_items) {
        const QSize nat = itemNatural(item);
        widest = qMax(widest, nat.width());
        tallest = qMax(tallest, nat.height());
    }
    int left = 0, top = 0, right = 0, bottom = 0;
    getContentsMargins(&left, &top, &right, &bottom);
    return QSize(widest + left + right, tallest + top + bottom);
}

QSize FlowLayout::sizeHint() const { return minimumSize(); }

void FlowLayout::setGeometry(const QRect &rect) {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const {
    int left = 0, top = 0, right = 0, bottom = 0;
    getContentsMargins(&left, &top, &right, &bottom);
    const QRect effective = rect.adjusted(left, top, -right, -bottom);
    const int hSpace = horizontalSpacing();
    const int vSpace = verticalSpacing();
    const int lineMax = effective.right() + 1;

    /* 第一趟：按"自然宽"分行（放不下就换行，绝不压缩） */
    QVector<QList<int>> lines;
    QVector<int> lineWidths;
    QList<int> current;
    int currentWidth = 0;
    int x = effective.x();
    for (int i = 0; i < m_items.size(); ++i) {
        const int w = itemNatural(m_items.at(i)).width();
        if (!current.isEmpty() && x + w > lineMax) {
            lines.append(current);
            lineWidths.append(currentWidth);
            current.clear();
            currentWidth = 0;
            x = effective.x();
        }
        current.append(i);
        currentWidth += w + (current.size() > 1 ? hSpace : 0);
        x += w + hSpace;
    }
    if (!current.isEmpty()) {
        lines.append(current);
        lineWidths.append(currentWidth);
    }

    /* 第二趟：摆位置。带 Expanding 的项吃掉这一行剩下的宽度（搜索框撑满一行）。 */
    int y = effective.y();
    for (int li = 0; li < lines.size(); ++li) {
        const QList<int> &line = lines.at(li);
        int lineHeight = 0;
        int expandingCount = 0;
        for (int idx : line) {
            lineHeight = qMax(lineHeight, itemNatural(m_items.at(idx)).height());
            if (expandingH(m_items.at(idx))) {
                ++expandingCount;
            }
        }
        const int extra = effective.width() - lineWidths.at(li);
        const int grow = (expandingCount > 0 && extra > 0) ? extra / expandingCount : 0;
        int lx = effective.x();
        for (int idx : line) {
            QLayoutItem *item = m_items.at(idx);
            const QSize nat = itemNatural(item);
            int w = nat.width();
            if (expandingH(item) && grow > 0) {
                w += grow;
            }
            if (!testOnly) {
                item->setGeometry(QRect(QPoint(lx, y), QSize(w, nat.height())));
            }
            lx += w + hSpace;
        }
        y += lineHeight + vSpace;
    }
    const int used = y - effective.y() - (lines.isEmpty() ? 0 : vSpace);
    return qMax(used, 0);
}

} // namespace sxcl::ui
