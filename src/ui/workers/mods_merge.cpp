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
        /* 从来没写过这个键 = **两个源都勾上**。勾选框的语义只有一个：用户**不想用哪个就取消勾选**；
         * 默认全给上（用户 2026-09-27：「默认两个都勾上」「是排除用户不想用的」）。 */
        return modsAllSources();
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
    row.versionsMin = fromC(hit.versions_min);
    row.versionsMax = fromC(hit.versions_max);
    row.updated = fromC(hit.updated);
    row.downloads = (qlonglong)hit.downloads;
    return row;
}

QString modsDownloadText(qlonglong downloads) {
    if (downloads < 10000) {
        return QString::number(downloads); // 一万以内就是原数
    }
    /* 截断到一位小数（不是四舍五入）:9999.96 万不许说成 1 亿。
     * 整数（35000 -> 3.5 万? 不是:3.5 有小数; 20000 -> 2.0 -> "2"）不带多余的 ".0"。 */
    const auto scaled = [](qlonglong value, qlonglong unit) {
        const qlonglong tenths = (value % unit) * 10 / unit; // 0..9
        QString out = QString::number(value / unit);
        if (tenths > 0) {
            out += QLatin1Char('.');
            out += QString::number(tenths);
        }
        return out;
    };
    if (downloads < 100000000LL) {
        return scaled(downloads, 10000LL) + QStringLiteral("万");
    }
    return scaled(downloads, 100000000LL) + QStringLiteral("亿");
}

QString modsUpdatedText(const QString &iso, const QDateTime &now) {
    const QString text = iso.trimmed();
    if (text.isEmpty() || !now.isValid()) {
        return QString(); // 上游没给时间 = 这一格不写(不编)
    }
    /* Modrinth: "2026-09-01T12:00:00Z";CF: "2026-09-01T12:00:00.789Z"（带毫秒）。
     * 两种都试;都认不出就空串。 */
    QDateTime when = QDateTime::fromString(text, Qt::ISODateWithMs);
    if (!when.isValid()) {
        when = QDateTime::fromString(text, Qt::ISODate);
    }
    if (!when.isValid()) {
        return QString();
    }
    const qint64 secs = when.toUTC().secsTo(now.toUTC());
    if (secs < 0) {
        return QStringLiteral("刚刚"); // 上游时间比本机时钟还新:不说"负几分钟前"这种怪话
    }
    if (secs < 60) {
        return QStringLiteral("刚刚");
    }
    if (secs < 3600) {
        return QStringLiteral("%1 分钟前").arg(secs / 60);
    }
    if (secs < 86400) {
        return QStringLiteral("%1 小时前").arg(secs / 3600);
    }
    if (secs < 86400LL * 30) {
        return QStringLiteral("%1 天前").arg(secs / 86400);
    }
    if (secs < 86400LL * 365) {
        return QStringLiteral("%1 个月前").arg(secs / (86400LL * 30));
    }
    return QStringLiteral("%1 年前").arg(secs / (86400LL * 365));
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
