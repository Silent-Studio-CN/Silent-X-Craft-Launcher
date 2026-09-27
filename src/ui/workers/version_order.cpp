/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 版本号的解析与比较(设计理由逐条写在 version_order.h)。

#include "version_order.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace sxcl::ui {
namespace {

/* 老版本前缀 -> 纪元(越小越老)。名字前面的这一个字母决定了它排在哪个年代:
 *   rd / c / inf / in  = Classic 与更早的那些预览版
 *   a = Alpha,b = Beta(它们都比 1.0 老)
 *   v = "v1.2.3" 这种多余的写法,剥掉不改变年代。
 * 顺序要紧:"inf-" 必须排在 "in-" 前面,否则 "inf-20100618" 会被当成 "in-" + "f-…"。 */
struct VersionPrefix {
    const char *literal;
    int epoch;
};

const VersionPrefix kPrefixes[] = {
    {"rd-", -3}, {"inf-", -3}, {"in-", -3}, {"c", -3}, {"a", -2}, {"b", -1}, {"v", 0},
};

/* 同号不同档:正式版 > 候选版(rc)> 预览版(pre)。1.21.4-pre1 必须排在 1.21.4 **下面**、
 * 1.21.3 **上面** —— 这就是为什么档位是"数字段之后再加两段",而不是另开一套排序。 */
constexpr int kStageRelease = 3;
constexpr int kStageRc = 2;
constexpr int kStagePre = 1;

/* 年份式命名(2026 年起 Mojang 用「26.1」这种两段式)与快照「25w14a」换算到同一根轴上量:
 * 都写成 2000+年份。判据只看第一段是否 >= 25 —— 在那之前 MC 从来没发布过主版本号大于 1 的
 * 正式版(2.0 只出现在公告里,没发布),所以不会误伤 1.x 那一大串。 */
constexpr int kYearStyleThreshold = 25;
constexpr int kYearBase = 2000;

/* 快照:14w02a / 25w14a(**只有**这种形状算,别把任意 "数字w数字" 当版本)。 */
const QRegularExpression &snapshotRe() {
    static const QRegularExpression re(QStringLiteral("^(\\d{2,4})w(\\d{1,2})([a-z])?$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

/* 前导的数字段:必须**至少有一个小数点** —— "114514" / "424242" 这种目录名不算版本号
 * (它们排在认得出的一组后面,而不是被当成 114514 版)。 */
const QRegularExpression &leadRe() {
    static const QRegularExpression re(QStringLiteral("^(\\d+(?:\\.\\d+)+)"));
    return re;
}

/* 版本号后面那一截里的档位:-pre1 / -rc2 / Pre-Release 1(大小写不敏感)。 */
const QRegularExpression &stageRe() {
    static const QRegularExpression re(
        QStringLiteral("^[-_ ]?(pre|rc)(?:[-_ ]?(\\d+))?"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

/** 逐段比较(段不够按 0 补):第一个不相等的段定胜负。 */
int compareParts(const QVector<int> &a, const QVector<int> &b) {
    const int n = qMax(a.size(), b.size());
    for (int i = 0; i < n; ++i) {
        const int av = i < a.size() ? a.at(i) : 0;
        const int bv = i < b.size() ? b.at(i) : 0;
        if (av != bv)
            return av < bv ? -1 : 1;
    }
    return 0;
}

} // namespace

McVersionKey parseMcVersion(const QString &text) {
    McVersionKey key;
    key.text = text.trimmed();
    QString body = key.text;
    if (body.isEmpty())
        return key;

    int epoch = 0;
    for (const VersionPrefix &prefix : kPrefixes) {
        const int len = int(qstrlen(prefix.literal));
        if (body.size() <= len)
            continue;
        if (body.left(len).compare(QLatin1String(prefix.literal), Qt::CaseInsensitive) != 0)
            continue;
        const QChar next = body.at(len);
        // 前缀后面必须紧跟数字或 '-'("classic" / "beta" 这类词不是版本前缀)
        if (!next.isDigit() && next != QLatin1Char('-'))
            continue;
        epoch = prefix.epoch;
        body = body.mid(len);
        break;
    }
    if (body.isEmpty())
        return key;

    // ① 快照:25w14a -> [0, 2025, 14, 字母]
    const QRegularExpressionMatch snap = snapshotRe().match(body);
    if (snap.hasMatch()) {
        int year = snap.captured(1).toInt();
        if (year < 100)
            year += kYearBase;
        int letter = 0;
        if (!snap.captured(3).isEmpty())
            letter = snap.captured(3).toLower().at(0).unicode() - QLatin1Char('a').unicode() + 1;
        key.known = true;
        key.parts = QVector<int>{epoch, year, snap.captured(2).toInt(), letter};
        return key;
    }

    // ② 数字段(必须带小数点)
    const QRegularExpressionMatch lead = leadRe().match(body);
    if (!lead.hasMatch())
        return key;
    const QStringList segments = lead.captured(1).split(QLatin1Char('.'));
    QVector<int> numbers;
    numbers.reserve(segments.size());
    for (const QString &segment : segments)
        numbers.append(segment.toInt());
    if (numbers.isEmpty())
        return key;
    if (numbers.first() >= kYearStyleThreshold)
        numbers[0] += kYearBase; // 年份式命名与快照同一根轴(见上)

    // ③ 档位:剩下的那一截里认 pre / rc(认不出 = 正式版)
    int stage = kStageRelease;
    int stageNumber = 0;
    const QString rest = body.mid(lead.captured(1).size());
    if (!rest.isEmpty()) {
        const QRegularExpressionMatch st = stageRe().match(rest);
        if (st.hasMatch()) {
            stage = st.captured(1).compare(QLatin1String("rc"), Qt::CaseInsensitive) == 0 ? kStageRc
                                                                                            : kStagePre;
            stageNumber = st.captured(2).isEmpty() ? 0 : st.captured(2).toInt();
        }
    }

    key.known = true;
    key.parts = QVector<int>{epoch} + numbers + QVector<int>{stage, stageNumber};
    return key;
}

int compareMcVersion(const McVersionKey &a, const McVersionKey &b) {
    if (a.known != b.known)
        return a.known ? 1 : -1; // 认出版本号的永远排在认不出的前面
    if (!a.known)
        return 0; // 两个都不是版本号:交给调用方按名字定序
    return compareParts(a.parts, b.parts);
}

int compareVersionText(const QString &a, const QString &b) {
    const McVersionKey ka = parseMcVersion(a);
    const McVersionKey kb = parseMcVersion(b);
    const int byVersion = compareMcVersion(ka, kb);
    if (byVersion != 0)
        return byVersion;
    // 同号(或都认不出):按名字倒序 —— 顺序必须**确定**,不能随扫描顺序抖
    const int byName = QString::compare(ka.text, kb.text, Qt::CaseInsensitive);
    return byName == 0 ? 0 : (byName > 0 ? 1 : -1);
}

QString instanceSortKey(const InstalledInstance &inst) {
    const QString base = recognizedBaseVersion(inst);
    if (!base.isEmpty())
        return base;
    return inst.id;
}

namespace {

/** 一组内部排序:版本号从新到旧。用 stable_sort + 确定性的比较器(同号按名字倒序),
 *  同一个 instances 表跑两次结果一模一样(验收要按顺序断言,顺序不许抖)。 */
void sortGroup(QVector<InstalledInstance> *group) {
    std::stable_sort(group->begin(), group->end(),
                     [](const InstalledInstance &a, const InstalledInstance &b) {
                         return compareVersionText(instanceSortKey(a), instanceSortKey(b)) > 0;
                     });
}

} // namespace

VersionDisplay orderInstancesForDisplay(const QVector<InstalledInstance> &instances) {
    VersionDisplay out;
    for (const InstalledInstance &inst : instances) {
        if (inst.launchable)
            out.healthy.append(inst);
        else
            out.broken.append(inst);
    }
    sortGroup(&out.healthy);
    sortGroup(&out.broken);
    return out;
}

} // namespace sxcl::ui
