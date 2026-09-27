/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 已安装版本扫描(设计理由见 instance_scan.h)。

#include "instance_scan.h"

#include <cstring>

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include "sxcl/instance.h"
#include "sxcl/manifest.h"  // 缺什么 = sxcl_version_plan_build 展开出来的清单里目标不在磁盘上的那些

namespace sxcl::ui {

namespace {

/** 核心库那条记录 -> 界面层的 InstalledInstance(装箱只写这一处:列表扫描与单个扫描共用)。 */
InstalledInstance instanceFromCore(const sxcl_instance &inst) {
    InstalledInstance item;
    item.id = QString::fromUtf8(inst.id);
    item.type = QString::fromUtf8(inst.version_type[0] != 0 ? inst.version_type : "release");
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
    return item;
}

} // namespace

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
        InstalledInstance item = instanceFromCore(list.items[i]);
        /* 「缺什么」只在**不能启动**的实例上数(用户 2026-09-27 要的统计)。
         * 能启动的版本不缺东西,为它们把几百个依赖库挨个 stat 一遍纯属白烧 IO ——
         * 而且验证这条路正是"点一下卡一下"的来源(本文件开头写着的那个教训)。 */
        if (!item.launchable)
            fillInstanceMissingFacts(&item, gameDir);
        out.append(item);
    }
    sxcl_instance_list_free(&list);
    return out;
}

bool scanInstalledInstance(const QString &gameDir, const QString &id, InstalledInstance *out,
                           QString *errorOut) {
    if (out == nullptr)
        return false;
    *out = InstalledInstance();
    if (errorOut != nullptr)
        errorOut->clear();
    if (gameDir.isEmpty() || id.isEmpty()) {
        if (errorOut != nullptr)
            *errorOut = QStringLiteral("还没选版本（或游戏目录为空）");
        return false;
    }
    sxcl_instance inst;
    std::memset(&inst, 0, sizeof(inst));
    char err[SXCL_INSTANCE_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_instance_scan_one(gameDir.toUtf8().constData(), id.toUtf8().constData(),
                                          &inst, err, sizeof(err));
    if (rc != SXCL_INSTANCE_OK) {
        if (errorOut != nullptr)
            *errorOut = QString::fromUtf8(err[0] != '\0' ? err : "这个实例读不出来");
        return false;
    }
    *out = instanceFromCore(inst); // 与列表扫描**同一份**装箱
    if (!out->launchable)
        fillInstanceMissingFacts(out, gameDir); // 与列表扫描同一条规矩:只给坏的那些数
    return true;
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

/* 「到底缺哪些文件」的原料:把一份版本 JSON 展开成"它需要哪些文件",再看目标路径在不在。
 * 用的是**下载器自己那个展开器**(sxcl_version_plan_build)—— 补文件那一遍跑的也是它,
 * 所以"数出来的缺"与"补得上的"永远是同一件事(自己另写一套判断,迟早就走岔)。
 * 返回目标路径(本机分隔符,未去重)。 */
QStringList missingTargetsOf(const sxcl_json *doc, const QString &gameDir, const QString &versionId) {
    QStringList missing;
    if (doc == nullptr)
        return missing;
    char err[192];
    err[0] = '\0';
    sxcl_version_plan *plan =
        sxcl_version_plan_build(doc, gameDir.toUtf8().constData(), versionId.toUtf8().constData(),
                                err, sizeof(err));
    if (plan == nullptr)
        return missing; // 展开不了(JSON 残缺到拼不出清单):留给核心库那一句笼统的话
    const size_t count = sxcl_version_plan_count(plan);
    for (size_t i = 0; i < count; ++i) {
        const sxcl_task *task = sxcl_version_plan_task(plan, i);
        if (task == nullptr || task->dest == nullptr || task->dest[0] == '\0')
            continue;
        const QString dest = QDir::fromNativeSeparators(QString::fromUtf8(task->dest));
        if (QFileInfo::exists(dest))
            continue;
        missing.append(dest);
    }
    sxcl_version_plan_free(plan);
    return missing;
}

} // namespace

QString recognizedBaseVersion(const InstalledInstance &inst) {
    /* 两条路,顺序不能换:
     *   ① 版本文件里**写着**的(clientVersion / patches / inheritsFrom / --fml.mcVersion / jar,
     *      核心库认出来时才置 base_reliable)—— 那是权威;
     *   ② 版本文件自己的 id(只在它与目录名不同时才看:与目录名相同 = 本来就是"按目录名猜",
     *      用户 2026-09-26 点名不要这种行为)。 */
    if (!inst.baseVersion.isEmpty() && inst.baseReliable)
        return inst.baseVersion;
    return versionFromJsonId(inst.jsonId, inst.id);
}

void fillInstanceMissingFacts(InstalledInstance *item, const QString &gameDir) {
    if (item == nullptr)
        return;
    item->missing.clear();
    item->missingLibraries = 0;

    char jsonPath[1024];
    jsonPath[0] = '\0';
    char err[192];
    err[0] = '\0';
    sxcl_json *doc = sxcl_instance_read_json(gameDir.toUtf8().constData(),
                                             item->id.toUtf8().constData(), jsonPath,
                                             sizeof(jsonPath), err, sizeof(err));
    if (doc == nullptr) {
        /* 版本文件本身就读不出来:除了前置版本那句,别的都无从谈起(没有清单可展开)。
         * 区分"没有"与"坏了" —— 用户看到的是人话,不是一个笼统的"出错了"。 */
        if (item->problemCode == SXCL_INSTANCE_PROBLEM_BAD_JSON)
            item->missing.append(QStringLiteral("版本文件坏了"));
        else
            item->missing.append(QStringLiteral("缺版本文件"));
        if (!item->missingParent.isEmpty())
            item->missing.append(QStringLiteral("缺前置版本 %1").arg(item->missingParent));
        return;
    }

    QStringList targets = missingTargetsOf(doc, gameDir, item->id);

    /* 有继承时,父版本那一层要用的文件同样是"这个版本要用的文件" ——
     * Forge/Fabric 实例自己的 JSON 里只有一个加载器壳,原版那些库与客户端 jar 全在父版本那一层。
     * 父版本的版本文件不在磁盘上时清单展不开,那就落到"缺前置版本"那一条(核心库已经判过)。 */
    const QString parent =
        QString::fromUtf8(sxcl_json_get_string(sxcl_json_root(doc), "inheritsFrom", ""));
    if (!parent.isEmpty()) {
        char perr[192];
        perr[0] = '\0';
        sxcl_json *parentDoc = sxcl_instance_read_json(gameDir.toUtf8().constData(),
                                                      parent.toUtf8().constData(), nullptr, 0, perr,
                                                      sizeof(perr));
        if (parentDoc != nullptr) {
            targets += missingTargetsOf(parentDoc, gameDir, parent);
            sxcl_json_free(parentDoc);
        }
    }
    sxcl_json_free(doc);

    /* 归类成人话。**不写路径、不写 libraries/assets 这种词** ——
     * 用户要知道的是"缺什么、缺几个",不是我们的目录长什么样(完整清单在 tooltip 里)。 */
    const QString librariesDir = QDir::fromNativeSeparators(
        QDir(gameDir).filePath(QStringLiteral("libraries")));
    const QString assetsDir =
        QDir::fromNativeSeparators(QDir(gameDir).filePath(QStringLiteral("assets")));
    int libraries = 0;
    int assets = 0;
    int jars = 0;
    int others = 0;
    for (const QString &dest : targets) {
        if (dest.startsWith(librariesDir + QLatin1Char('/'), Qt::CaseInsensitive)) {
            ++libraries;
        } else if (dest.startsWith(assetsDir + QLatin1Char('/'), Qt::CaseInsensitive)) {
            ++assets;
        } else if (dest.endsWith(QLatin1String(".jar"), Qt::CaseInsensitive)) {
            ++jars; // versions/<版本>/<版本>.jar —— 游戏本体(含父版本那一层的)
        } else {
            ++others;
        }
    }
    if (jars > 0)
        item->missing.append(QStringLiteral("缺游戏本体文件"));
    item->missingLibraries = libraries;
    if (libraries > 0)
        item->missing.append(QStringLiteral("缺依赖库 %1 个").arg(libraries));
    if (assets > 0)
        item->missing.append(QStringLiteral("缺资源文件"));
    if (others > 0)
        item->missing.append(QStringLiteral("缺其它文件 %1 个").arg(others));
    if (!item->missingParent.isEmpty())
        item->missing.append(QStringLiteral("缺前置版本 %1").arg(item->missingParent));
}

namespace {

/** 加载器 kind_id -> **人话名**(核心库 sxcl_instance_kind_id 的那一套小写 id)。
 *  认不出来就原样写回去:宁可写个英文词,也不写"未知加载器"这种解释性废话。 */
QString loaderDisplayName(const QString &kindId) {
    const QString k = kindId.trimmed().toLower();
    if (k == QLatin1String("forge"))
        return QStringLiteral("Forge");
    if (k == QLatin1String("neoforge"))
        return QStringLiteral("NeoForge");
    if (k == QLatin1String("fabric"))
        return QStringLiteral("Fabric");
    if (k == QLatin1String("quilt"))
        return QStringLiteral("Quilt");
    if (k == QLatin1String("optifine"))
        return QStringLiteral("OptiFine");
    if (k == QLatin1String("liteloader"))
        return QStringLiteral("LiteLoader");
    if (k == QLatin1String("vanilla"))
        return QString(); // 原版不是"加载器",不进这一段
    return kindId.trimmed();
}

/** 这个字符串**像不像一个版本号**:至少得有一个数字。
 *  为什么需要这条:PCL 的 setup.ini 里 LiteLoader / OptiFine 那几项是**开关**,值是
 *  True / False;核心库照原样把它们当版本号收下了(instance.c 的 INST_SETUP_RULES),
 *  于是界面上就出现了 "Forge 14.23.5.2864 + LiteLoader False" 这种"字段名 = 布尔值"的
 *  机器话 —— 用户 2026-09-27 点名不许出现在界面上。认不出数字就**只写加载器名字**
 *  (用户口径:"没有版本号就只写名字")。 */
bool looksLikeVersion(const QString &text) {
    for (const QChar &c : text) {
        if (c.isDigit())
            return true;
    }
    return false;
}

/** 加载器那一段的**人话**:"Forge 14.23.5.2847 + OptiFine" / "Fabric 0.15.11" / 空 = 没加载器。
 *  **不存在的东西一个字都不写**(没装的加载器根本不在 inst.loaders 里),也绝不写
 *  True / False / unknown / 字段名 / maven 坐标。 */
QString loaderSummaryText(const InstalledInstance &inst) {
    QStringList parts;
    for (const QVariant &value : inst.loaders) {
        const QStringList pair = value.toStringList();
        if (pair.isEmpty())
            continue;
        const QString name = loaderDisplayName(pair.value(0));
        if (name.isEmpty())
            continue;
        const QString version = pair.value(1).trimmed();
        parts.append(looksLikeVersion(version) ? QStringLiteral("%1 %2").arg(name, version) : name);
    }
    return parts.join(QStringLiteral(" + "));
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
    out.base = recognizedBaseVersion(inst); // 唯一一份判据(排序也用它)
    if (out.base.isEmpty()) {
        out.baseFrom = QStringLiteral("none");
    } else if (!inst.baseVersion.isEmpty() && inst.baseReliable) {
        out.baseFrom = (!inst.inheritsFrom.isEmpty() && inst.inheritsFrom == inst.baseVersion)
                           ? QStringLiteral("core:inheritsFrom")
                           : QStringLiteral("core:json-other");
    } else {
        out.baseFrom = QStringLiteral("json:id");
    }

    /* 加载器那一段**自己拼人话**,不再直接摆核心库的 summary 原文(用户 2026-09-27:
     * 「你是不是有病?能分得清什么是给人看的、什么是给机器看的吗?」)——
     * 核心库那条 summary 是给机器/日志看的:它会把 setup.ini 里的开关值(False)当版本号写出来。
     * 这里只写**装了的加载器**(人话名 + 有数字的版本号)、没有的东西一个字不写。 */
    const QString summary = loaderSummaryText(inst);
    if (inst.launchable) {
        out.state = QString::fromLatin1("grass");
        /* ② 能启动的行:只说"这是什么版本"(原版 x / 加载器 · 原版 x)。以前这里还写
         *    "有 jar / 无自己的 jar" —— 对玩家没用,删。 */
        if (summary.isEmpty()) {
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
    /* 动作词:用户 2026-09-27 点名「把"去下载"改成"修复"」——
     * 点了真的去**补文件**(versions_select_page 的 repairVersion -> workers/repair_worker),
     * 不是跳去下载页把用户丢在那儿。 */
    out.action = QStringLiteral("修复");
    out.reason = inst.problem.isEmpty()
                     ? QString::fromUtf8(sxcl_instance_problem_default_text(
                           static_cast<sxcl_instance_problem>(inst.problemCode)))
                     : inst.problem;


    /* 行内那句话 = **数出来的缺什么**(用户 2026-09-27:「缺什么东西你给我统计出来」)。
     * missing 里每一条都自带结论("缺依赖库 12 个" / "版本文件坏了"),所以直接顿号连起来念
     * 就是一句人话。数不出来时(例如文件夹名与版本文件对不上 —— 那什么都不缺)才退回核心库
     * 那一句笼统的。最多念三条,再多就"等"(完整清单在 tooltip 里,那里想写多全都行)。 */
    QString fallbackNote;
    switch (static_cast<sxcl_instance_problem>(inst.problemCode)) {
    case SXCL_INSTANCE_PROBLEM_NO_JSON:
        fallbackNote = QStringLiteral("缺版本文件");
        break;
    case SXCL_INSTANCE_PROBLEM_BAD_JSON:
        fallbackNote = QStringLiteral("版本文件坏了");
        break;
    case SXCL_INSTANCE_PROBLEM_MISSING_JAR:
        fallbackNote = QStringLiteral("缺游戏本体文件");
        break;
    case SXCL_INSTANCE_PROBLEM_MISSING_PARENT:
        fallbackNote = inst.missingParent.isEmpty()
                           ? QStringLiteral("缺前置版本")
                           : QStringLiteral("缺前置版本 %1").arg(inst.missingParent);
        break;
    case SXCL_INSTANCE_PROBLEM_ID_MISMATCH:
        fallbackNote = QStringLiteral("文件夹名对不上版本文件");
        break;
    default:
        fallbackNote = QStringLiteral("这一份现在起不来");
        break;
    }
    if (inst.missing.isEmpty()) {
        out.note = fallbackNote;
    } else {
        QStringList shown = inst.missing;
        if (shown.size() > 3) {
            shown = shown.mid(0, 3);
            shown.last() += QStringLiteral("等");
        }
        out.note = shown.join(QStringLiteral("、"));
    }

    QStringList tip;
    tip << inst.id;
    if (!inst.missing.isEmpty()) {
        // 缺什么逐条列出来(比行内那一句更全:行内最多三条,这里一条不落)
        tip << inst.missing.join(QLatin1Char('\n'));
    }
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
