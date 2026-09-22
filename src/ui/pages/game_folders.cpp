/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "game_folders.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPushButton>
#include <QSet>

#include <algorithm>

#include "sxcl/paths.h"
#include "sxcl/settings.h"

#include "workers/ui_paths.h" // uiSettingsFilePath:设置文件路径的唯一权威(见 ownSettingsFilePath)

namespace sxcl::ui {
namespace {

// platform.py:148-152 default_config_directory() —— Windows = %APPDATA%/SilentXCraftLauncher
QString legacyConfigFilePath() {
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/AppData/Roaming");
    return base + QStringLiteral("/SilentXCraftLauncher/config.json");
}

// 读旧版(Python)SXCL 配置里的某一项(launcher_config.py 的分组/键名)。
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

// 一次设置读/写。写走"读-改-存"三步(设置模块的语义见 sxcl/settings.h)。
QString readSetting(const char *key, const QString &fallback) {
    const QByteArray path = ownSettingsFilePath().toUtf8();
    QString out = fallback;
    if (sxcl_settings *st = sxcl_settings_open(path.constData())) {
        out = QString::fromUtf8(sxcl_settings_get(st, key, fallback.toUtf8().constData()));
        sxcl_settings_free(st);
    }
    return out;
}

void writeSetting(const char *key, const QString &value) {
    const QByteArray path = ownSettingsFilePath().toUtf8();
    if (sxcl_settings *st = sxcl_settings_open(path.constData())) {
        sxcl_settings_set(st, key, value.toUtf8().constData());
        sxcl_settings_save(st, path.constData()); // **必须存盘**:用户要的就是"关掉重开还在"
        sxcl_settings_free(st);
    }
}

int countVersionsIn(const QString &folder) { // folders.py:76-91 _count_versions
    const QDir versionsDir(folder + QStringLiteral("/versions"));
    if (!versionsDir.exists())
        return 0;
    int count = 0;
    const QFileInfoList entries =
        versionsDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        const QString dir = entry.absoluteFilePath();
        if (QFileInfo::exists(dir + QLatin1Char('/') + name + QStringLiteral(".json")) ||
            QFileInfo::exists(dir + QLatin1Char('/') + name + QStringLiteral(".jar"))) {
            ++count;
        } else if (!QDir(dir).entryList(QStringList() << QStringLiteral("*.json"), QDir::Files).isEmpty()) {
            ++count; // 加载器版本常常只有 JSON(jar 靠继承)
        }
    }
    return count;
}

GameFolder inspectFolder(const QString &path, const QString &label) { // folders.py:94-101 _inspect
    GameFolder folder;
    folder.path = path;
    folder.label = label;
    folder.exists = QFileInfo(path).isDir();
    folder.versions = countVersionsIn(path);
    folder.hasAssets = QFileInfo(path + QStringLiteral("/assets")).isDir();
    folder.hasProfiles = QFileInfo(path + QStringLiteral("/launcher_profiles.json")).isFile();
    return folder;
}

} // namespace

int GameFolder::score() const { // folders.py:64-74
    if (!exists)
        return -1;
    int value = versions * 10;
    if (hasAssets)
        value += 5;
    if (hasProfiles)
        value += 2;
    return value;
}

QString GameFolder::name() const {
    // 用户点名:"我们不强制根目录一定是 .minecraft,如果有别的名字,'当前文件夹'就显示这个文件夹的名字"
    const QString name = QDir(path).dirName();
    return name.isEmpty() ? path : name;
}

QString ownSettingsFilePath() {
    /* **不要在这里手拼路径**:它要的就是 uiSettingsFilePath() 那个文件,而那条路是唯一权威
     * (核心库 sxcl_settings_default_path();SXCL_UI_SETTINGS 可以钉死)。
     *
     * 这里以前手拼了一份 %APPDATA%/SilentXCraftLauncher/settings.conf —— Windows 上碰巧一致,
     * 安卓/Linux 上核心库给的路径**完全不同**(与 ui_paths.cpp 里记的那个"读的写的不是同一个文件"
     * 是同一类事故),于是"当前版本 / 游戏目录 / 文件夹图标"这些键会读写两份文件。
     *
     * 2026-09-22 晚真机踩到:把 SXCL_UI_SETTINGS 钉到临时设置文件后,模组页/启动线程读的是临时文件,
     * 主页却还读真实配置 —— 现象是"主页显示 26.3、点启动说这个版本还没安装"。
     * 生产环境(Windows)两者恰好同路径,所以只在这里才暴露 —— 正是这类错误最危险的地方。 */
    return uiSettingsFilePath();
}

QString resolveGameDirectory() {
    char err[256];
    char buf[4096];
    err[0] = '\0';
    QString configured;
    if (sxcl_settings *st = sxcl_settings_open(ownSettingsFilePath().toUtf8().constData())) {
        configured = QString::fromUtf8(sxcl_settings_game_default_dir(st));
        sxcl_settings_free(st);
    }
    // 一次性迁移:从 Python 版配置里把 gameDirectory 导入我们自己的设置(只导一次)。
    if (configured.isEmpty()) {
        const QString legacy =
            legacyConfigString(QStringLiteral("Game"), QStringLiteral("gameDirectory"));
        if (!legacy.isEmpty()) {
            setGameDirectory(legacy);
            configured = legacy;
        }
    }
    if (!configured.isEmpty()) {
        if (sxcl_paths_resolve_game_dir(configured.toUtf8().constData(), buf, sizeof(buf), err,
                                        sizeof(err)) == SXCL_PATHS_OK)
            return QDir::fromNativeSeparators(QString::fromUtf8(buf));
        return QDir::fromNativeSeparators(configured);
    }
    if (sxcl_paths_default_game_dir(buf, sizeof(buf), err, sizeof(err)) == SXCL_PATHS_OK)
        return QDir::fromNativeSeparators(QString::fromUtf8(buf));
    return QDir::fromNativeSeparators(QDir::homePath()) + QStringLiteral("/.minecraft");
}

void setGameDirectory(const QString &dir) {
    writeSetting("game.default_dir", QDir::fromNativeSeparators(dir));
    rememberGameDir(dir);
}

int configuredMemoryMb() {
    const int mb = readSetting("game.max_memory_mb", QStringLiteral("0")).toInt();
    return mb > 0 ? mb : 0;
}

QStringList scanInstalledVersions(const QString &gameDir) {
    QStringList versions;
    const QDir versionsDir(gameDir + QStringLiteral("/versions"));
    if (!versionsDir.exists())
        return versions;
    const QFileInfoList entries =
        versionsDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QFileInfo &entry : entries) {
        const QString name = entry.fileName();
        const QString dir = entry.absoluteFilePath();
        if (QFileInfo::exists(dir + QLatin1Char('/') + name + QStringLiteral(".jar")) ||
            QFileInfo::exists(dir + QLatin1Char('/') + name + QStringLiteral(".json")) ||
            !QDir(dir).entryList(QStringList() << QStringLiteral("*.json"), QDir::Files).isEmpty())
            versions.append(name);
    }
    std::sort(versions.begin(), versions.end(), [](const QString &a, const QString &b) {
        return a > b; // Python: sorted(..., reverse=True)
    });
    return versions;
}

QString versionLoaderTag(const QString &gameDir, const QString &versionId) {
    QFile file(gameDir + QStringLiteral("/versions/") + versionId + QLatin1Char('/') +
               versionId + QStringLiteral(".json"));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() || doc.object().isEmpty())
        return QString();
    const QString id = versionId.toLower();
    // 判定顺序照抄 Python(先 forge,所以 neoforge 永远命中 Forge 分支 —— Python 里就是死代码)
    if (id.contains(QStringLiteral("forge")))
        return QStringLiteral("Forge");
    if (id.contains(QStringLiteral("neoforge")))
        return QStringLiteral("NeoForge");
    if (id.contains(QStringLiteral("fabric")))
        return QStringLiteral("Fabric");
    return QString();
}

QVector<GameFolder> detectGameFolders(const QString &configuredDir) {
    QVector<GameFolder> candidates;
    const QString programDir = QDir::fromNativeSeparators(QCoreApplication::applicationDirPath());
    for (const QString &name : {QStringLiteral(".minecraft"), QStringLiteral("minecraft"),
                                QStringLiteral("MC")})
        candidates.append(inspectFolder(programDir + QLatin1Char('/') + name,
                                        QStringLiteral("启动器目录（便携）")));
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty())
        candidates.append(inspectFolder(QDir::fromNativeSeparators(appData) +
                                            QStringLiteral("/.minecraft"),
                                        QStringLiteral("官方启动器（APPDATA）")));
    const QString home = QDir::fromNativeSeparators(QDir::homePath());
    candidates.append(inspectFolder(home + QStringLiteral("/.minecraft"), QStringLiteral("用户目录")));
    for (const QString &desktop : {QStringLiteral("Desktop"), QStringLiteral("桌面")})
        candidates.append(inspectFolder(home + QLatin1Char('/') + desktop +
                                            QStringLiteral("/.minecraft"),
                                        QStringLiteral("桌面")));
    if (!configuredDir.isEmpty())
        candidates.append(inspectFolder(configuredDir, QStringLiteral("当前配置")));
    // 用户导入/用过的文件夹(状态保留的那份历史)
    for (const QString &known : knownGameDirs())
        candidates.append(inspectFolder(known, QStringLiteral("用过/导入的")));

    QVector<GameFolder> folders;
    QSet<QString> seen;
    for (const GameFolder &folder : candidates) {
        const QString key = QDir(folder.path).absolutePath().toLower();
        if (seen.contains(key))
            continue;
        seen.insert(key);
        if (folder.exists)
            folders.append(folder);
    }
    std::stable_sort(folders.begin(), folders.end(),
                     [](const GameFolder &a, const GameFolder &b) { return a.score() > b.score(); });
    return folders;
}

const GameFolder *bestGameFolder(const QVector<GameFolder> &folders) {
    const GameFolder *first = nullptr;
    for (const GameFolder &folder : folders) {
        if (!first)
            first = &folder;
        if (folder.versions > 0)
            return &folder;
    }
    return first;
}

QString describeFolder(const GameFolder &folder) {
    return QStringLiteral("%1（%2，%3 个版本）").arg(folder.path, folder.label).arg(folder.versions);
}

QStringList knownGameDirs() {
    const QString raw = readSetting("game.known_dirs", QString());
    QStringList out;
    for (const QString &piece : raw.split(QLatin1Char('|'), Qt::SkipEmptyParts)) {
        const QString dir = QDir::fromNativeSeparators(piece.trimmed());
        if (!dir.isEmpty() && !out.contains(dir, Qt::CaseInsensitive))
            out.append(dir);
    }
    return out;
}

void rememberGameDir(const QString &dir) {
    if (dir.trimmed().isEmpty())
        return;
    QStringList dirs = knownGameDirs();
    dirs.removeAll(QDir::fromNativeSeparators(dir));
    dirs.prepend(QDir::fromNativeSeparators(dir));
    while (dirs.size() > 8)
        dirs.removeLast();
    writeSetting("game.known_dirs", dirs.join(QLatin1Char('|')));
}

QString selectedVersionName() { return readSetting("game.selected_version", QString()); }

void setSelectedVersionName(const QString &name) { writeSetting("game.selected_version", name); }

QString offlinePlayerName() {
    const QString name = readSetting("launch.offline_name", QStringLiteral("Player")).trimmed();
    return name.isEmpty() ? QStringLiteral("Player") : name;
}

void setOfflinePlayerName(const QString &name) {
    const QString trimmed = name.trimmed();
    writeSetting("launch.offline_name",
                 trimmed.isEmpty() ? QStringLiteral("Player") : trimmed);
}

void applyButtonFont(QPushButton *button) {
    QFont font = button->font();
    font.setPixelSize(14);
    font.setWeight(QFont::Normal);
    button->setFont(font);
    button->setFixedHeight(32);
}

} // namespace sxcl::ui
