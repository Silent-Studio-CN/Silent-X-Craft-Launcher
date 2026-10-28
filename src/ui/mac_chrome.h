/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

/* macOS 的窗口外观:左上角那三枚红黄绿,以及屏幕顶部那条系统菜单栏。
 * 只在 macOS 上编译;Windows / Linux / 安卓走原来的自绘三键。 */

#include <QWidget>

class QMenuBar;

namespace sxcl::ui {

class MainWindow;

/** 红黄绿三枚圆点(关闭 / 最小化 / 缩放)。
 *
 * 尺寸与间距按原生窗口来:直径 12、间隔 8、左边距 20 —— 直径再大就不像 macOS 了。
 * 悬停时三枚一起出现符号(和系统一致),离开即消失。 */
class MacTrafficLights : public QWidget {
    Q_OBJECT

public:
    explicit MacTrafficLights(QWidget *parent = nullptr);
    QSize sizeHint() const override;

signals:
    void closeRequested();
    void minimizeRequested();
    void zoomRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    int hotspot(const QPoint &pos) const; /* 0 关闭 / 1 最小化 / 2 缩放 / -1 无 */
    QRect dotRect(int index) const;

    int m_hover = -1;
    int m_pressed = -1;
};

/** 建一条菜单栏并挂到窗口上。macOS 下 QMenuBar 会自动并进屏幕顶部的系统菜单栏。 */
QMenuBar *createMacMenuBar(MainWindow *window);

} // namespace sxcl::ui
