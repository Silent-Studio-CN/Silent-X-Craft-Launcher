/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "page_factory.h"

#include <QWidget>

namespace sxcl::ui {

QWidget *createPageForRoute(const QString &routeKey, QWidget *parent) {
    if (routeKey == QStringLiteral("home"))
        return createHomePage(parent);
    if (routeKey == QStringLiteral("versions"))
        return createVersionsPage(parent);
    if (routeKey == QStringLiteral("tasks"))
        return createTasksPage(parent);
    if (routeKey == QStringLiteral("keymap"))
        return createKeymapPage(parent);
    if (routeKey == QStringLiteral("multiplayer"))
        return createMultiplayerPage(parent);
    if (routeKey == QStringLiteral("settings"))
        return createSettingsPage(parent);
    // 界面重构(docs/25):下载 / 团队 / 更多 三个新页;versions/tasks/keymap 仍在,
    // 只是不再占侧边栏(从"下载"和"更多"里进)。
    if (routeKey == QStringLiteral("download"))
        return createDownloadPage(parent);
    if (routeKey == QStringLiteral("team"))
        return createTeamPage(parent);
    if (routeKey == QStringLiteral("more"))
        return createMorePage(parent);
    // 版本选择页:主页的"更换"按钮点进来的(不在侧边栏,按需建页 —— 见 switchToRoute)。
    if (routeKey == QStringLiteral("select"))
        return createVersionsSelectPage(parent);
    return nullptr;
}

} // namespace sxcl::ui
