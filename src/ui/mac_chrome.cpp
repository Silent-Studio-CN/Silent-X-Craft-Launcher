/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "mac_chrome.h"

#include "main_window.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>

#include "sxcl/log.h"
#include "sxcl/version.h"

namespace sxcl::ui {
namespace {

/* 原生窗口的度量:直径 12、中心间隔 20、左边距 20;标题栏高 48。 */
constexpr int kDotDiameter = 12;
constexpr int kDotStep = 20;
constexpr int kLeftInset = 20;
constexpr int kBarHeight = 48;

const QColor kCloseColor(0xFF, 0x5F, 0x57);
const QColor kMinColor(0xFE, 0xBC, 0x2E);
const QColor kZoomColor(0x28, 0xC8, 0x40);
const QColor kInactiveColor(0x59, 0x5B, 0x5E);

} // namespace

MacTrafficLights::MacTrafficLights(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("macTrafficLights"));
    setMouseTracking(true);
    setFixedSize(sizeHint());
}

QSize MacTrafficLights::sizeHint() const {
    return QSize(kLeftInset + kDotStep * 2 + kDotDiameter + 8, kBarHeight);
}

QRect MacTrafficLights::dotRect(int index) const {
    const int x = kLeftInset + index * kDotStep;
    const int y = (height() - kDotDiameter) / 2;
    return QRect(x, y, kDotDiameter, kDotDiameter);
}

int MacTrafficLights::hotspot(const QPoint &pos) const {
    for (int i = 0; i < 3; ++i) {
        if (dotRect(i).adjusted(-4, -4, 4, 4).contains(pos))
            return i;
    }
    return -1;
}

void MacTrafficLights::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    /* 窗口失焦时三枚一起变灰 —— 这是 macOS 自己的规矩,不这么做会一直"亮着"。 */
    const bool active = window() != nullptr && window()->isActiveWindow();
    const QColor colors[3] = {
        active ? kCloseColor : kInactiveColor,
        active ? kMinColor : kInactiveColor,
        active ? kZoomColor : kInactiveColor,
    };

    for (int i = 0; i < 3; ++i) {
        const QRect r = dotRect(i);
        p.setPen(Qt::NoPen);
        p.setBrush(colors[i]);
        p.drawEllipse(r);

        if (m_hover < 0)
            continue;

        /* 悬停时三枚都出现符号(鼠标在任意一枚上,和系统一致)。 */
        QColor glyph(0, 0, 0, 150);
        if (!active)
            glyph = QColor(0, 0, 0, 90);
        p.setBrush(Qt::NoBrush);
        QPen pen(glyph, 1.4);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);

        const QPointF c = r.center();
        const qreal h = 2.6; /* 符号半宽 */
        if (i == 0) {
            p.drawLine(QPointF(c.x() - h, c.y() - h), QPointF(c.x() + h, c.y() + h));
            p.drawLine(QPointF(c.x() - h, c.y() + h), QPointF(c.x() + h, c.y() - h));
        } else if (i == 1) {
            p.drawLine(QPointF(c.x() - h - 0.4, c.y()), QPointF(c.x() + h + 0.4, c.y()));
        } else {
            /* 缩放:两个相对的小三角 */
            p.setPen(Qt::NoPen);
            p.setBrush(glyph);
            QPainterPath left;
            left.moveTo(c.x() - h, c.y() - h);
            left.lineTo(c.x() - h, c.y() + h - 1.2);
            left.lineTo(c.x() + 0.4, c.y());
            left.closeSubpath();
            QPainterPath right;
            right.moveTo(c.x() + h, c.y() + h);
            right.lineTo(c.x() + h, c.y() - h + 1.2);
            right.lineTo(c.x() - 0.4, c.y());
            right.closeSubpath();
            p.drawPath(left);
            p.drawPath(right);
        }
    }
}

void MacTrafficLights::mouseMoveEvent(QMouseEvent *event) {
    const int hit = hotspot(event->pos());
    if (hit != m_hover) {
        m_hover = hit;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void MacTrafficLights::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_pressed = hotspot(event->pos());
}

void MacTrafficLights::mouseReleaseEvent(QMouseEvent *event) {
    const int hit = hotspot(event->pos());
    const int pressed = m_pressed;
    m_pressed = -1;
    if (event->button() != Qt::LeftButton || hit < 0 || hit != pressed)
        return;
    if (hit == 0)
        emit closeRequested();
    else if (hit == 1)
        emit minimizeRequested();
    else
        emit zoomRequested();
}

void MacTrafficLights::leaveEvent(QEvent *event) {
    if (m_hover >= 0) {
        m_hover = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

QMenuBar *createMacMenuBar(MainWindow *window) {
    auto *bar = new QMenuBar(window);
    bar->setNativeMenuBar(true);

    /* 第一条在 macOS 上会变成"应用菜单"(标题被系统忽略,菜单项保留)。 */
    QMenu *app = bar->addMenu(QStringLiteral("SXCL"));
    QAction *about = app->addAction(QStringLiteral("关于 SXCL"));
    QObject::connect(about, &QAction::triggered, window, [window] {
        QMessageBox box(window);
        box.setWindowTitle(QStringLiteral("关于 SXCL"));
        box.setTextFormat(Qt::RichText);
        box.setText(QStringLiteral("<b>Silent X Craft Launcher</b> %1<br>%2")
                        .arg(QString::fromUtf8(sxcl_version_string()),
                             QStringLiteral("Minecraft: Java Edition 启动器")));
        box.exec();
    });
    app->addSeparator();
    QAction *settings = app->addAction(QStringLiteral("设置…"));
    settings->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    QObject::connect(settings, &QAction::triggered, window,
                     [window] { window->switchToRoute(QStringLiteral("settings")); });
    app->addSeparator();
    QAction *quit = app->addAction(QStringLiteral("退出 SXCL"));
    quit->setShortcut(QKeySequence::Quit);
    QObject::connect(quit, &QAction::triggered, window, [window] { window->close(); });

    /* 视图:四个页面,⌘1..⌘4。 */
    QMenu *view = bar->addMenu(QStringLiteral("视图"));
    const struct {
        const char *route;
        const char *title;
        const char *key;
    } kRoutes[] = {
        {"home", "主页", "Ctrl+1"},
        {"download", "下载", "Ctrl+2"},
        {"select", "版本选择", "Ctrl+3"},
        {"settings", "设置", "Ctrl+4"},
    };
    for (const auto &r : kRoutes) {
        const QString route = QString::fromUtf8(r.route);
        QAction *action = view->addAction(QString::fromUtf8(r.title));
        action->setShortcut(QKeySequence(QString::fromUtf8(r.key)));
        QObject::connect(action, &QAction::triggered, window, [window, route] {
            QMetaObject::invokeMethod(window, "switchToRoute", Qt::DirectConnection,
                                      Q_ARG(QString, route));
        });
    }

    /* 文件:只放真实存在的入口(没有的功能不摆)。 */
    QMenu *file = bar->addMenu(QStringLiteral("文件"));
    QAction *gameDir = file->addAction(QStringLiteral("更改游戏目录…"));
    QObject::connect(gameDir, &QAction::triggered, window,
                     [window] { window->switchToRoute(QStringLiteral("settings")); });

    /* 窗口 */
    QMenu *win = bar->addMenu(QStringLiteral("窗口"));
    QAction *minimize = win->addAction(QStringLiteral("最小化"));
    minimize->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
    QObject::connect(minimize, &QAction::triggered, window, [window] { window->showMinimized(); });
    QAction *zoom = win->addAction(QStringLiteral("缩放"));
    QObject::connect(zoom, &QAction::triggered, window, [window] {
        if (window->isFullScreen())
            window->showNormal();
        else
            window->showFullScreen();
    });

    /* 帮助 */
    QMenu *help = bar->addMenu(QStringLiteral("帮助"));
    QAction *logs = help->addAction(QStringLiteral("打开日志目录"));
    QObject::connect(logs, &QAction::triggered, window, [] {
        const char *dir = sxcl_log_dir();
        if (dir != nullptr && dir[0] != '\0')
            QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(dir)));
    });
    QAction *site = help->addAction(QStringLiteral("官网"));
    QObject::connect(site, &QAction::triggered, window, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://mc.silentstudio.cn")));
    });

    return bar;
}

} // namespace sxcl::ui
