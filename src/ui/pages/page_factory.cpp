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
    return nullptr;
}

} // namespace sxcl::ui
