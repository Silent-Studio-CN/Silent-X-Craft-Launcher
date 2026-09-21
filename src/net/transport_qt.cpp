/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "sxcl/net.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <QUrl>

#include <cstring>

namespace {

constexpr int kDefaultTimeoutMs = 30000;

struct QtBody {
    QNetworkReply *reply = nullptr;
    QByteArray pending;   // 已到达但还没被 read() 取走的字节
    bool finished = false;
    bool aborted = false;
    bool ioError = false;
};

struct QtTransport {
    sxcl_transport pub{};
    QNetworkAccessManager *nam = nullptr;
    QSet<QtBody *> bodies;
    bool cancelled = false;
};

QtTransport *asTransport(void *ctx) { return static_cast<QtTransport *>(ctx); }
QtBody *asBody(sxcl_http_body *body) { return reinterpret_cast<QtBody *>(body); }

/* 把异步回复转成阻塞等待。两条规矩都是实测踩出来的:
 *  1) **进入等待前先查状态**:信号是"一次性"的,若回复在我们 connect 之前就结束了,
 *     那些信号永远不会再来,光 connect 就得白等整个超时。症状:每个 10KB 资源文件卡满
 *     30 秒(相邻落盘间隔中位数 30116ms),5147 个文件要 40 小时。
 *  2) 等待期间放 50ms 心跳兜底,真漏了信号也不会卡死。 */
/* 泵一次事件循环:某个信号到了、或有数据可读、或最多等 maxWaitMs 毫秒就返回。
 *
 * 语义刻意做成"最多等一小会儿",由调用方循环 + 自己判超时:
 * 把超时判断放在这里、让调用方只看一次状态,是错的 —— 心跳一唤醒调用方就以为"没响应",
 * 实测导致 30/30 全部报"拿不到响应"(而系统 curl 同一个 URL 是 200)。 */
void pumpReply(QNetworkReply *reply, int maxWaitMs) {
    QEventLoop loop;
    QTimer wake;
    wake.setSingleShot(true);
    QObject::connect(&wake, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::errorOccurred, &loop, &QEventLoop::quit);
    wake.start(maxWaitMs);
    loop.exec();
}

/* 把回复里已经到达的字节收进 pending,并在结束时置上 finished/ioError */
void harvestReply(QtBody *b) {
    b->pending += b->reply->readAll();
    if (!b->reply->isFinished()) {
        return;
    }
    b->finished = true;
    const QNetworkReply::NetworkError err = b->reply->error();
    if (err != QNetworkReply::NoError && err != QNetworkReply::OperationCanceledError) {
        /* 关键区分:Qt 把 4xx/5xx 也报成 error()(ContentNotFoundError /
         * ProtocolInvalidOperationError / InternalServerError…),但那**不是**传输故障 ——
         * 响应头与正文都好好的。把它们当 IO 错误会毁掉两件事:
         *   - 登录链:OAuth 的 authorization_pending / slow_down / expired_token
         *     全写在 400 的 JSON 正文里,读不到正文就没法轮询(实测踩过);
         *   - 下载引擎:404 的正文(错误页/换源提示)同样读不到。
         * 只有"连 HTTP 状态码都没有"才算真正的传输层故障。 */
        const QVariant status = b->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (!status.isValid() || status.toInt() == 0) {
            b->ioError = true;
        }
    }
    b->pending += b->reply->readAll();
}

/* 从 Content-Range: bytes a-b/total 里取 a/b/total;解析不出返回 false */
bool parseContentRange(const QByteArray &value, int64_t *start, int64_t *end, int64_t *total) {
    const QByteArray v = value.trimmed();
    // Qt6 去掉了 startsWith 的大小写重载,自己比
    if (v.size() < 6 || v.left(6).toLower() != QByteArrayLiteral("bytes "))
        return false;
    const QList<QByteArray> parts = v.mid(6).split('/');
    if (parts.size() != 2)
        return false;
    const QList<QByteArray> range = parts.at(0).split('-');
    if (range.size() != 2)
        return false;
    bool ok1 = false, ok2 = false;
    const qint64 a = range.at(0).toLongLong(&ok1);
    const qint64 b = range.at(1).toLongLong(&ok2);
    if (!ok1 || !ok2)
        return false;
    *start = a;
    *end = b;
    *total = (parts.at(1) == "*") ? -1 : parts.at(1).toLongLong();
    return true;
}

int qtRequest(void *ctx, const sxcl_http_request *req, sxcl_http_response *resp, sxcl_http_body **body) {
    QtTransport *t = asTransport(ctx);
    if (!t || !req || !req->url || !resp || !body)
        return SXCL_NET_ERR_BAD_ARG;
    *body = nullptr;
    std::memset(resp, 0, sizeof(*resp));
    resp->content_length = -1;
    resp->total_length = -1;
    resp->range_start = -1;
    resp->range_end = -1;
    if (t->cancelled)
        return SXCL_NET_ERR_CANCELLED;

    const QUrl url(QString::fromUtf8(req->url));
    if (!url.isValid() || url.scheme().isEmpty())
        return SXCL_NET_ERR_BAD_ARG;
    // 只允许 http/https:版本元数据来自网络,不能让 file:// 之类混进来
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) {
        return SXCL_NET_ERR_UNSUPPORTED;
    }

    QNetworkRequest qreq(url);
    qreq.setAttribute(QNetworkRequest::Http2AllowedAttribute, req->force_http1 ? false : true);
    qreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    qreq.setTransferTimeout(req->timeout_ms > 0 ? int(req->timeout_ms) : kDefaultTimeoutMs);
    if (req->extra_headers) {
        for (const char *const *h = req->extra_headers; *h; ++h) {
            const QByteArray line(*h);
            const int colon = line.indexOf(':');
            if (colon > 0) {
                qreq.setRawHeader(line.left(colon).trimmed(), line.mid(colon + 1).trimmed());
            }
        }
    }
    if (req->range_start >= 0) {
        QByteArray range = "bytes=" + QByteArray::number(qlonglong(req->range_start)) + "-";
        if (req->range_end >= req->range_start)
            range += QByteArray::number(qlonglong(req->range_end));
        qreq.setRawHeader("Range", range);
        // 关键:Range 请求必须禁用压缩。
        // 实测 Mojang CDN(Fastly/Azure):带 Accept-Encoding: gzip 时它会放弃 Range,
        // 返回 200 + 全量压缩正文(curl 带 gzip 拿到 200/42376 字节,不带拿到 206/256 字节)。
        // 不写这一行,续传与多连接分片会静默退化成"每次全量重下"——功能看着正常,带宽全浪费。
        qreq.setRawHeader("Accept-Encoding", QByteArrayLiteral("identity"));
    }

    const QByteArray method = (req->method && *req->method) ? QByteArray(req->method) : QByteArray("GET");
    /* 请求体:正版登录要 POST JSON/表单。req->body 非空才算带体(GET 请求不允许有体)。
     * Qt 的 sendCustomRequest 会自己补 Content-Length;Content-Type 由调用方放进 extra_headers。 */
    QByteArray bodyBytes;
    if (req->body != nullptr && req->body_len > 0)
        bodyBytes = QByteArray(reinterpret_cast<const char *>(req->body), int(req->body_len));
    QNetworkReply *reply = nullptr;
    if (method == "HEAD")
        reply = t->nam->head(qreq);
    else if (req->body != nullptr)
        reply = t->nam->sendCustomRequest(qreq, method, bodyBytes);
    else
        reply = t->nam->get(qreq);

    // 循环等"有响应头":每轮最多泵 50ms,超时由这里判(不让心跳误判成没响应)
    QElapsedTimer clock;
    clock.start();
    const int deadlineMs = req->timeout_ms > 0 ? int(req->timeout_ms) + 1000 : kDefaultTimeoutMs + 1000;
    int status = 0;
    for (;;) {
        status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        /* 3xx 不是最终状态:Qt 正按 RedirectPolicy 继续跟。
         * 实测 BMCLAPI(镜像站)每个文件都先 302 到预签名 URL、再 200(共 2 跳);
         * 以前这里"一看到状态码就跳出",拿到的是第一跳的 302,于是镜像那条路**永远算失败**、
         * 每次回落到官方 —— 镜像排第一等于白排。
         * 只有"重定向且 Qt 还在跟"时才继续等;真跟不下去(reply 已结束)照样退出。 */
        const bool redirecting = (status == 301 || status == 302 || status == 303 || status == 307 ||
                                  status == 308) &&
                                 !reply->isFinished();
        if ((status != 0 && !redirecting) || reply->isFinished() || clock.elapsed() >= deadlineMs) {
            break;
        }
        pumpReply(reply, 50);
    }
    if (status == 0) {
        // 连响应头都没拿到:DNS/连接/TLS/超时,或用户取消。把 Qt 的错误码打出来便于定位
        qWarning("QT 无响应: %s  error=%d (%s) elapsed=%lldms", req->url, int(reply->error()),
                 qPrintable(reply->errorString()), static_cast<long long>(clock.elapsed()));
        reply->abort();
        reply->deleteLater();
        return t->cancelled ? SXCL_NET_ERR_CANCELLED : SXCL_NET_ERR_CONNECT;
    }

    if (!qEnvironmentVariableIsEmpty("SXCL_NET_DEBUG")) {
        qInfo("QT %s (h2=%d range=%lld) -> %d", req->url, req->force_http1 ? 0 : 1,
              static_cast<long long>(req->range_start), status);
        const QList<QPair<QByteArray, QByteArray>> pairs = reply->rawHeaderPairs();
        for (const QPair<QByteArray, QByteArray> &p : pairs) {
            qInfo("   %s: %s", p.first.constData(), p.second.constData());
        }
    }

    /* 响应头回调:必须在返回前发完(头这时已经全到了)。name/value 都是 QByteArray 的
     * NUL 结尾缓冲,直接当 C 串用;回调里存指针是错的(函数返回后就没了)。 */
    if (req->on_header != nullptr) {
        const QList<QPair<QByteArray, QByteArray>> pairs = reply->rawHeaderPairs();
        for (const QPair<QByteArray, QByteArray> &p : pairs) {
            req->on_header(req->header_userdata, p.first.constData(), p.second.constData());
        }
    }

    resp->status = status;
    resp->is_range_response = (status == 206) ? 1 : 0;
    resp->content_length = reply->header(QNetworkRequest::ContentLengthHeader).isValid()
                               ? reply->header(QNetworkRequest::ContentLengthHeader).toLongLong()
                               : -1;
    const QByteArray acceptRanges = reply->rawHeader("Accept-Ranges").trimmed().toLower();
    resp->accept_ranges = (acceptRanges == QByteArrayLiteral("bytes")) ? 1 : 0;
    int64_t rs = -1, re = -1, total = -1;
    if (parseContentRange(reply->rawHeader("Content-Range"), &rs, &re, &total)) {
        resp->range_start = rs;
        resp->range_end = re;
        resp->total_length = total;
    }

    QtBody *b = new QtBody();
    b->reply = reply;
    b->pending = reply->readAll(); // 头到达时往往已经带了第一批正文
    t->bodies.insert(b);
    *body = reinterpret_cast<sxcl_http_body *>(b);
    return SXCL_NET_OK;
}

int64_t qtRead(void *ctx, sxcl_http_body *body, void *buf, size_t len) {
    QtTransport *t = asTransport(ctx);
    QtBody *b = asBody(body);
    if (!t || !b || !buf)
        return SXCL_NET_ERR_BAD_ARG;
    if (b->aborted || t->cancelled)
        return SXCL_NET_ERR_CANCELLED;
    if (len == 0)
        return 0;

    for (;;) {
        if (!b->pending.isEmpty()) {
            const int take = int(qMin<qint64>(qint64(len), qint64(b->pending.size())));
            std::memcpy(buf, b->pending.constData(), size_t(take));
            b->pending.remove(0, take);
            return take;
        }
        if (b->finished)
            return 0;
        if (b->ioError)
            return SXCL_NET_ERR_IO;
        /* 先把回复的当前状态同步过来:信号是一次性的,已经结束的回复不会再发信号 */
        harvestReply(b);
        if (b->ioError)
            return SXCL_NET_ERR_IO;
        if (!b->pending.isEmpty())
            continue;
        if (b->finished)
            return 0;
        if (t->cancelled)
            return SXCL_NET_ERR_CANCELLED;
        pumpReply(b->reply, 50); /* 每轮最多 50ms,由上面的状态判断决定是否继续 */
    }
}

void qtCloseBody(void *ctx, sxcl_http_body *body) {
    QtTransport *t = asTransport(ctx);
    QtBody *b = asBody(body);
    if (!t || !b)
        return;
    t->bodies.remove(b);
    if (b->reply) {
        if (!b->reply->isFinished())
            b->reply->abort();
        b->reply->deleteLater();
    }
    delete b;
}

void qtCancelAll(void *ctx) {
    QtTransport *t = asTransport(ctx);
    if (!t)
        return;
    t->cancelled = true;
    const QSet<QtBody *> snapshot = t->bodies; // abort 会触发信号,复制一份再遍历
    for (QtBody *b : snapshot) {
        if (b->reply && !b->reply->isFinished())
            b->reply->abort();
    }
}

void qtDestroy(void *ctx) {
    QtTransport *t = asTransport(ctx);
    if (!t)
        return;
    qtCancelAll(t);
    const QSet<QtBody *> snapshot = t->bodies;
    for (QtBody *b : snapshot)
        qtCloseBody(t, reinterpret_cast<sxcl_http_body *>(b));
    if (t->nam) {
        // 先断掉所有 keep-alive 连接再销毁:否则 Qt 内部线程还在等,
        // 退出时会打印 "QWaitCondition: Destroyed while threads are still waiting"
        t->nam->clearAccessCache();
        delete t->nam;
        t->nam = nullptr;
    }
    delete t;
}

} // namespace

extern "C" void sxcl_transport_qt_bootstrap(void) {
    if (QCoreApplication::instance() != nullptr) {
        return;
    }
    // 故意不释放:进程级单例,与 Qt 的常规用法一致
    static int argc = 1;
    static char name[] = "sxcl-dl";
    static char *argv[] = {name, nullptr};
    new QCoreApplication(argc, argv);
}

extern "C" sxcl_transport *sxcl_transport_qt_create(void) {
    QtTransport *t = new QtTransport();
    t->nam = new QNetworkAccessManager();
    t->pub.ctx = t;
    t->pub.request = &qtRequest;
    t->pub.read = &qtRead;
    t->pub.close_body = &qtCloseBody;
    t->pub.cancel_all = &qtCancelAll;
    t->pub.destroy = &qtDestroy;
    return &t->pub;
}
