/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 流式布局（用户 2026-09-27 定的一条**总则**，见 docs/25 §「控件必须完整显示自己」）：
//   「所有文本框都自身独立为一个元素，独立的元素必须保证自己能完全显示，除非窗口真的小于
//    独立显示完之后的大小。」放不下就**换行**，不许把控件压扁/裁切。
//
// 与 Qt 官方 FlowLayout 示例同一套做法，只多两条：
//   * 每一项的宽度**永不低于它自己的 sizeHint**（不是只看 minimumSize —— 很多控件的
//     minimumSize 是 0，一压就切字；"勾选框被压成 41px、文字被切掉"就是这个成因）；
//   * 带 Expanding 的项（搜索框）吃掉这一行**剩下的**宽度，所以它能撑满一行，
//     而其它控件保持自然宽度。
#pragma once

#include <QList>
#include <QLayout>

namespace sxcl::ui {

class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget *parent = nullptr, int margin = 0, int hSpacing = 8, int vSpacing = 8);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect &rect) override;

    /** 每一行都**靠右**排(默认关:不设时行为与以前逐像素一致 —— 模组页筛选区就是那个口径)。
     *  两行卡右侧的控件列要它:一行放不下换行之后,第二行也得贴右边缘,
     *  否则只有第一行靠右、后面的行全掉到最左边(实测 900x600 的内置 JRE 卡就是那样)。 */
    void setAlignRight(bool on) { m_alignRight = on; }
    bool alignRight() const { return m_alignRight; }

private:
    int doLayout(const QRect &rect, bool testOnly) const;
    QList<QLayoutItem *> m_items;
    int m_hSpace;
    int m_vSpace;
    bool m_alignRight = false;
};

} // namespace sxcl::ui
