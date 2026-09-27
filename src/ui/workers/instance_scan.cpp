/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 已安装版本扫描(设计理由见 instance_scan.h)。

#include "instance_scan.h"

#include <cstring>

#include <QDir>
#include <QRegularExpression>
#include <QStringList>

#include "sxcl/instance.h"

namespace sxcl::ui {

QVector<InstalledInstance> scanInstalledInstances(const QString &gameDir, QString *errorOut) {
    QVector<InstalledInstance> out;
    if (errorOut != nullptr)
        errorOut->clear();
    const QByteArray dir = gameDir.toUtf8();
    if (dir.isEmpty()) {
        if (errorOut != nullptr)
            *errorOut = QStringLiteral("游戏目录为空(还没选过目录)");
        return out;
    }
    sxcl_instance_list list;
    std::memset(&list, 0, sizeof(list));
    char err[SXCL_INSTANCE_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_instance_scan(dir.constData(), nullptr, &list, err, sizeof(err));
    if (rc != SXCL_INSTANCE_OK) {
        if (errorOut != nullptr)
            *errorOut = QString::fromUtf8(err[0] != '\0' ? err : "扫描本地实例失败");
        return out;
    }
    out.reserve(static_cast<int>(list.count));
    for (size_t i = 0; i < list.count; ++i) {
        const sxcl_instance &inst = list.items[i];
        InstalledInstance item;
        item.id = QString::fromUtf8(inst.id);
        item.type = QString::fromUtf8(inst.version_type[0] != '\0' ? inst.version_type : "release");
        item.summary = QString::fromUtf8(inst.summary);
        item.problem = QString::fromUtf8(inst.problem);
        item.launchable = inst.launchable != 0;
        item.hasJar = inst.has_jar != 0;
        item.hasJson = inst.has_json != 0;
        item.problemCode = inst.problem_code;
        item.baseVersion = QString::fromUtf8(inst.base_version);
        item.baseReliable = inst.base_reliable != 0;
        // 展示口径(versionRowInfo)要用的几件事实:JSON 自己写的 id / 继承关系 / 缺哪个前置
        item.jsonId = QString::fromUtf8(inst.json_id);
        item.inheritsFrom = QString::fromUtf8(inst.inherits_from);
        item.missingParent = QString::fromUtf8(inst.missing_parent);
        item.jsonPath = QString::fromUtf8(inst.json_path);
        for (size_t k = 0; k < inst.loader_count; ++k) {
            const sxcl_instance_loader &ld = inst.loaders[k];
            // 内层是 QStringList{kind_id, version} —— 与模型/委托的约定一致
            // (委托用 item.toStringList() 读它;写成 QVariantList 会让加载器小标签**静默消失**)
            item.loaders.append(QStringList{
                QString::fromUtf8(sxcl_instance_kind_id(ld.kind)),
                QString::fromUtf8(ld.version),
            });
        }
        out.append(item);
    }
    sxcl_instance_list_free(&list);
    return out;
}

// ── 版本行的展示口径(唯一一份;理由逐条写在 instance_scan.h)───────────────────

namespace {

/* 版本 JSON 的 id 里认版本号。**只在 id 与目录名不同**时才用 ——
 * 目录名与 JSON id 一样的那些实例,核心库早就在目录名那一支上兜过底了(reliable=0),
 * 再从 id 里抠一遍等于把"按目录名猜"换个来源写出来,用户点名不要这种行为。
 * 形状卡得很死:必须以 \d+.\d+ 开头,后面要么到头、要么跟一个非数字非点号的字符
 * (“1.12.2”“1.12.2-forge-14.23.5.2859”认;“114514”“forge-1.0”不认)。 */
QString versionFromJsonId(const QString &jsonId, const QString &folderName) {
    const QString id = jsonId.trimmed();
    if (id.isEmpty() || id == folderName)
        return QString();
    static const QRegularExpression re(QStringLiteral("^(\\d+\\.\\d+(?:\\.\\d+)?(?:-(?:pre|rc)\\d+)?)(?![0-9.])"));
    const QRegularExpressionMatch m = re.match(id);
    return m.hasMatch() ? m.captured(1) : QString();
}

} // namespace

VersionRowInfo versionRowInfo(const InstalledInstance &inst, const QString &gameDir) {
    VersionRowInfo out;
    out.path = QDir(gameDir).filePath(QStringLiteral("versions/%1").arg(inst.id));

    /* ① 原版版本号:只认**版本文件里写着的**(核心库 base_reliable=1)。
     *    核心库不回报具体是哪一支字段给的,这里只能按"和 inheritsFrom 同不同"分个类:
     *    同 = core:inheritsFrom,不同 = core:json-other(clientVersion / patches /
     *    --fml.mcVersion / jar 之一)。核心库认不出来 -> 再看 JSON 的 id(见上);
     *    两条路都认不出来 -> **什么都不写**(用户 2026-09-26:「认不出来就什么都不写」)。 */
    out.coreReliable = inst.baseReliable;
    if (!inst.baseVersion.isEmpty() && inst.baseReliable) {
        out.base = inst.baseVersion;
        out.baseFrom = (!inst.inheritsFrom.isEmpty() && inst.inheritsFrom == inst.baseVersion)
                           ? QStringLiteral("core:inheritsFrom")
                           : QStringLiteral("core:json-other");
    } else {
        const QString fromId = versionFromJsonId(inst.jsonId, inst.id);
        if (!fromId.isEmpty()) {
            out.base = fromId;
            out.baseFrom = QStringLiteral("json:id");
        } else {
            out.baseFrom = QStringLiteral("none");
        }
    }

    const QString summary = inst.summary.trimmed();
    if (inst.launchable) {
        out.state = QString::fromLatin1("grass");
        /* ② 能启动的行:只说"这是什么版本"。以前这里还写"有 jar / 无自己的 jar" ——
         *    对玩家没用("jar"是什么?正常版本来就不该有/可以有,取决于装法),删。
         *    summary 本身就是"原版"时不重复写第二遍(以前是"原版 · 原版 1.12.2")。 */
        if (summary.isEmpty() || summary == QLatin1String("原版")) {
            out.info = out.base.isEmpty() ? QString() : QStringLiteral("原版 %1").arg(out.base);
        } else {
            out.info = out.base.isEmpty() ? summary
                                          : QStringLiteral("%1 · 原版 %2").arg(summary, out.base);
        }
        out.tip = QStringLiteral("点一下就用它启动（%1）").arg(inst.id);
        return out;
    }

    /* ③ 不能启动的行:**一句话 + 一个动作**。完整的核心库原因与路径只进 tooltip。
     *    每一句话都必须是"用户能看懂、且真的发生了"的事,不解释实现、不倒字段名。 */
    out.state = QString::fromLatin1("warn");
    out.action = QStringLiteral("去下载");
    out.reason = inst.problem.isEmpty()
                     ? QString::fromUtf8(sxcl_instance_problem_default_text(
                           static_cast<sxcl_instance_problem>(inst.problemCode)))
                     : inst.problem;
    switch (static_cast<sxcl_instance_problem>(inst.problemCode)) {
    case SXCL_INSTANCE_PROBLEM_NO_JSON:
        out.note = QStringLiteral("缺版本文件");
        break;
    case SXCL_INSTANCE_PROBLEM_BAD_JSON:
        out.note = QStringLiteral("版本文件坏了");
        break;
    case SXCL_INSTANCE_PROBLEM_MISSING_JAR:
        out.note = QStringLiteral("缺游戏本体文件");
        break;
    case SXCL_INSTANCE_PROBLEM_MISSING_PARENT:
        out.note = inst.missingParent.isEmpty()
                       ? QStringLiteral("缺前置版本")
                       : QStringLiteral("缺前置版本 %1").arg(inst.missingParent);
        break;
    case SXCL_INSTANCE_PROBLEM_ID_MISMATCH:
        out.note = QStringLiteral("文件夹名对不上版本文件");
        break;
    default:
        out.note = QStringLiteral("这一份现在起不来");
        break;
    }

    QStringList tip;
    tip << inst.id;
    tip << out.reason;
    tip << QStringLiteral("位置：%1").arg(QDir::toNativeSeparators(out.path));
    if (!out.base.isEmpty() && out.baseFrom == QLatin1String("json:id")) {
        // JSON 自己说它是哪个版本 —— 目录名对不上时这是**唯一**能告诉用户的东西
        tip << QStringLiteral("版本文件里写的版本：%1").arg(out.base);
    }
    out.tip = tip.join(QLatin1Char('\n'));
    out.info = QString();
    return out;
}

} // namespace sxcl::ui
