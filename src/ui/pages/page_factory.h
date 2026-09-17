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
