/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 模组 / 光影页「来源」那一层的纯逻辑（用户 2026-09-26 点名）：
//   「模组下载的两个圆要作为勾选的选项，而不是搜一个模组从两个下面儿选，这点你要模仿 PCL 的理念。」
//
// 也就是说：两个源是**可同时勾选的筛选项**（勾上哪个就搜哪个），不是"先选一个源、再在它下面搜"；
// 结果**合并成一份列表**，每一条自带真实来源 —— 两个源之间绝不互相冒充。
//
// 这份刻意做成"只吃字符串与纯数据"的纯逻辑：单测直接调它，不需要窗口、不需要网络
// （tests/mods_sources_test.cpp）。
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct sxcl_mod_hit;  // 核心库 include/sxcl/mods.h
struct sxcl_mod_page;

namespace sxcl::ui {

/** 两个源（与核心库 sxcl_mod_hit::source 同一套小写串）。顺序 = 界面上的固定顺序，
 *  也是合并列表里两段的先后 —— **勾选顺序不该改变列表长相**。 */
QStringList modsAllSources();
/** "modrinth" -> "Modrinth"。认不出的源**原样返回**（宁可显示得难看，也不改名冒充）。 */
QString modsSourceDisplayName(const QString &source);
bool modsSourceValid(const QString &source);

/** 落盘串 -> 勾选列表。单个值（旧版本只存过 "modrinth" / "curseforge"）天然也能读；
 *  认不出的词丢掉，结果按 modsAllSources() 的固定顺序去重。 */
QStringList parseModsSources(const QString &stored);
/** 勾选列表 -> 落盘串（"modrinth,curseforge"；一个都没勾 = 空串）。 */
QString formatModsSources(const QStringList &sources);
/** 设置文件里读到的原样值 -> 勾选列表：
 *   * nullptr（**从来没写过这个键**）-> 默认**两个源都勾上**（勾选框只表达"不想用哪个"，不表达偏好）；
 *   * 写过但解析不出（含显式空串 = 用户把两个都取消了）-> 一个都不勾，**不偷偷改回默认**。 */
QStringList modsSourcesFromStored(const char *storedOrNull);

/** 合并列表里的一条：核心库 sxcl_mod_hit 的值拷贝。
 *  核心库那份内存随 sxcl_mods_page_free() 释放，而卡片的「装」回调还要用它，所以必须自己拷一份。 */
struct ModsHitRow {
    QString id;
    QString slug;
    QString title;
    QString author;
    QString description;
    QString iconUrl;
    QString source;   // "modrinth" / "curseforge" —— 这一行**真实的**来源
    QString versions; // 空格分隔（支持的 MC 版本）
    qlonglong downloads = 0;
};

/** 核心库的一条命中 -> 合并列表的一条。 */
ModsHitRow modsHitRowFromCore(const sxcl_mod_hit &hit);

/** 把一页搜索结果并进 rows：
 *   * 同一个源里重复的 id 只留第一次出现的那条（上游分页/重复时不该出现两行一样的卡片）；
 *   * **不跨源合并**：两个源里的同名工程是两个东西，合成一条就等于拿一个冒充另一个
 *     （用户明确要"两个源"而不是"一个源"）；
 *   * 两段的先后由 modsAllSources() 定，与请求回来的先后、与勾选顺序都无关。 */
void appendModsHits(QVector<ModsHitRow> &rows, const sxcl_mod_page &page);

/** 每个**被请求过**的源各出了多少条，例如 "Modrinth 20 · CurseForge 0"。
 *  没有结果的源也要写出来（0 就是 0，不许省略成"没有它"）—— 用户才能看出是"这个源没结果"
 *  还是"这个源根本没搜"。没请求过任何源 = 空串。 */
QString modsPerSourceCounts(const QVector<ModsHitRow> &rows, const QStringList &requested);

} // namespace sxcl::ui
