/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

namespace sxcl::ui2 {

/* 一行"会冒出来"的列表行(docs/27 §10.2.1 第 3 条的落点控件)。
 *
 * 它存在的理由:错峰淡入要**看得见**,而 QLabel/QToolButton 没法在不吃 QGraphicsOpacityEffect
 * 的前提下淡入(§10.2 明确不用 effect 做这类动画)。所以这里是一个自绘行:
 *   - 只暴露一个动画属性 reveal(0 = 还没出现:全透明 + 下沉 8px;1 = 完全就位);
 *   - 动画写它 -> paintEvent 读它:不碰布局、不 setGeometry、不重建 QSS(§10.2.1 性能纪律);
 *   - 背景/圆角/悬停/选中态仍旧吃主题 QSS(WA_StyledBackground + PE_Widget),颜色不在代码里写死。
 *
 * 页面里的用法(一行,没有回调):
 *   Anim::stagger(rows, 100, 25).start();     // 默认就驱动 "reveal"
 */
class MotionRow : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal reveal READ reveal WRITE setReveal)

public:
    explicit MotionRow(QWidget *parent = nullptr);
    explicit MotionRow(const QString &text, QWidget *parent = nullptr);

    /** 出现度 [0,1]。默认 1:不播动画的行就是完整可见的。 */
    qreal reveal() const { return m_reveal; }
    void setReveal(qreal reveal);

    QString text() const { return m_text; }
    void setText(const QString &text);

    /** 出现前下沉多少像素(§4:内容淡入 + 8px 上移)。 */
    int liftPx() const { return m_lift; }
    void setLiftPx(int px);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_text;
    qreal m_reveal = 1.0;
    int m_lift = 8;
};

} // namespace sxcl::ui2
