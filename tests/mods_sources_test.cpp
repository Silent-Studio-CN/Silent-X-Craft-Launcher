/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 *
 * 模组 / 光影页「来源」那一层的验收（用户 2026-09-26 点名）：
 *   「模组下载的两个圆要作为勾选的选项，而不是搜一个模组从两个下面儿选，这点你要模仿 PCL 的理念。」
 *   「关于 CF 源的密钥配置，我后续给你，不要让用户自己填写。」
 *
 * 这份用例不看截图、不联网，只钉死"来源"这件事里可核对的部分：
 *   1) 两个源的名字与固定顺序；
 *   2) 勾选状态的落盘与读回：列表 <-> 串；旧版本的单值（"curseforge"）能升级；
 *      **从来没写过这个键** = 默认勾 Modrinth；**写过但是空** = 一个都不勾（不许偷偷改回默认）；
 *   3) 合并：两源结果并成一份，段序固定、源内保序、同源去重，
 *      **跨源绝不合并、绝不改来源**（两个源里同名的工程必须还是两条，各带自己的来源）；
 *   4) 每个**被请求过**的源都要有计数，0 也要写出来。
 */

#include <QApplication>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdio>
#include <cstring>

#include "sxcl/mods.h"
#include "workers/mods_merge.h"

using namespace sxcl::ui;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  [!!] %s\n", what.toUtf8().constData());
    }
}

static sxcl_mod_hit makeHit(const char *id, const char *source, const char *title) {
    sxcl_mod_hit hit;
    std::memset(&hit, 0, sizeof(hit));
    std::snprintf(hit.id, sizeof(hit.id), "%s", id);
    std::snprintf(hit.source, sizeof(hit.source), "%s", source);
    std::snprintf(hit.title, sizeof(hit.title), "%s", title);
    return hit;
}

static sxcl_mod_page makePage(sxcl_mod_hit *items, size_t count, size_t total) {
    sxcl_mod_page page;
    page.items = items;
    page.count = count;
    page.total = total;
    page.offset = 0;
    return page;
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    const QStringList all = modsAllSources();

    // ---- 1) 两个源 ----
    check(all == QStringList{QStringLiteral("modrinth"), QStringLiteral("curseforge")},
          QStringLiteral("两个源且顺序固定：%1").arg(all.join(QLatin1Char(','))));
    check(modsSourceDisplayName(QStringLiteral("modrinth")) == QLatin1String("Modrinth") &&
              modsSourceDisplayName(QStringLiteral("curseforge")) == QLatin1String("CurseForge"),
          QStringLiteral("源名显示：Modrinth / CurseForge"));
    check(modsSourceDisplayName(QStringLiteral("cf")) == QLatin1String("cf"),
          QStringLiteral("认不出的源原样返回，不改名冒充"));
    check(modsSourceValid(QStringLiteral("curseforge")) &&
              !modsSourceValid(QStringLiteral("cf")),
          QStringLiteral("只有两个合法源"));

    // ---- 2) 落盘 / 读回 ----
    check(parseModsSources(QStringLiteral("modrinth,curseforge")) == all,
          QStringLiteral("列表串读回两个源"));
    check(parseModsSources(QStringLiteral("curseforge")) ==
              QStringList{QStringLiteral("curseforge")},
          QStringLiteral("旧版本的单值能读（升级）"));
    check(parseModsSources(QStringLiteral("curseforge,modrinth")) == all,
          QStringLiteral("顺序规范化：与勾选先后无关"));
    check(parseModsSources(QStringLiteral("modrinth,modrinth")).size() == 1,
          QStringLiteral("同一个词重复只算一次"));
    check(parseModsSources(QStringLiteral("cf,nope, ")).isEmpty(),
          QStringLiteral("认不出的词丢掉"));
    check(parseModsSources(QString()).isEmpty(), QStringLiteral("空串 = 一个都不勾"));
    check(formatModsSources(QStringList{QStringLiteral("curseforge"), QStringLiteral("modrinth")}) ==
              QLatin1String("modrinth,curseforge"),
          QStringLiteral("落盘串固定长相：modrinth,curseforge"));
    check(parseModsSources(formatModsSources(all)) == all,
          QStringLiteral("落盘 -> 读回 一致"));
    check(modsSourcesFromStored(nullptr) == QStringList{QStringLiteral("modrinth")},
          QStringLiteral("从来没写过这个键 = 默认勾 Modrinth"));
    check(modsSourcesFromStored("").isEmpty(),
          QStringLiteral("写过但是空 = 一个都不勾（不偷偷改回默认）"));
    check(modsSourcesFromStored("curseforge") == QStringList{QStringLiteral("curseforge")},
          QStringLiteral("老设置文件照样能读"));

    // ---- 3) 合并 ----
    sxcl_mod_hit mine[2] = {makeHit("A", "modrinth", "Sodium"),
                            makeHit("B", "modrinth", "Lithium")};
    sxcl_mod_hit cf[3] = {makeHit("11", "curseforge", "Sodium"),
                          makeHit("22", "curseforge", "JEI"),
                          makeHit("22", "curseforge", "JEI-dup")}; // 同一个 id 的第二条
    sxcl_mod_page pageMine = makePage(mine, 2, 1234);
    sxcl_mod_page pageCf = makePage(cf, 3, 57);
    QVector<ModsHitRow> rows;
    appendModsHits(rows, pageCf);   // 故意让 CurseForge **先**回来
    appendModsHits(rows, pageMine);
    check(rows.size() == 4, QStringLiteral("合并后 4 条（2 + 3 去掉同源重复 1 条），实际 %1")
                                .arg(rows.size()));
    if (rows.size() == 4) {
        check(rows.at(0).source == QLatin1String("modrinth") &&
                  rows.at(1).source == QLatin1String("modrinth"),
              QStringLiteral("先回来的 CurseForge 也排在后面（段序由源定）"));
        check(rows.at(2).source == QLatin1String("curseforge") &&
                  rows.at(3).source == QLatin1String("curseforge"),
              QStringLiteral("CurseForge 段跟在 Modrinth 段后面"));
        check(rows.at(0).id == QLatin1String("A") && rows.at(1).id == QLatin1String("B"),
              QStringLiteral("源内保持上游顺序"));
        check(rows.at(2).id == QLatin1String("11") && rows.at(3).id == QLatin1String("22") &&
                  rows.at(3).title == QLatin1String("JEI"),
              QStringLiteral("同源重复的 id 只留第一条（先出现的那条赢）"));
    }
    int sodium = 0;
    for (const ModsHitRow &row : rows) {
        if (row.title == QLatin1String("Sodium")) {
            ++sodium;
        }
    }
    check(sodium == 2, QStringLiteral("两个源里同名的工程还是两条（跨源不合并、不冒充）"));
    bool sourcesIntact = true;
    for (const ModsHitRow &row : rows) {
        if (!modsSourceValid(row.source)) {
            sourcesIntact = false;
        }
    }
    check(sourcesIntact, QStringLiteral("每一条都带着自己的真实来源"));

    // 字段整份拷出来（核心库那份随 page 释放，卡片回调里还要用）
    sxcl_mod_hit one = makeHit("X1", "modrinth", "Iris");
    std::snprintf(one.author, sizeof(one.author), "%s", "\xe4\xbd\x9c\xe8\x80\x85");
    std::snprintf(one.description, sizeof(one.description), "%s", "desc");
    std::snprintf(one.icon_url, sizeof(one.icon_url), "%s", "https://example.invalid/i.png");
    std::snprintf(one.versions, sizeof(one.versions), "%s", "1.20.1 1.20.2");
    std::snprintf(one.slug, sizeof(one.slug), "%s", "iris");
    one.downloads = 424242;
    sxcl_mod_page pageOne = makePage(&one, 1, 1);
    QVector<ModsHitRow> copied;
    appendModsHits(copied, pageOne);
    check(copied.size() == 1 && copied.at(0).id == QLatin1String("X1") &&
              copied.at(0).slug == QLatin1String("iris") &&
              copied.at(0).iconUrl == QLatin1String("https://example.invalid/i.png") &&
              copied.at(0).versions == QLatin1String("1.20.1 1.20.2") &&
              copied.at(0).downloads == 424242 && copied.at(0).description == QLatin1String("desc"),
          QStringLiteral("id/slug/图标/版本/下载量/简介 整份拷贝"));

    // 没有 id 的条目不许摆上去（摆上去点不了「装」）
    sxcl_mod_hit noId = makeHit("", "modrinth", "no-id");
    sxcl_mod_page pageNoId = makePage(&noId, 1, 1);
    QVector<ModsHitRow> skipped;
    appendModsHits(skipped, pageNoId);
    check(skipped.isEmpty(), QStringLiteral("没有 id 的条目不上列表"));

    // 空页 / 空指针不许炸，也不许改变已有结果
    sxcl_mod_page empty = makePage(nullptr, 0, 0);
    const int before = rows.size();
    appendModsHits(rows, empty);
    check(rows.size() == before, QStringLiteral("空页不改变任何东西"));

    // ---- 4) 每源计数（0 也要写出来）----
    // 注意用 QStringLiteral 而不是 QLatin1String：分隔符是 U+00B7，按 Latin-1 读会变成两个字节
    check(modsPerSourceCounts(rows, all) == QStringLiteral("Modrinth 2 · CurseForge 2"),
          QStringLiteral("计数：%1").arg(modsPerSourceCounts(rows, all)));
    check(modsPerSourceCounts(rows, QStringList{QStringLiteral("curseforge"),
                                                QStringLiteral("modrinth")}) ==
              QStringLiteral("Modrinth 2 · CurseForge 2"),
          QStringLiteral("计数顺序固定（与勾选先后无关）"));
    check(modsPerSourceCounts(rows, QStringList{QStringLiteral("modrinth")}) ==
              QLatin1String("Modrinth 2"),
          QStringLiteral("只勾一个源时只写它"));
    QVector<ModsHitRow> onlyMine;
    appendModsHits(onlyMine, pageMine);
    check(modsPerSourceCounts(onlyMine, all) == QStringLiteral("Modrinth 2 · CurseForge 0"),
          QStringLiteral("没结果的源写成 0，不许省略成【没有它】"));
    check(modsPerSourceCounts(rows, QStringList()).isEmpty(),
          QStringLiteral("一个源都没请求 = 空串"));

    std::printf("mods-sources: pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
