/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

// ── 游戏文件夹 + 已安装版本 + 两个新状态键(主页与版本选择页共用) ──
//
// 2026-09-22 界面重构(docs/25)从 home_page.cpp 里搬出来的:
// 原来这套(文件夹探测/版本扫描/设置读写)只有主页用,现在"版本选择页"也要,
// 所以提成一份实现 —— 两页看到的东西必须**完全一致**,不能各算各的。
//
// 用户点名的两条在这里落地:
//   * 版本 = **versions/ 下的文件夹名**,与"MC 版本号"无关(文件夹叫 114514 也能跑);
//   * "当前文件夹"显示的是**文件夹自己的名字**,不强制叫 .minecraft;
//   * 切换文件夹/选版本/离线 ID 都要**状态保留**(写我们自己的设置文件)。

#include <QString>
#include <QStringList>
#include <QVector>

class QPushButton;

namespace sxcl::ui {

// 一个候选游戏文件夹(folders.py:49-101 的 GameFolder)
struct GameFolder {
    QString path;
    QString label;      // 来源说明(启动器目录 / 官方启动器（APPDATA） / 用户目录 / 桌面 / 当前配置)
    int versions = 0;
    bool hasAssets = false;
    bool hasProfiles = false;
    bool exists = false;
    int score() const;                 // folders.py:64-74
    QString name() const;              // 文件夹自己的名字(用户要求显示它)
};

// 我们自己的设置文件路径(sxcl/settings.h 那条唯一权威)
QString ownSettingsFilePath();
// 游戏目录:设置 game.default_dir > 平台默认 > <home>/.minecraft
QString resolveGameDirectory();
// 把当前游戏目录落盘(用户点"更改/自动检测/导入文件夹"时都要存)
void setGameDirectory(const QString &dir);
// 设置里的最大内存(<=0 = 交给核心库按位数取默认)
int configuredMemoryMb();

// versions/ 下"有 jar 或 json"的目录名,名字倒序(installed.py:38-57)
QStringList scanInstalledVersions(const QString &gameDir);
// 只看 id 里有没有加载器关键字(home_page.py:212-230;判定顺序照抄 Python)
QString versionLoaderTag(const QString &gameDir, const QString &versionId);

// 文件夹探测(folders.py:112-151)+ 挑选(folders.py:154-160)+ 描述(folders.py:163-164)
QVector<GameFolder> detectGameFolders(const QString &configuredDir);
const GameFolder *bestGameFolder(const QVector<GameFolder> &folders);
QString describeFolder(const GameFolder &folder);

// ── 状态保留(docs/25 §8) ──
// game.known_dirs:用户导入/用过的文件夹历史(用 '|' 分隔的一份字符串)
QStringList knownGameDirs();
void rememberGameDir(const QString &dir);
// game.selected_version:当前版本 = **文件夹名**
QString selectedVersionName();
void setSelectedVersionName(const QString &name);
// launch.offline_name:离线启动的 ID(默认 "Player")
QString offlinePlayerName();
void setOfflinePlayerName(const QString &name);

// 按钮字体(主页/选择页共用;home_page.py 里那份的等价物)
void applyButtonFont(QPushButton *button);

} // namespace sxcl::ui
