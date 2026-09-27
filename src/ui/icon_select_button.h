/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

// 图标点选钮(docs/27 §11「按钮最少化」:能猜到的动作只给图标,人话进 tooltip)。
//
// 为什么单独一个控件:下载页侧2 页脚那两个"版本形态"选项原来是一个 SwitchButton 滑块 + 两个
// 纯文字标签(用户 2026-09-26 点名「点这两个所代表的图标,而不是一个按钮,非常诡异啊」)。
// 做成**图标本身可点**要的东西很具体:透明底、悬停一档提亮、选中态用**主题令牌**
// (**图标下方一条 2 逻辑像素的 accent 指示条**,不是方框 —— 用户 2026-09-26 最终口径),
// 而且绘制时**现取令牌** —— docs/27 §0 踩过的坑就是"构造期把颜色烤死,换主题必出事故"。
//
// 图标文件来自 assets/icons/edition/(原版素材,出处见该目录 NOTICE.md):
//   * .svg 走 QSvgRenderer(矢量,任意尺寸都清晰);
//   * .png 按 DPR 出图;整数倍放大用最近邻(像素画不糊),大图缩小走两段平滑(不产生锯齿)。
//
// **尺寸按"视觉高度"给**(setIconHeight):宽度不再写死,由素材自己的横纵比算出来 ——
// 咖啡杯接近 1:1,而基岩版那枚官方 MINECRAFT 标题 LOGO 是 5.8:1 的宽幅图,两者要能摆在一起。
// 这一层**不认识**具体是哪个图标,文件名由调用方给。
//
// 键盘可达(docs/27 §4):Tab 能到、空格/回车能按(QAbstractButton 自带)。

#include <QAbstractButton>
#include <QSize>
#include <QString>

namespace sxcl::ui {

class IconSelectButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit IconSelectButton(QWidget *parent = nullptr);

    // assets/icons/<dir>/<file>;空串 = 不画图标(也不会崩)
    //   * 默认 dir = "edition"(下载页的版本形态:Java 咖啡杯 / 基岩版原版 LOGO);
    //   * 主页的正版/离线两枚在 "ui"(microsoft.svg 四色方块 + disconnected.svg 断线线稿),
    //     见 assets/icons/ui/NOTICE.md;
    // 目录解析写在一处(编译期 SXCL_UI_EDITION_DIR / 环境变量 / exe 旁的 assets/icons/<dir> /
    // SXCL_UI_BLOCK_DIR 的兄弟目录 <dir>)—— 与 SxclIcons、ui_icons 同一套口径。
    void setIconFile(const QString &file);
    QString iconFile() const { return m_iconFile; }
    void setIconDir(const QString &dir);
    QString iconDir() const { return m_iconDir; }
    // 图标的**绘制高度**(逻辑像素);宽度按素材横纵比算,按钮尺寸 = 绘制盒 + 2 * kPad
    void setIconHeight(int height);
    int iconHeight() const { return m_iconHeight; }

    /* 单色线稿:按**主题令牌**在绘制时现染(未选中 textSecondary、选中 accent)。
     * 默认**关** —— 原版彩色素材(微软四色方块、Java 咖啡杯、基岩版 LOGO)绝不许被染成一色,
     * 那正是这几枚图标的识别度所在。开着它的只有断线那类线稿。 */
    void setMonochrome(bool on);
    bool isMonochrome() const { return m_monochrome; }
    // 当前算出来的绘制盒(逻辑像素)= 按钮去掉内边距那一块
    QSize iconBoxSize() const;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QString m_iconFile;
    QString m_iconDir = QStringLiteral("edition");
    int m_iconHeight = 20;
    bool m_monochrome = false;
    bool m_hover = false;
};

} // namespace sxcl::ui
