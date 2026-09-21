#include "ui_paths.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QStandardPaths>

#include <cstdio>

#include "sxcl/paths.h"
#include "sxcl/settings.h"

namespace sxcl::ui {
namespace {

// platform.py:148-152 default_config_directory() —— Windows = %APPDATA%/SilentXCraftLauncher
// 旧版(Python)配置。只用来做**一次性迁移**,不做主数据源(home_page.cpp:126-141 同口径)。
QString legacyConfigFilePath() {
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/AppData/Roaming");
    return base + QStringLiteral("/SilentXCraftLauncher/config.json");
}

QString legacyConfigString(const QString &group, const QString &key) {
    QFile file(legacyConfigFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return QString();
    const QJsonValue value = doc.object().value(group).toObject().value(key);
    return value.isString() ? value.toString() : QString();
}

} // namespace

QString uiSettingsFilePath() {
    // 取证通路:钉死设置文件(不改用户配置)。空串 = 没钉。
    const QString pinned = qEnvironmentVariable("SXCL_UI_SETTINGS");
    if (!pinned.isEmpty())
        return pinned;
    // 与 home_page.cpp:129-141 的 ownSettingsFilePath() 逐字同口径。
#if defined(Q_OS_WIN)
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/AppData/Roaming");
    return base + QStringLiteral("/SilentXCraftLauncher/settings.conf");
#elif defined(Q_OS_MACOS)
    return QDir::homePath() +
           QStringLiteral("/Library/Application Support/SilentXCraftLauncher/settings.conf");
#else
    return QDir::homePath() + QStringLiteral("/.config/SilentXCraftLauncher/settings.conf");
#endif
}

QString uiLauncherDataRoot() {
    // 取证通路:钉死数据根(不动用户配置/缓存)
    const QString pinned = qEnvironmentVariable("SXCL_UI_DATA_DIR");
    if (!pinned.isEmpty())
        return QDir::fromNativeSeparators(pinned);
#if defined(Q_OS_WIN)
    // platform.py:148-152 default_config_directory():与 Python 版同一目录,过渡期共用
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/AppData/Roaming");
    return base + QStringLiteral("/SilentXCraftLauncher");
#elif defined(Q_OS_MACOS)
    return QDir::homePath() +
           QStringLiteral("/Library/Application Support/SilentXCraftLauncher");
#elif defined(Q_OS_ANDROID)
    // Qt for Android 的 AppDataLocation 落在 App 私有目录里(可写,且不需要任何权限);
    // 拿不到时退回 HOME 下的隐藏目录(HOME 在 Android 上也是私有 files 目录)。
    const QString standard =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return standard.isEmpty() ? QDir::homePath() + QStringLiteral("/.silentxcraftlauncher")
                              : standard;
#else
    return QDir::homePath() + QStringLiteral("/.config/SilentXCraftLauncher");
#endif
}

QString uiGameDirectory() {
    // 1) 验收/取证通路优先。**必须最优先**:抓图与端到端验收都是拿它钉条件的,
    //    被本机用户配置悄悄改掉就没法复现(与 SXCL_UI_THEME 同一口径)。
    const QString pinned = qEnvironmentVariable("SXCL_UI_GAME_DIR");
    if (!pinned.isEmpty())
        return QDir::fromNativeSeparators(pinned);

    char err[256];
    char buf[4096];
    err[0] = '\0';

    // 2) 我们自己的设置
    QString configured;
    const QByteArray own = uiSettingsFilePath().toUtf8();
    if (sxcl_settings *st = sxcl_settings_open(own.constData())) {
        configured = QString::fromUtf8(sxcl_settings_game_default_dir(st));
        sxcl_settings_free(st);
    }

    // 3) 一次性迁移:把我们自己的设置里没有该项时,把 Python 版的 gameDirectory 导进来。
    //    Android 上那个文件不存在 -> 自然跳过,走核心库的 Android 默认。
    if (configured.isEmpty()) {
        const QString legacy = legacyConfigString(QStringLiteral("Game"),
                                                 QStringLiteral("gameDirectory"));
        if (!legacy.isEmpty()) {
            if (sxcl_settings *st = sxcl_settings_open(own.constData())) {
                sxcl_settings_set(st, "game.default_dir", legacy.toUtf8().constData());
                sxcl_settings_save(st, own.constData());
                sxcl_settings_free(st);
            }
            configured = legacy;
        }
    }

    if (!configured.isEmpty()) {
        if (sxcl_paths_resolve_game_dir(configured.toUtf8().constData(), buf, sizeof(buf), err,
                                        sizeof(err)) == SXCL_PATHS_OK)
            return QDir::fromNativeSeparators(QString::fromUtf8(buf));
        return QDir::fromNativeSeparators(configured);
    }

    // 4) 核心库的平台默认(Windows = %APPDATA%/.minecraft;Android = <files>/.minecraft)
    if (sxcl_paths_default_game_dir(buf, sizeof(buf), err, sizeof(err)) == SXCL_PATHS_OK)
        return QDir::fromNativeSeparators(QString::fromUtf8(buf));
    return QDir::fromNativeSeparators(QDir::homePath()) + QStringLiteral("/.minecraft");
}

QString uiJavaPath() {
    // 取证/验收通路:钉死 java(不影响用户设置)
    const QString pinned = qEnvironmentVariable("SXCL_UI_JAVA_PATH");
    if (!pinned.isEmpty())
        return QDir::fromNativeSeparators(pinned);

    // 设置页写的是 game.java_path(settings_page.cpp:143 kKeyJavaPath)
    if (sxcl_settings *st = sxcl_settings_open(uiSettingsFilePath().toUtf8().constData())) {
        const char *value = sxcl_settings_get(st, "game.java_path", "");
        const QString text = value != nullptr ? QString::fromUtf8(value) : QString();
        sxcl_settings_free(st); // 返回的指针归句柄所有,释放即失效 -> 先拷贝再释放
        if (!text.isEmpty())
            return QDir::fromNativeSeparators(text);
    }

    // 从 Python 版转过来的用户:旧配置里的 Game.javaPath(只读,不迁移 —— 核心库自己会探测)
    const QString legacy = legacyConfigString(QStringLiteral("Game"), QStringLiteral("javaPath"));
    return legacy.isEmpty() ? QString() : QDir::fromNativeSeparators(legacy);
}

int uiMemoryMb() {
    // 读我们自己的设置 game.max_memory_mb(设置页写的就是这个键)。
    // 读不到返回 0 = 交给核心库按位数取默认(与 CLI 不给 --memory 时一致)。
    if (sxcl_settings *st = sxcl_settings_open(uiSettingsFilePath().toUtf8().constData())) {
        const int mb = static_cast<int>(sxcl_settings_get_int(st, "game.max_memory_mb", 0));
        sxcl_settings_free(st);
        return mb > 0 ? mb : 0;
    }
    return 0;
}

bool uiTraceEnabled() {
    static const bool enabled = !qEnvironmentVariable("SXCL_UI_TRACE").isEmpty() &&
                                qEnvironmentVariable("SXCL_UI_TRACE") != QLatin1String("0");
    return enabled;
}

void uiTrace(const QString &line) {
    if (!uiTraceEnabled())
        return;
    // 一次 fwrite 原子写出整行,免得多个工作线程的行互相穿插
    const QByteArray bytes = QStringLiteral("[sxcl-ui] ") .toUtf8() + line.toUtf8() + "\n";
    std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stderr);
    std::fflush(stderr);
}

} // namespace sxcl::ui
