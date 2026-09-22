/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 文件夹自定义图标（用户 2026-09-22 晚点名）：
//   「可以自定义文件夹图标（包括资源中文件夹的图标）。目前内置狐狸头，铁砧，工作台，草方块等等。
//    SXCL 用的图标都给他支持」
//
// 图标全部来自我们**已有的**两套资产，不新增任何图片：
//   * 方块图 assets/icons/blocks/*.png —— SxclIcons 那 14 种
//     （草方块 / 命令方块 / 圆石 / 铁砧 / **狐狸**（NeoForge 的标识）/ 布料 / 土径 / 鸡蛋 /
//      红石块 / 金块 / 红石灯（亮/灭）…）；
//   * PCL 语义图 assets/icons/pcl/*.svg —— IconRegistry 那 13 个语义名
//     （主页 / 下载 / 任务 / 联机 / 更多 / 设置 / 帮助 / 个性化 / 启动 / 刷新 / 搜索 / 模组 / 光影）。
//
// 选择结果落在我们自己的设置文件里：game.folder_icons = "id=绝对路径|id=绝对路径"。
// 于是"同一台机器多个文件夹各自不同""下次打开还记得"都成立（路径里不会出现 '|' 与 '='，
// 所以这个分隔法对 Windows / POSIX 都安全）。
//
// **不做**"打开系统文件框挑一张图"：用户明确要求页面不许出现原生点选的画风。
// 挑图标一律弹在页面里（createFolderIconPicker），点一下立刻生效。
#pragma once

#include <QIcon>
#include <QString>
#include <QVector>

#include <functional>

class QWidget;

namespace sxcl::ui {

// 一个可选的文件夹图标
struct FolderIconOption {
    QString id;    // "block:forge" / "pcl:mod"
    QString title; // 人话名（铁砧 / 模组）
    QString group; // 分组标题（方块 / 界面）
};

// 全部可选项（方块在前、界面在后；顺序稳定，界面按它排布）
QVector<FolderIconOption> folderIconCatalog();
// 这个 id 现在还有效吗（设置里存的可能是旧版本删掉的图标）
bool folderIconIdValid(const QString &id);
// id -> 图标；认不出来的 id 回退默认图标（草方块）
QIcon folderIconForId(const QString &id, int size = 24);
// 默认图标：草方块（原版 = 一个游戏文件夹最自然的样子，与版本状态图标同一套语义）
QString defaultFolderIconId();
// 这个文件夹当前用的图标 id（没设过/设的已失效 -> 默认）
QString folderIconId(const QString &folderPath);
// 记住这个文件夹的图标（写 game.folder_icons）
void setFolderIconId(const QString &folderPath, const QString &id);

// 页面内的图标选择器（不弹原生对话框）：返回一张卡片，里面按分组铺满图标，
// 点一下调 onPick(id) 并由调用方收场（通常是把右栏切回版本列表 + 刷新侧栏图标）。
QWidget *createFolderIconPicker(const QString &folderPath, const QString &folderTitle,
                                const QString &currentId,
                                const std::function<void(const QString &)> &onPick,
                                QWidget *parent);

/** **弹窗版**图标选择器（用户 2026-09-22 晚点名：「图标切换做弹窗。颜色选择器样式，不做单页」）。
 *  Qt::Popup：点外面 / 按 Esc 自动关，不会把页面挤成两页；锚点通常在那一行的齿轮上。
 *  选完先关窗再回调。 */
void showFolderIconPopup(QWidget *anchor, const QString &folderPath, const QString &folderTitle,
                         const std::function<void(const QString &)> &onPick);

} // namespace sxcl::ui
