/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "sources.h"

#include <QByteArray>

#include <cstdio>
#include <utility>

#include "../dialogs/account.h"      // loadAccountSnapshot(界面层读账户的唯一入口)
#include "../workers/instance_scan.h" // scanInstalledInstances(界面层扫已安装版本的唯一实现)
#include "../workers/ui_paths.h"      // uiGameDirectory / uiSettingsFilePath
#include "sxcl/settings.h"
#include "task.h"

namespace sxcl::ui2 {
namespace {

/** 打开设置、跑一段代码、关掉(设置句柄不做线程安全承诺,所以每次现开现关、不跨线程持有)。 */
template <typename Fn> void withSettings(Fn fn) {
    const QByteArray path = sxcl::ui::uiSettingsFilePath().toUtf8();
    if (sxcl_settings *st = sxcl_settings_open(path.constData())) {
        fn(st, path);
        sxcl_settings_free(st);
    }
}

void writeSetting(const char *key, const QString &value, const char *why) {
    withSettings([&](sxcl_settings *st, const QByteArray &path) {
        sxcl_settings_set(st, key, value.toUtf8().constData());
        const int rc = sxcl_settings_save(st, path.constData());
        std::fprintf(stderr, "[sxcl-ui2] settings: %s=%s rc=%d 文件=%s(%s)\n", key,
                     value.toUtf8().constData(), rc, path.constData(), why);
    });
}

QString readSetting(const char *key, const char *fallback) {
    QString out = QString::fromUtf8(fallback);
    withSettings([&](sxcl_settings *st, const QByteArray &) {
        const char *value = sxcl_settings_get(st, key, fallback);
        if (value != nullptr)
            out = QString::fromUtf8(value);
    });
    return out;
}

} // namespace

HomeData loadHomeData() {
    requireWorkerThread("home-instances");
    HomeData data;
    data.gameDir = sxcl::ui::uiGameDirectory();
    data.gameDirReason = sxcl::ui::uiGameDirectoryReason();
    QString error;
    const QVector<sxcl::ui::InstalledInstance> items =
        sxcl::ui::scanInstalledInstances(data.gameDir, &error);
    data.error = error;
    data.versions.reserve(items.size());
    for (const sxcl::ui::InstalledInstance &it : items) {
        VersionItem v;
        v.id = it.id;
        v.summary = it.summary;
        v.type = it.type;
        v.base = it.baseVersion;
        v.launchable = it.launchable;
        v.problem = it.problem;
        data.versions.append(v);
    }
    return data;
}

PlayerData loadPlayerData() {
    requireWorkerThread("home-account");
    PlayerData data;
    const sxcl::ui::AccountSnapshot snapshot = sxcl::ui::loadAccountSnapshot();
    data.read = true;
    data.loggedIn = snapshot.loggedIn && !snapshot.playerName.isEmpty();
    data.name = snapshot.playerName;
    data.uuid = snapshot.uuid;
    data.error = snapshot.error;
    return data;
}

bool readPremiumEdition() {
    return readSetting("game.edition", "premium") != QLatin1String("offline");
}

void savePremiumEdition(bool premium) {
    writeSetting("game.edition", premium ? QStringLiteral("premium") : QStringLiteral("offline"),
                 "版本形态偏好(键沿用下载页)");
}

QString readOfflineName() {
    const QString name = readSetting("player.name", "").trimmed();
    return name.isEmpty() ? QStringLiteral("Steve") : name;
}

void saveOfflineName(const QString &name) {
    writeSetting("player.name", name.trimmed(), "离线玩家名");
}

QStringList readNoticeLines() {
    // 夹具优先:抓图/验收时钉死"有公告"这一条分支,不动用户配置(与 SXCL_UI_* 同一口径)。
    QString raw = qEnvironmentVariable("SXCL_UI2_NOTICE");
    if (raw.isEmpty())
        raw = readSetting("ui.notice", "");
    QStringList out;
    for (const QString &part : raw.split(QLatin1Char('|'))) {
        const QString line = part.trimmed();
        if (!line.isEmpty())
            out.append(line);
    }
    return out;
}

} // namespace sxcl::ui2
