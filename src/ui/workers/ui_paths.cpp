/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "ui_paths.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSettings>
#include <QStandardPaths>

#include <cstdio>
#include <cstring>

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
    // **唯一权威:核心库 sxcl_settings_default_path()**。
    //
    // 为什么必须这样(这是个真事故,不是洁癖):设置页写文件用的是核心库那条路径,
    // 而这里以前**自己手拼**了一份 —— Windows 上两者碰巧一致(都是 %APPDATA%/SilentXCraftLauncher)，
    // 所以桌面看不出来;Android 上核心库给的是 <应用私有 files>/SilentXCraftLauncher,
    // 这里给的是 ~/.config/... 而安卓的 HOME 是 "/" —— 于是**读的与写的根本不是同一个文件**:
    // 设置页改了、界面上看着也改了,重启后启动恢复(主题/强调色/语言)、游戏目录、Java 路径、
    // 内存、下载源全部回落默认值,用户看到的就是"设置关闭重开直接打回原形"。
    // Linux 上也有同一类错位(核心库/XDG 用全小写目录名,这里用大写)。
    static const QString path = [] {
        // 取证通路:钉死设置文件(不改用户配置)。空串 = 没钉。
        const QString pinned = qEnvironmentVariable("SXCL_UI_SETTINGS");
        if (!pinned.isEmpty())
            return QDir::fromNativeSeparators(pinned);
        char buf[1024];
        char err[SXCL_SETTINGS_ERR_MAX];
        err[0] = '\0';
        if (sxcl_settings_default_path(buf, sizeof(buf), err, sizeof(err)) == SXCL_SETTINGS_OK)
            return QDir::fromNativeSeparators(QString::fromUtf8(buf));
        // 连路径都拼不出来(极端受限环境)才兜底,避免整个设置页不可用。
        return uiLauncherDataRoot() + QStringLiteral("/settings.conf");
    }();
    // 一行取证:桌面上可以直接核对"读的和写的到底是不是同一个文件"。
    // 注意:**不能用 lambda 捕获 path** —— 它是 static 存储期,简单捕获在 C++ 里是编译错误(C3495)。
    static bool traced = false;
    if (!traced) {
        traced = true;
        uiTrace(QStringLiteral("settings | 文件=") + path);
    }
    return path;
}

QString uiLauncherDataRoot() {
    // 取证通路:钉死数据根(不动用户配置/缓存)
    const QString pinned = qEnvironmentVariable("SXCL_UI_DATA_DIR");
    if (!pinned.isEmpty())
        return QDir::fromNativeSeparators(pinned);
    // 与设置文件**同一个目录**(同一个权威:核心库)。缓存/配置散在两处是上一个事故的同类写法。
    char buf[1024];
    char err[256];
    err[0] = '\0';
    if (sxcl_settings_default_dir(buf, sizeof(buf), err, sizeof(err)) == SXCL_SETTINGS_OK)
        return QDir::fromNativeSeparators(QString::fromUtf8(buf));
    return QDir::homePath() + QStringLiteral("/.silentxcraftlauncher");
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

    // 4) 核心库的平台默认(Windows = %APPDATA%/.minecraft;macOS/Linux = ~/.minecraft)。
    //    **Android 上没有默认**(核心库返回 UNSUPPORTED,要求调用方自己给),
    //    所以下面单独走安卓分支。
    if (sxcl_paths_default_game_dir(buf, sizeof(buf), err, sizeof(err)) == SXCL_PATHS_OK)
        return QDir::fromNativeSeparators(QString::fromUtf8(buf));

#if defined(Q_OS_ANDROID)
    // ★ 安卓:用户机器上**通常已经有一份装好的游戏目录**(FCL / HMCL / Pojav / 共享存储里的
    //   .minecraft),而我们的私有目录往往是空的。用户实测报过:"不可能,我平板上 100% 有 mc 目录"
    //   —— 他点"启动 26.2-NeoForge"时报"版本没装好",而那个版本就躺在
    //   /storage/emulated/0/FCL/.minecraft/versions/26.2-NeoForge 里。
    //
    //   规则(顺序说清楚,不猜):
    //     1. 我们自己的私有 <files>/.minecraft **有版本** -> 用它(那是"本应用"的目录);
    //     2. 否则扫描候选(sxcl_paths_detect_android:私有/共享存储/FCL/HMCL/Pojav…),
    //        挑**真的有 versions/ 的**那一个,并**落盘**到 game.default_dir
    //        —— 落盘是为了稳定:下次不再探测,设置页里也能看到它并能改掉。
    //     3. 一个都没有 -> 回到私有目录(全新设备,装的时候会创建)。
    {
        const QByteArray filesUtf8 = qgetenv("SXCL_ANDROID_FILES");
        const QString privateDir =
            filesUtf8.isEmpty()
                ? QDir::homePath() + QStringLiteral("/.minecraft")
                : QDir::fromNativeSeparators(QString::fromLocal8Bit(filesUtf8)) +
                      QStringLiteral("/.minecraft");
        if (sxcl_paths_count_versions(privateDir.toUtf8().constData()) > 0)
            return privateDir;

        const QByteArray sharedUtf8 = qgetenv("SXCL_ANDROID_SHARED");
        sxcl_game_folders folders;
        std::memset(&folders, 0, sizeof(folders));
        char derr[SXCL_PATHS_ERROR_MAX];
        derr[0] = '\0';
        // 注意签名是 6 个参数:files_dir / shared_root / 结果 / **已配置目录** / err / err_len
        if (sxcl_paths_detect_android(filesUtf8.isEmpty() ? nullptr : filesUtf8.constData(),
                                      sharedUtf8.isEmpty() ? nullptr : sharedUtf8.constData(),
                                      &folders, nullptr, derr, sizeof(derr)) == SXCL_PATHS_OK) {
            const sxcl_game_folder *best = sxcl_paths_best(&folders);
            if (best != nullptr && best->versions > 0) {
                const QString picked = QDir::fromNativeSeparators(QString::fromUtf8(best->path));
                const QByteArray own = uiSettingsFilePath().toUtf8();
                if (sxcl_settings *st = sxcl_settings_open(own.constData())) {
                    sxcl_settings_set(st, "game.default_dir", picked.toUtf8().constData());
                    sxcl_settings_save(st, own.constData());
                    sxcl_settings_free(st);
                }
                uiTrace(QStringLiteral("gamedir | 采用检测到的游戏目录 %1(%2 个版本,来源 %3);"
                                       "已写入 game.default_dir")
                            .arg(picked)
                            .arg(best->versions)
                            .arg(QString::fromUtf8(best->source)));
                return picked;
            }
        }
        return privateDir;
    }
#endif
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

QString uiDownloadSource() {
    // 取证通路优先(钉死源,不动用户设置)
    const QString pinned = qEnvironmentVariable("SXCL_UI_DOWNLOAD_SOURCE");
    if (!pinned.isEmpty())
        return pinned.trimmed().toLower();
    // 设置页写的就是 download.source(settings_page.cpp 的 kKeyDownloadSource)
    if (sxcl_settings *st = sxcl_settings_open(uiSettingsFilePath().toUtf8().constData())) {
        const char *value = sxcl_settings_get(st, "download.source", "bmclapi");
        const QString text = value != nullptr ? QString::fromUtf8(value) : QString();
        sxcl_settings_free(st); // 返回的指针归句柄所有,释放即失效 -> 先拷贝再释放
        const QString normalized = text.trimmed().toLower();
        if (!normalized.isEmpty())
            return normalized;
    }
    return QStringLiteral("bmclapi");
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
