/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 图标钮(docs/27 §11.1 按钮最少化:能猜到的动作只给图标,人话进 tooltip —— 绝不带文字)。
//
// 形态:透明底 → 悬停提亮一档(surfaceHover)→ 选中态 = 一圈 accent 环。
// 为什么"选中"用环而不是填充块(§13.2 的同一套语言):强调色只出现在"能动手的地方",
// 环把两颗图标框成一组,颜色不盖住图标本身的形。
//
// 环画在**外框 4 逻辑像素**这一带:里面的 20x20 画布归图标自己,所以验收脚本扫这一带
// 数 accent 像素时,不会把图标自带的颜色(比如微软 LOGO 的蓝方块)算进来。
#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QSize>
#include <QString>

namespace sxcl::ui2 {

class IconButton : public QAbstractButton {
    Q_OBJECT
public:
    /** Theme = 按主题令牌现染(单色线稿);Original = 保留素材本色(微软 LOGO 这类官方四色)。 */
    enum class Tint { Theme, Original };

    explicit IconButton(const QString &iconFile, int glyphSize = 20, QWidget *parent = nullptr);

    void setTint(Tint tint);
    void setSelected(bool on);
    bool selected() const { return m_selected; }

    int glyphSize() const { return m_glyph; }
    /** 选中环所在的带宽(逻辑像素):验收脚本按它扫像素。 */
    static int ringBandPx() { return 4; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QColor tintColor() const;

    QString m_file;
    int m_glyph = 20;
    Tint m_tint = Tint::Theme;
    bool m_selected = false;
    bool m_hover = false;
};

/** 只画一枚图标的小控件(不可点):**每次 paintEvent 现取令牌染一遍** ——
 *  主题一换自己就跟着变,不存在"构造期把颜色烤进 QPixmap"那个坑(§0 第 2 条)。 */
class IconGlyph : public QWidget {
    Q_OBJECT
public:
    IconGlyph(const QString &iconFile, int size, QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_file;
    int m_size = 16;
};

} // namespace sxcl::ui2
