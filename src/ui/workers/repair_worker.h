/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 「修复」—— 版本选择页那一行动作词背后真正干的活。
//
// 用户 2026-09-27 原话:
//   「这个版本识别,说什么去下载,那破坏的、已经残缺的版本,缺什么东西你给我统计出来;
//     然后把"去下载"改成"修复" —— 整个去下载跳到下载页儿,后面儿啥也没有了,你给用户当傻逼呢。」
//
// 所以这个文件存在的唯一理由:用户点「修复」,我们**真的把这个版本补起来**,而不是把他丢到下载页。
// 补的方式与产品里已有的三条路**完全同一套**(绝不另写一份下载/校验/镜像逻辑):
//   1) 版本文件(versions/<名>/<名>.json)不在/读不动 -> 从版本清单里按名字把它取回来,
//      用 sxcl_install_write_version_json 落盘(它做 tmp + 原子改名,重复执行不会弄坏已有的好文件);
//   2) 依赖库 / 客户端 jar / 资源索引 -> 用 sxcl_version_plan_build 展开成清单,
//      再交给下载引擎跑一遍(与"启动前补全文件"同一个展开器、同一个引擎);
//   3) 有继承(inheritsFrom)时,父版本那一层**单独展一张清单**一起补
//      (加载器实例自己的 JSON 里没有原版那些东西,补不到父版本等于没补)。
//
// 纪律:本文件里全是**阻塞**代码(网络 + 磁盘),只许在 worker(BgTask)里调 —— 界面线程一次都不许。
#pragma once

#include <QString>
#include <QStringList>

#include <atomic>

namespace sxcl::ui {

/** 一次修复的结果(全是事实,界面照它说话)。 */
struct RepairReport {
    bool fetchedVersionJson = false; /**< 版本文件是这次从清单里取回来的 */
    QStringList fetchedExtras;       /**< 顺带取回来的版本文件(父版本那一层) */
    int filesTotal = 0;              /**< 清单里一共要几个文件 */
    int filesDownloaded = 0;         /**< 这次真的补上了几个 */
    int filesSkipped = 0;            /**< 已经在磁盘上、一个字节都没下的 */
    int filesFailed = 0;             /**< 没补上的 */
    qint64 bytesDone = 0;            /**< 这次真的写下去的字节数 */
    QStringList still;               /**< 补完之后**还缺什么**(人话;空 = 补齐了) */
};

/** 修一个实例(阻塞)。返回 true = 补完之后**一个都不缺了**;
 *  false 时 error 里是人话原因(能修的部分照样修了,只是没收全 —— 页面照实说)。
 *  cancel 可空:页面已经走了就置位,网络与文件循环会在边界上停下。 */
bool repairInstance(const QString &gameDir, const QString &instanceId, const QString &settingsFile,
                    std::atomic<bool> *cancel, RepairReport *out, QString *error);

/** 补完之后这个实例还缺什么(人话短语;空表 = 一个都不缺)。
 *  单独暴露出来:验收要能"修完再数一遍",而不是只看修复函数自己报的账。 */
QStringList instanceMissingFacts(const QString &gameDir, const QString &instanceId);

} // namespace sxcl::ui
