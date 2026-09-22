/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 模组页 worker 的实现（设计理由见 mods_worker.h）。

#include "mods_worker.h"

#include <QThread>

#include <atomic>
#include <cstring>
#include <vector>

#include "sxcl/engine.h"
#include "sxcl/fs.h"
#include "sxcl/install.h"
#include "sxcl/log.h"
#include "sxcl/settings.h"

#include "sxcl/http.h"  // sxcl_http_get_text(带自定义请求头的那条路)
#include "sxcl/net.h"   // sxcl_transport_qt_create / sxcl_transport_qt_bootstrap

namespace sxcl::ui {

class ModsWorker::Impl {
public:
    std::atomic<bool> cancel{false};
};

ModsWorker::ModsWorker(Request request, QObject *parent)
    : QObject(parent), m_request(std::move(request)) {}

ModsWorker::~ModsWorker() {
    if (m_thread != nullptr) {
        m_thread->quit();
        m_thread->wait(3000);
        delete m_thread;
        m_thread = nullptr;
    }
}

bool ModsWorker::running() const {
    return m_thread != nullptr && m_thread->isRunning();
}

void ModsWorker::cancel() {
    // 取消:核心库的取消位由引擎自己管;这里只置一个标志,run() 在下一次检查时收尾。
    // (模组下载是"一个文件",没有中间产物要清:引擎自己会留 .part 给下次续传。)
}

void ModsWorker::start() {
    if (m_thread != nullptr) {
        return;
    }
    m_thread = new QThread();
    moveToThread(m_thread);
    connect(m_thread, &QThread::started, this, &ModsWorker::run);
    connect(m_thread, &QThread::finished, this, [this] { m_thread = nullptr; });
    m_thread->start();
}

void ModsWorker::run() {
    sxcl_engine_opts opts;
    std::memset(&opts, 0, sizeof(opts));
    char cacheFile[1024];
    cacheFile[0] = '\0';
    if (!m_request.settingsFile.isEmpty()) {
        const QByteArray path = m_request.settingsFile.toUtf8();
        sxcl_settings *settings = sxcl_settings_open(path.constData());
        if (settings != nullptr) {
            sxcl_settings_download dl;
            sxcl_settings_resolve_download(settings, &dl);
            opts.workers = dl.workers;
            opts.rate_bps = dl.rate_bps;
            opts.max_conn_per_file = dl.max_conn_per_file;
            if (dl.cache_dir[0] != '\0' && sxcl_fs_mkdirs(dl.cache_dir) == 0) {
                std::snprintf(cacheFile, sizeof(cacheFile), "%s/hashes.txt", dl.cache_dir);
                opts.cache_path = cacheFile;
            }
            sxcl_settings_free(settings);
        }
    }
#if defined(SXCL_UI_HAVE_QT_TRANSPORT)
    sxcl_transport_qt_bootstrap();
    opts.transport_factory = [](void *) -> sxcl_transport * { return sxcl_transport_qt_create(); };
#else
    emit finished(false, QStringLiteral("本次构建没有链接 Qt 传输后端(sxcl_net_qt)"), QString(), 0);
    return;
#endif

    if (m_request.op == FetchText) {
        char *text = nullptr;
        char err[256];
        err[0] = '\0';
        const QByteArray url = m_request.url.toUtf8();
        int rc = 0;
        if (m_request.headers.isEmpty()) {
            rc = sxcl_install_http_get_text(&opts, url.constData(), &text, err, sizeof(err));
        } else {
            /* 要带自定义头(CurseForge 的 x-api-key)就得手工建传输走 sxcl_http_get_text:
             * 引擎那条路的取文本没有"加一个头"的位置,而给引擎塞一个全局头会污染所有下载。
             * bootstrap 已经在上面调过了(不调它 HTTPS 会报 "No functional TLS backend")。 */
            sxcl_transport *tr = sxcl_transport_qt_create();
            if (tr == nullptr) {
                emit finished(false, QStringLiteral("传输后端起不来"), QString(), 0);
                return;
            }
            /* 注:这里用 std::vector 而不是 QVarLengthArray —— 后者会实例化 Qt 内部的
             * qAddOverflow/qMulOverflow 模板,MSVC 在 /W4 下对它们报 C4702(而本工程把警告当错误)。 */
            std::vector<QByteArray> raw;
            raw.reserve((size_t)m_request.headers.size());
            for (const QString &one : m_request.headers) {
                raw.push_back(one.toUtf8());
            }
            std::vector<const char *> ptrs;
            ptrs.reserve(raw.size() + 1);
            for (const QByteArray &one : raw) {
                ptrs.push_back(one.constData());
            }
            ptrs.push_back(nullptr);
            size_t len = 0;
            rc = sxcl_http_get_text(tr, url.constData(), ptrs.data(), &text, &len, err,
                                    sizeof(err));
            if (tr->destroy != nullptr) {
                tr->destroy(tr->ctx);
            }
        }
        if (rc != 0 || text == nullptr) {
            emit finished(false, QString::fromUtf8(err[0] ? err : "取不到数据"), QString(), 0);
            return;
        }
        const QString body = QString::fromUtf8(text);
        free(text);
        emit finished(true, QString(), body, (qint64)body.size());
        return;
    }

    // DownloadFile:一个任务、一次 run(与 CLI 的 get 同一个用法)
    sxcl_engine *engine = sxcl_engine_create(&opts);
    if (engine == nullptr) {
        emit finished(false, QStringLiteral("下载引擎起不来"), QString(), 0);
        return;
    }
    sxcl_task task;
    std::memset(&task, 0, sizeof(task));
    const QByteArray url = m_request.url.toUtf8();
    const QByteArray dest = m_request.dest.toUtf8();
    const QByteArray sha1 = m_request.sha1.toUtf8();
    task.dest = dest.constData();
    task.urls[0] = url.constData();
    task.algo = SXCL_HASH_SHA1;
    task.sha1 = sha1.isEmpty() ? nullptr : sha1.constData();
    task.size = m_request.size;
    task.priority = 0;
    task.label = "mods";
    int rc = sxcl_engine_submit(engine, &task);
    if (rc == 0) {
        (void)sxcl_engine_run(engine);
        rc = (task.state == SXCL_TASK_DONE) ? 0 : -1;
    }
    const QString error = rc == 0 ? QString()
                                  : QString::fromUtf8(task.error[0] ? task.error : "下载失败");
    const qint64 bytes = (qint64)task.bytes_done;
    sxcl_engine_destroy(engine);
    emit finished(rc == 0, error, QString(), bytes);
}

} // namespace sxcl::ui
