/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 *
 * 文件夹自定义图标的验收（用户 2026-09-22 晚点名的那条）：
 *   「可以自定义文件夹图标（目前内置狐狸头，铁砧，工作台，草方块等等），SXCL 用的图标都给他支持」
 *
 * 这份用例**不看截图**，只看可核对的四件事：
 *   1) 图标目录里有多少项、每一项的 id 都认得出来、都能真的取到图（空图 = 界面上是个洞）；
 *   2) 选择的持久化：写进去 -> 读出来一致；多个文件夹互不干扰；Windows 大小写与斜杠等价；
 *   3) 认不出来的旧 id 回落到默认（草方块），不会留一个空白图标；
 *   4) 页面内的选择器（createFolderIconPicker）确实把**每一项**都铺出来了，
 *      点第一项能回调到 onPick —— 这条替代"我去看图"。
 *
 * 设置文件被重定向到临时目录（APPDATA 改掉），绝不碰用户真实配置。
 */

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QWidget>

#include <cstdio>
#include <cstring>

#include "folder_icons.h"
#include "pages/game_folders.h"

using namespace sxcl::ui;

static int g_pass = 0, g_fail = 0;
static void check(int ok, const QString &what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  [!!] %s\n", what.toUtf8().constData());
    }
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    // 设置文件重定向到构建目录下的临时点（APPDATA 一改，ownSettingsFilePath 就跟着走）
    const QString tmp = QDir::currentPath() + QStringLiteral("/_ui_folder_icons_tmp");
    QDir(tmp).removeRecursively();
    QDir().mkpath(tmp);
    qputenv("APPDATA", tmp.toUtf8());
    check(ownSettingsFilePath().startsWith(tmp), QStringLiteral("设置文件已重定向到临时目录"));

    // 1) 目录：14 方块 + 13 界面 = 27
    const QVector<FolderIconOption> catalog = folderIconCatalog();
    check(catalog.size() == 27, QStringLiteral("图标目录 27 项（实际 %1）").arg(catalog.size()));
    int blocks = 0, pcl = 0, nullIcons = 0, invalidIds = 0;
    for (const FolderIconOption &opt : catalog) {
        if (opt.id.startsWith(QStringLiteral("block:")))
            ++blocks;
        if (opt.id.startsWith(QStringLiteral("pcl:")))
            ++pcl;
        if (!folderIconIdValid(opt.id))
            ++invalidIds;
        if (folderIconForId(opt.id, 24).isNull())
            ++nullIcons;
        if (opt.title.isEmpty())
            ++invalidIds;
    }
    check(blocks == 14, QStringLiteral("方块图 14 项（实际 %1）").arg(blocks));
    check(pcl == 13, QStringLiteral("界面图 13 项（实际 %1）").arg(pcl));
    check(invalidIds == 0, QStringLiteral("每一项都有 id 与人话名"));
    check(nullIcons == 0, QStringLiteral("每一项都能取到图（空图会在界面上留洞）"));

    // 用户点名的那几个必须在内（狐狸 = NeoForge 的方块图、铁砧 = Forge、草方块 = 原版）
    const QStringList wanted = {QStringLiteral("block:neoforge"), QStringLiteral("block:forge"),
                                QStringLiteral("block:vanilla")};
    for (const QString &id : wanted) {
        check(folderIconIdValid(id), QStringLiteral("点名要有的图标：%1").arg(id));
    }

    // 2) 持久化
    const QString folderA = tmp + QStringLiteral("/mc-a");
    const QString folderB = tmp + QStringLiteral("/mc-b");
    check(folderIconId(folderA) == defaultFolderIconId(), QStringLiteral("没设过 -> 默认（草方块）"));
    check(defaultFolderIconId() == QStringLiteral("block:vanilla"), QStringLiteral("默认就是草方块"));

    setFolderIconId(folderA, QStringLiteral("block:forge"));
    setFolderIconId(folderB, QStringLiteral("pcl:mod"));
    check(folderIconId(folderA) == QStringLiteral("block:forge"), QStringLiteral("A 记住的是铁砧"));
    check(folderIconId(folderB) == QStringLiteral("pcl:mod"), QStringLiteral("B 记住的是模组图标"));

    // 再写一次 A：不重复堆积，读出来还是它
    setFolderIconId(folderA, QStringLiteral("block:neoforge"));
    check(folderIconId(folderA) == QStringLiteral("block:neoforge"), QStringLiteral("改图标是覆盖不是追加"));
    check(folderIconId(folderB) == QStringLiteral("pcl:mod"), QStringLiteral("改 A 不影响 B"));

    // 路径规范：反斜杠/大小写（Windows）等价
    {
        QString alt = folderA;
        alt.replace(QLatin1Char('/'), QLatin1Char('\\'));
#if defined(Q_OS_WIN)
        alt = alt.toUpper();
#endif
        check(folderIconId(alt) == QStringLiteral("block:neoforge"),
              QStringLiteral("同一路径的另一种写法也认得（分隔符/大小写）"));
    }

    // 3) 失效 id 回落
    setFolderIconId(folderA, QStringLiteral("block:this-icon-is-gone"));
    check(folderIconId(folderA) == defaultFolderIconId(), QStringLiteral("失效 id 回落到默认"));

    // 4) 页面内的选择器：每一项都铺出来 + 点一下能回调
    setFolderIconId(folderA, defaultFolderIconId());
    QString picked;
    QWidget *picker = createFolderIconPicker(
        folderA, QStringLiteral("mc-a"), folderIconId(folderA),
        [&picked](const QString &id) { picked = id; }, nullptr);
    check(picker != nullptr, QStringLiteral("选择器建出来了"));
    const QList<QToolButton *> buttons = picker->findChildren<QToolButton *>();
    // 27 个图标 + 1 个"恢复默认"按钮（PushButton 也是 QToolButton 的兄弟，这里只数图标格）
    int iconCells = 0;
    for (QToolButton *b : buttons) {
        if (b->toolTip().isEmpty() == false)
            ++iconCells;
    }
    check(iconCells == catalog.size(),
          QStringLiteral("选择器把 %1 项都铺出来了（实际 %2）").arg(catalog.size()).arg(iconCells));
    // 点第一格 -> onPick 收到它的 id
    for (QToolButton *b : buttons) {
        if (!b->toolTip().isEmpty()) {
            b->click();
            break;
        }
    }
    check(!picked.isEmpty(), QStringLiteral("点一下图标会回调 onPick"));
    check(folderIconIdValid(picked), QStringLiteral("回调回来的 id 有效：%1").arg(picked));
    delete picker;

    QDir(tmp).removeRecursively();
    std::printf("folder_icons 测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
