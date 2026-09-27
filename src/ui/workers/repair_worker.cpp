/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 「修复」的实现(设计理由见 repair_worker.h)。
//
// **只能在 worker 里跑**:整条路都是网络 + 磁盘(取版本文件、展开清单、几百个文件 stat/下载)。

#include "repair_worker.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <QDir>
#include <QFile>
#include <QStringList>

#include "sxcl/engine.h"
#include "sxcl/http.h"
#include "sxcl/install.h"
#include "sxcl/instance.h"
#include "sxcl/json.h"
#include "sxcl/manifest.h"
#include "sxcl/net.h"

#include "instance_scan.h" // InstalledInstance / fillInstanceMissingFacts(「缺什么」的唯一口径)
#include "launch_worker.h"   // uiDownloadEngineOpts:与安装 / 启动前补全**同一份**下载口径
#include "ui_paths.h"

#if defined(SXCL_UI_HAVE_QT_TRANSPORT)
#define SXCL_REPAIR_HAVE_TRANSPORT 1
#endif

namespace sxcl::ui {
namespace {

/* 与 versions_page.cpp 的 kManifestOfficialUrl 是同一个地址(那边是版本列表,这边是修复取
 *  版本文件);改地址要两处一起改。 */
const char *const kManifestOfficialUrl =
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
constexpr int64_t kHttpTimeoutMs = 12000; // 单次请求的等待上限

int cancelRequested(void *userdata) {
    const auto *flag = static_cast<const std::atomic<bool> *>(userdata);
    return (flag != nullptr && flag->load()) ? 1 : 0;
}

/** 拿一段文本。失败时 error 是人话(照实说:连不上 / 对方没给内容)。 */
QByteArray fetchText(sxcl_transport *transport, const QString &url, QString *error) {
#if defined(SXCL_REPAIR_HAVE_TRANSPORT)
    if (transport == nullptr) {
        if (error != nullptr)
            *error = QStringLiteral("这台机器上没有可用的网络后端");
        return {};
    }
    char *text = nullptr;
    size_t len = 0;
    char err[256];
    err[0] = '\0';
    sxcl_http_opts opts;
    std::memset(&opts, 0, sizeof(opts));
    opts.timeout_ms = kHttpTimeoutMs;
    const QByteArray urlUtf8 = url.toUtf8();
    const int rc = sxcl_http_get_text_ex(transport, urlUtf8.constData(), nullptr, &opts, &text, &len,
                                         err, sizeof(err));
    if (rc != SXCL_HTTP_OK || text == nullptr || len == 0) {
        if (error != nullptr)
            *error = QString::fromUtf8(err[0] != '\0' ? err : "没拿到内容");
        if (text != nullptr)
            free(text);
        return {};
    }
    const QByteArray body(text, static_cast<int>(len));
    free(text);
    return body;
#else
    Q_UNUSED(transport)
    Q_UNUSED(url)
    if (error != nullptr)
        *error = QStringLiteral("这个构建里没有网络后端");
    return {};
#endif
}

/** 清单文本:两条路(**顺序按设置里的下载源**)—— 官方与 BMCLAPI 镜像各试一次。
 *  取证通路与版本页同一族入口:SXCL_UI_MANIFEST=<文件> 直接读文件(离线复现)。 */
QByteArray manifestText(sxcl_transport *transport, QString *error) {
    const QString pinnedFile = qEnvironmentVariable("SXCL_UI_MANIFEST");
    if (!pinnedFile.isEmpty()) {
        QFile file(pinnedFile);
        if (file.open(QIODevice::ReadOnly))
            return file.readAll();
        if (error != nullptr)
            *error = QStringLiteral("指定的版本清单文件打不开");
        return {};
    }

    char mirror[512];
    mirror[0] = '\0';
    const bool haveMirror =
        sxcl_manifest_mirror_url(kManifestOfficialUrl, nullptr, mirror, sizeof(mirror)) == 0;
    const QString pinnedUrl = qEnvironmentVariable("SXCL_UI_MANIFEST_URL");
    const bool mirrorFirst = (uiDownloadSource() != QLatin1String("mojang"));

    QStringList candidates;
    if (!pinnedUrl.isEmpty()) {
        candidates << pinnedUrl; // 钉住:只试这一个
    } else {
        if (mirrorFirst && haveMirror)
            candidates << QString::fromUtf8(mirror);
        candidates << QString::fromUtf8(kManifestOfficialUrl);
        if (!mirrorFirst && haveMirror)
            candidates << QString::fromUtf8(mirror);
    }
    QStringList failures;
    for (const QString &url : candidates) {
        QString why;
        const QByteArray body = fetchText(transport, url, &why);
        if (!body.isEmpty())
            return body;
        failures << why;
    }
    if (error != nullptr)
        *error = failures.isEmpty() ? QStringLiteral("拿不到版本清单") : failures.first();
    return {};
}

/** 这个版本号的版本文件地址(清单里那一条的 url;空 = 清单里没有它)。 */
QString versionJsonUrl(const QByteArray &manifest, const QString &id, QString *error) {
    char err[256];
    err[0] = '\0';
    sxcl_json *doc = sxcl_json_parse(manifest.constData(), size_t(manifest.size()), err, sizeof(err));
    if (doc == nullptr) {
        if (error != nullptr)
            *error = QStringLiteral("版本清单读不动");
        return QString();
    }
    QString url;
    if (sxcl_version_list *list = sxcl_version_list_build(doc)) {
        if (const sxcl_version_entry *entry = sxcl_version_list_find(list, id.toUtf8().constData()))
            url = QString::fromUtf8(entry->url);
        sxcl_version_list_free(list);
    }
    sxcl_json_free(doc);
    if (url.isEmpty() && error != nullptr)
        *error = QStringLiteral("版本清单里没有 %1").arg(id);
    return url;
}

/** 把一段版本 JSON 落到 versions/<id>/<id>.json。走核心库的写入口:**tmp + 原子改名 +
 *  只认合法版本名** —— 重复点「修复」不会把已经好了的文件弄坏。 */
bool writeVersionJson(const QString &gameDir, const QString &id, const QByteArray &body,
                      QString *error) {
    const QString cacheDir = uiLauncherDataRoot() + QStringLiteral("/cache");
    QDir().mkpath(cacheDir);
    const QString tempPath = cacheDir + QStringLiteral("/repair-%1.json").arg(id);
    QFile temp(tempPath);
    if (!temp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr)
            *error = QStringLiteral("临时文件写不进去");
        return false;
    }
    temp.write(body);
    temp.close();
    char err[256];
    err[0] = '\0';
    const int rc = sxcl_install_write_version_json(gameDir.toUtf8().constData(),
                                                   id.toUtf8().constData(),
                                                   tempPath.toUtf8().constData(), err, sizeof(err));
    QFile::remove(tempPath);
    if (rc != 0) {
        if (error != nullptr)
            *error = QString::fromUtf8(err[0] != '\0' ? err : "版本文件写不进去");
        return false;
    }
    return true;
}

/** 取一个版本的版本文件(清单 -> 那条 url -> 正文 -> 落盘)。 */
bool fetchVersionJsonInto(sxcl_transport *transport, const QString &gameDir, const QString &id,
                          QString *error) {
    QString why;
    const QByteArray manifest = manifestText(transport, &why);
    if (manifest.isEmpty()) {
        if (error != nullptr)
            *error = why;
        return false;
    }
    const QString url = versionJsonUrl(manifest, id, &why);
    if (url.isEmpty()) {
        if (error != nullptr)
            *error = why;
        return false;
    }
    // 镜像做第二路(与下载引擎里的候选路同一个口径)
    char mirror[512];
    mirror[0] = '\0';
    QStringList urls{url};
    if (sxcl_manifest_mirror_url(url.toUtf8().constData(), nullptr, mirror, sizeof(mirror)) == 0)
        urls << QString::fromUtf8(mirror);
    QStringList failures;
    for (const QString &candidate : urls) {
        QString candidateError;
        const QByteArray body = fetchText(transport, candidate, &candidateError);
        if (body.isEmpty()) {
            failures << candidateError;
            continue;
        }
        if (writeVersionJson(gameDir, id, body, &why))
            return true;
        failures << why;
    }
    if (error != nullptr)
        *error = failures.isEmpty() ? QStringLiteral("版本文件拿不回来") : failures.first();
    return false;
}

struct FetchTotals {
    int total = 0;
    int downloaded = 0;
    int skipped = 0;
    int failed = 0;
    qint64 bytes = 0;
};

/** 把一份版本 JSON 展开成清单跑一遍(缺的下来,已有的一个字节都不下)。 */
bool fetchPlanFor(const sxcl_json *doc, const QString &gameDir, const QString &versionId,
                  const sxcl_engine_opts *opts, int preferMirror, int skipFileCheck,
                  std::atomic<bool> *cancel, FetchTotals *totals, QString *error) {
    char err[256];
    err[0] = '\0';
    sxcl_version_plan *plan =
        sxcl_version_plan_build(doc, gameDir.toUtf8().constData(), versionId.toUtf8().constData(),
                                err, sizeof(err));
    if (plan == nullptr) {
        if (error != nullptr)
            *error = QString::fromUtf8(err[0] != '\0' ? err : "列不出这个版本要哪些文件");
        return false;
    }
    if (preferMirror) {
        char mirrorErr[192];
        mirrorErr[0] = '\0';
        sxcl_version_plan_add_mirror(plan, nullptr, mirrorErr, sizeof(mirrorErr)); // 镜像做第二候选
        sxcl_version_plan_prefer_mirror(plan); // 下载源 bmclapi/auto:镜像优先
    }
    if (skipFileCheck)
        sxcl_version_plan_set_skip_existing(plan, 1);

    sxcl_fetch_stats stats;
    std::memset(&stats, 0, sizeof(stats));
    err[0] = '\0';
    const int rc = sxcl_version_plan_fetch(plan, opts,
                                           cancel == nullptr ? nullptr : &cancelRequested, cancel,
                                           &stats, err, sizeof(err));
    totals->total += stats.total;
    totals->downloaded += stats.downloaded;
    totals->skipped += stats.skipped;
    totals->failed += stats.failed;
    totals->bytes += stats.bytes_done;
    sxcl_version_plan_free(plan);
    if (rc != SXCL_FETCH_OK && rc != SXCL_FETCH_PARTIAL) {
        if (error != nullptr)
            *error = QString::fromUtf8(err[0] != '\0' ? err : "补文件没跑成");
        return false;
    }
    return true;
}

/** 读一个实例的版本 JSON(读不出来 = 空指针)。 */
sxcl_json *readInstanceJson(const QString &gameDir, const QString &id) {
    char path[1024];
    path[0] = '\0';
    char err[192];
    err[0] = '\0';
    return sxcl_instance_read_json(gameDir.toUtf8().constData(), id.toUtf8().constData(), path,
                                   sizeof(path), err, sizeof(err));
}

} // namespace

QStringList instanceMissingFacts(const QString &gameDir, const QString &instanceId) {
    InstalledInstance item;
    item.id = instanceId;
    fillInstanceMissingFacts(&item, gameDir);
    return item.missing;
}

bool repairInstance(const QString &gameDir, const QString &instanceId, const QString &settingsFile,
                    std::atomic<bool> *cancel, RepairReport *out, QString *error) {
    if (out != nullptr)
        *out = RepairReport();
    if (error != nullptr)
        error->clear();
    if (out == nullptr) {
        if (error != nullptr)
            *error = QStringLiteral("没有地方放结果");
        return false;
    }
    if (gameDir.isEmpty() || instanceId.isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("游戏目录或版本名为空");
        out->still = QStringList{QStringLiteral("缺版本文件")};
        return false;
    }
    out->still = instanceMissingFacts(gameDir, instanceId);

#if defined(SXCL_REPAIR_HAVE_TRANSPORT)
    sxcl_transport_qt_bootstrap();
    sxcl_transport *transport = sxcl_transport_qt_create();
#else
    sxcl_transport *transport = nullptr;
#endif

    sxcl_engine_opts opts;
    char cacheFile[600];
    cacheFile[0] = '\0';
    int preferMirror = 1;
    int assetsLevel = 1;
    int skipFileCheck = 0;
    const int ready = uiDownloadEngineOpts(settingsFile, &opts, cacheFile, sizeof(cacheFile),
                                           &preferMirror, &assetsLevel, &skipFileCheck);

    bool ok = true;
    QString firstError;

    /* ① 版本文件:不在就按名字从清单里取回来。
     * 取不回来 = 这个版本没法修(清单里都没有它,连"它该长什么样"都不知道)—— 如实报,不假装。 */
    if (readInstanceJson(gameDir, instanceId) == nullptr) {
        QString why;
        if (!fetchVersionJsonInto(transport, gameDir, instanceId, &why)) {
            if (error != nullptr)
                *error = why;
            out->still = instanceMissingFacts(gameDir, instanceId);
            return false;
        }
        out->fetchedVersionJson = true;
    }

    /* ② 展开清单补文件。父版本那一层**单独一张清单**:
     *    加载器实例自己的 JSON 里只有一个壳,原版那些库与客户端 jar 全在父版本里。 */
    if (ready != 0) {
        QStringList versionsToFetch;
        QStringList parentsToFetch;
        if (sxcl_json *doc = readInstanceJson(gameDir, instanceId)) {
            versionsToFetch << instanceId;
            const QString parent =
                QString::fromUtf8(sxcl_json_get_string(sxcl_json_root(doc), "inheritsFrom", ""));
            sxcl_json_free(doc);
            if (!parent.isEmpty()) {
                if (readInstanceJson(gameDir, parent) == nullptr) {
                    // 父版本的版本文件也没有:先取回来(取不回来不算致命,最后照实报"还缺什么")
                    QString why;
                    if (fetchVersionJsonInto(transport, gameDir, parent, &why))
                        out->fetchedExtras << parent;
                    else if (firstError.isEmpty())
                        firstError = why;
                }
                parentsToFetch << parent;
            }
        } else {
            ok = false; // 版本文件刚落地却读不动(内容不是合法 JSON)—— 那不是能修的
            if (firstError.isEmpty())
                firstError = QStringLiteral("版本文件读不动");
        }

        const QStringList all = versionsToFetch + parentsToFetch;
        for (const QString &versionId : all) {
            sxcl_json *doc = readInstanceJson(gameDir, versionId);
            if (doc == nullptr)
                continue;
            FetchTotals totals;
            QString why;
            const bool fetched = fetchPlanFor(doc, gameDir, versionId, &opts, preferMirror,
                                              skipFileCheck, cancel, &totals, &why);
            sxcl_json_free(doc);
            out->filesTotal += totals.total;
            out->filesDownloaded += totals.downloaded;
            out->filesSkipped += totals.skipped;
            out->filesFailed += totals.failed;
            out->bytesDone += totals.bytes;
            if (!fetched) {
                ok = false;
                if (firstError.isEmpty())
                    firstError = why;
            }
        }
    } else {
        ok = false;
        if (firstError.isEmpty())
            firstError = QStringLiteral("这个构建里没有网络后端,补不了文件");
    }

    /* ③ 补完之后**再数一遍**还缺什么 —— 不照抄修复过程自己的账,否则"修没修好"就只能靠它自说自话。 */
    out->still = instanceMissingFacts(gameDir, instanceId);
    if (!out->still.isEmpty() && firstError.isEmpty())
        firstError = QStringLiteral("还有文件没补上");
    if (error != nullptr)
        *error = firstError;
    return ok && out->still.isEmpty();
}

} // namespace sxcl::ui
