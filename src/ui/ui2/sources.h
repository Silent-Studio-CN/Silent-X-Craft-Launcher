/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 主页的**数据来源**:这里全是阻塞活(磁盘 / 解密 / 目录探测),只许在 Task 的工作线程里调。
//
// 为什么单独一个文件(docs/27 §6 第 1 条的落地):pages/** 里不许出现
//   sxcl_http_get_text / sxcl_manifest_* / sxcl_instance_list / QFile ——
// 页面只写"要什么数据",取数据这件事住在这里。每个函数头一句 requireWorkerThread(),
// 界面线程上误调会落一行 MAIN-THREAD-VIOLATION(验收脚本断言 count=0)。
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace sxcl::ui2 {

struct VersionItem {
    QString id;        // 版本名(目录名)
    QString summary;   // "原版" / "Forge 47.2.0 + OptiFine I6"
    QString type;      // release / snapshot / old_beta …(空 = release)
    QString base;      // 继承的原版(空 = 就是原版自己)
    bool launchable = true;
    QString problem;   // 不能启动的人话原因(空 = 没问题)
};

struct HomeData {
    QString gameDir;
    QString gameDirReason; // 游戏目录为什么是它(人话,§14 的同一条纪律)
    QString error;         // 扫不动的原因(空 = 没问题;空版本列表不一定是错)
    QVector<VersionItem> versions;
};

struct PlayerData {
    bool read = false;     // 令牌文件真的读过了(没读过 ≠ 没登录)
    bool loggedIn = false; // 解出会话且有玩家名
    QString name;          // 正版 ID(空 = 这个账号没有 Java 版)
    QString uuid;
    QString error;
};

/** 阻塞:解析游戏目录(含一次平台探测)+ 扫 <gameDir>/versions。 */
HomeData loadHomeData();
/** 阻塞:解密读账户令牌文件(不联网)。 */
PlayerData loadPlayerData();

/** 版本形态偏好:设置键 game.edition(键沿用下载页那个;home 这一侧只认 offline = 离线,
 *  其余值(含下载页写的 java/bedrock)= 正版)。 */
bool readPremiumEdition();
void savePremiumEdition(bool premium);

/** 离线玩家名:设置键 player.name(空 = 默认 Steve)。 */
QString readOfflineName();
void saveOfflineName(const QString &name);

/** 公告正文:设置键 ui.notice(多条用 '|' 分隔);环境变量 SXCL_UI2_NOTICE 是验收夹具,优先。
 *  空 = 主页**不出现**公告卡(docs/27 §11.4:没有内容时整块不出现,绝不写"暂无公告")。 */
QStringList readNoticeLines();

} // namespace sxcl::ui2
