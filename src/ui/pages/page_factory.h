#pragma once
// page_factory —— 页面工厂(阶段 6)
//
// 每条路由一个实现文件,由各页独立开发(避免多个改动抢同一个 main_window.cpp)。
// 返回 nullptr 表示该页尚未移植,主窗口退回占位页。
// 页面外观的唯一依据:docs/05-UI-1to1规格.md + Python 版对应页面源码。
#include <QString>

class QWidget;

namespace sxcl::ui {

QWidget *createHomePage(QWidget *parent);
QWidget *createVersionsPage(QWidget *parent);
QWidget *createTasksPage(QWidget *parent);
QWidget *createKeymapPage(QWidget *parent);
QWidget *createMultiplayerPage(QWidget *parent);
QWidget *createSettingsPage(QWidget *parent);

// 路由 -> 页面对象(nullptr = 未移植)
QWidget *createPageForRoute(const QString &routeKey, QWidget *parent);

} // namespace sxcl::ui
