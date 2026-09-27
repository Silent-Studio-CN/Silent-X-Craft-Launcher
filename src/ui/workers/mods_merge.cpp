/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 模组页「来源」的纯逻辑实现（为什么这样切见 mods_merge.h）。

#include "mods_merge.h"

#include <QRegularExpression>
#include <QSet>

#include "sxcl/mods.h"

namespace sxcl::ui {
namespace {

/** 合并列表里两段的先后：认不出的源排在最后（不丢、也不插到已知源中间）。 */
int sourceRank(const QString &source) {
    const QStringList all = modsAllSources();
    for (int i = 0; i < all.size(); ++i) {
        if (source == all.at(i)) {
            return i;
        }
    }
    return int(all.size());
}

QString fromC(const char *text) { return QString::fromUtf8(text); }

/** 去重键：**按源分开**。不同源里的同一个 id 是两件事，不能互相顶掉。 */
QString rowKey(const QString &source, const QString &id) {
    return source + QLatin1Char('\x1f') + id;
}

} // namespace

QStringList modsAllSources() {
    return QStringList{QStringLiteral("modrinth"), QStringLiteral("curseforge")};
}

QString modsSourceDisplayName(const QString &source) {
    if (source == QLatin1String("modrinth")) {
        return QStringLiteral("Modrinth");
    }
    if (source == QLatin1String("curseforge")) {
        return QStringLiteral("CurseForge");
    }
    return source; // 认不出就原样 —— 不改名冒充
}

bool modsSourceValid(const QString &source) {
    return modsAllSources().contains(source);
}

QStringList parseModsSources(const QString &stored) {
    const QStringList tokens =
        stored.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
    QStringList out;
    for (const QString &known : modsAllSources()) {
        for (const QString &token : tokens) {
            if (token.compare(known, Qt::CaseInsensitive) == 0) {
                out << known;
                break;
            }
        }
    }
    return out;
}

QString formatModsSources(const QStringList &sources) {
    // 先规范化(去重 + 固定顺序),再落盘 —— 磁盘上永远只有一种长相
    return parseModsSources(sources.join(QLatin1Char(','))).join(QLatin1Char(','));
}

QStringList modsSourcesFromStored(const char *storedOrNull) {
    if (storedOrNull == nullptr) {
        return QStringList{QStringLiteral("modrinth")}; // 从来没写过这个键 = 默认这一源
    }
    return parseModsSources(fromC(storedOrNull));
}

ModsHitRow modsHitRowFromCore(const sxcl_mod_hit &hit) {
    ModsHitRow row;
    row.id = fromC(hit.id);
    row.slug = fromC(hit.slug);
    row.title = fromC(hit.title);
    row.author = fromC(hit.author);
    row.description = fromC(hit.description);
    row.iconUrl = fromC(hit.icon_url);
    row.source = fromC(hit.source);
    row.versions = fromC(hit.versions);
    row.downloads = (qlonglong)hit.downloads;
    return row;
}

void appendModsHits(QVector<ModsHitRow> &rows, const sxcl_mod_page &page) {
    if (page.items == nullptr || page.count == 0) {
        return;
    }
    QSet<QString> seen;
    seen.reserve(int(rows.size()) * 2);
    for (const ModsHitRow &row : rows) {
        seen.insert(rowKey(row.source, row.id));
    }
    for (size_t i = 0; i < page.count; ++i) {
        const ModsHitRow row = modsHitRowFromCore(page.items[i]);
        if (row.id.isEmpty()) {
            continue; // 没有 id 的条目在界面上点不了「装」,不摆上去
        }
        const QString key = rowKey(row.source, row.id);
        if (seen.contains(key)) {
            continue;
        }
        seen.insert(key);
        /* 段序:插到**第一个比它大的段**前面(没有就追加)。
         * 这样"先回来的源"不会因为回来得早就跑到列表最前面 —— 勾选先后、回来先后都不改变长相。 */
        const int rank = sourceRank(row.source);
        int insertAt = int(rows.size());
        for (int k = 0; k < rows.size(); ++k) {
            if (sourceRank(rows.at(k).source) > rank) {
                insertAt = k;
                break;
            }
        }
        rows.insert(insertAt, row);
    }
}

QString modsPerSourceCounts(const QVector<ModsHitRow> &rows, const QStringList &requested) {
    const QStringList wanted = parseModsSources(requested.join(QLatin1Char(',')));
    QStringList bits;
    for (const QString &source : wanted) {
        int count = 0;
        for (const ModsHitRow &row : rows) {
            if (row.source == source) {
                ++count;
            }
        }
        bits << QStringLiteral("%1 %2").arg(modsSourceDisplayName(source)).arg(count);
    }
    return bits.join(QStringLiteral(" · "));
}

} // namespace sxcl::ui
