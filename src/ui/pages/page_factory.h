/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include <QString>

class QWidget;

namespace sxcl::ui {

QWidget *createHomePage(QWidget *parent);
// embedded=true:嵌在下载页右列那一格(**不许**再自带 ScrollArea/标题 —— 见 versions_page.cpp 的说明)
QWidget *createVersionsPage(QWidget *parent, bool embedded = false);
QWidget *createTasksPage(QWidget *parent);
QWidget *createKeymapPage(QWidget *parent);
QWidget *createMultiplayerPage(QWidget *parent);
QWidget *createSettingsPage(QWidget *parent);
// 2026-09-22 界面重构(docs/25)新增的三个页面
QWidget *createDownloadPage(QWidget *parent);
QWidget *createTeamPage(QWidget *parent);
QWidget *createMorePage(QWidget *parent);
// 版本选择页(用户 2026-09-22:主页的"所有安装版本"整块搬到这里,左侧是文件夹列表)
QWidget *createVersionsSelectPage(QWidget *parent);
// 模组/光影那一栏(docs/22 的 A1):shaders = 1 时装进 shaderpacks、搜光影包
QWidget *createModsPage(QWidget *parent, bool shaders);

// 路由 -> 页面对象(nullptr = 未移植)
QWidget *createPageForRoute(const QString &routeKey, QWidget *parent);

// ---------------------------------------------------------------------------
// 临时页(会话页)—— 对应 Python src/app/main_window.py:188-243 的
// switch_to_download_config / switch_to_download_progress / switch_to_launch。
//
// 它们**不在导航里**,由主窗口的"会话保持的临时页"机制按需创建并挂进内容栈:
//   * 下载配置页 download_config_page.cpp  <- download_config_page.py
//   * 下载进度页 download_progress_page.cpp <- download_progress_page.py
//   * 启动进度页 launch_page.cpp            <- launch_page.py
//
// Python 侧这三个页面的构造函数第一个参数都是 services/minecraft/manifest.py:72-96 的
// GameVersion;三个页面里只用到它的 .id(标题/版本名/启动键),所以这里只带 id。
// 不叫 GameVersion 是为了不和 versions_page.cpp 里同名的局部结构撞名。
struct VersionRef {
    QString id;
};

QWidget *createDownloadConfigPage(const VersionRef &version, QWidget *parent);
QWidget *createDownloadProgressPage(const VersionRef &version, const QString &versionName,
                                    const QString &loaderType, const QString &loaderVersion,
                                    QWidget *parent);
QWidget *createLaunchPage(const VersionRef &version, QWidget *parent);

} // namespace sxcl::ui
