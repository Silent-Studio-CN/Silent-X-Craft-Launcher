/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "sxcl/net.h"

#include "sxcl/log.h" // 每个请求一行(debug 级):方法/打码 URL/状态码/字节数/耗时

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQueue>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QWaitCondition>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace {

constexpr int kDefaultTimeoutMs = 30000;

struct NetJob;

struct QtBody {
    QNetworkReply *reply = nullptr;         // 直连路径:回复在本线程
    QSharedPointer<NetJob> job;             // 池路径:字节从共享网络线程灌过来
    QByteArray pending;   // 已到达但还没被 read() 取走的字节
    bool finished = false;
    bool aborted = false;
    bool ioError = false;
    /* 结果复用:读干了才写缓存(半途放弃的正文不缓存) */
    bool cacheWanted = false;
    bool cacheDone = true;
    int cacheClass = 0;
    int cacheStatus = 0;
    QByteArray cacheUrl;
    QByteArray cacheBuf;
    QByteArray cacheContentType;
    QByteArray cacheControl;
    /* 停滞看门狗:连接还在、但没有新字节到达多久算"死了"。
     * 不设这条会**永久卡住** —— 实测 Quilt 官方 maven 下到 1.19MB 后彻底停摆,
     * 进程 CPU 0.09s、9 分钟零进展,界面就那样一直显示"正在下载"(见 docs/15 §7)。 */
    QElapsedTimer lastData;
    int stallMs = 60000;
    QByteArray maskedUrl; // 只给日志用(URL 必须打码)
    std::shared_ptr<struct TraceAcc> acc; // 取证样本(见下):读正文时累加字节
};

struct QtTransport {
    sxcl_transport pub{};
    QNetworkAccessManager *nam = nullptr;
    QSet<QtBody *> bodies;
    bool cancelled = false;
    bool ownNam = false; /* 1 = 这个实例自己持有的实例(旧行为/回退);0 = 按线程共享的那一个 */
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
    /* 先问一句"设备还开着吗":回复底下就是那条 QSslSocket,对端把空闲连接关掉之后,Qt 的回复
     * **既不发 finished 也不置 error**,而 readAll() 会一路读到**已经关掉的 socket** 上 ——
     * 每读一次 Qt 就回一行 "QIODevice::read (QSslSocket): device not open" 并返回空,
     * 我们的循环就空转、stderr 刷屏(现场日志里重复出现的那条;登录窗开着退出的现场也复现到 2 行)。
     * 只在"回复还没结束 + 设备已经关了"这一种组合下提前收尾:**回复真结束了的正常收尾仍然走
     * 下面这条路,把最后一批字节读干净 —— 不跳过任何数据**。
     * 出口与 60 秒停滞看门狗同一个:报 IO 错误 = 这次尝试作废,引擎保留已下部分、
     * 换路或原路续传,不丢进度。 */
    if (!b->reply->isFinished() && !b->reply->isOpen()) {
        sxcl_log_write(SXCL_LOG_WARN, "net", "%s -> 连接已关闭(读不出数据),放弃本次尝试",
                       b->maskedUrl.constData());
        b->ioError = true;
        b->finished = true;
        return;
    }
    const int before = b->pending.size();
    b->pending += b->reply->readAll();
    if (b->pending.size() != before) {
        b->lastData.restart(); // 有字节到达 = 这条连接还活着
    }
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
    if (b->pending.size() != before) {
        b->lastData.restart();
    }
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

/* ══════════════════════════════════════════════════════════════════════════════
 * 取证:SXCL_NET_TRACE=1 时每个请求留一份样本,行给人看、结构体给基准工具
 * (tools/net_bench.ps1 -> src/net/net_bench_main.cpp)。测不到就是 -1,不编数。
 * ══════════════════════════════════════════════════════════════════════════════ */

bool netFlagOn(const char *name, bool fallback) {
    const QByteArray v = qgetenv(name).trimmed().toLower();
    if (v.isEmpty())
        return fallback;
    return !(v == "0" || v == "off" || v == "false" || v == "no");
}

int netIntEnv(const char *name, int fallback) {
    bool ok = false;
    const int v = qgetenv(name).trimmed().toInt(&ok);
    return (ok && v > 0) ? v : fallback;
}

bool traceOn() {
    static const bool on = netFlagOn("SXCL_NET_TRACE", false);
    return on;
}

/* 一个请求的相位计时。信号是异步来的(TLS 握手可能晚于发起、正文晚于响应头),
 * 所以用 shared_ptr 挂在回复上,别用栈上的计时器。 */
struct TraceAcc {
    QByteArray url;
    QByteArray method;
    QElapsedTimer clock;
    qint64 conn_ms = -1;   /* 发起 -> TLS 握手完成 */
    qint64 ttfb_ms = -1;   /* 发起 -> 响应头/首字节 */
    qint64 dns_ms = -1;    /* 本进程对这台主机的解析耗时 */
    qint64 wire_bytes = -1;
    qint64 body_bytes = 0;
    qint64 total_ms = -1;
    int status = 0;
    int http2 = 0;
    int reused = -1;
    int from_cache = 0;
    int attempts = 1;
    QByteArray encoding;
    bool headers_seen = false;
    bool finalized = false;
};

struct TraceSlot {
    sxcl_net_sample sample{};
    int has = 0;
};
TraceSlot &traceSlot() {
    static thread_local TraceSlot slot;
    return slot;
}

struct NetCounters {
    std::atomic<long long> requests{0};
    std::atomic<long long> cache_mem{0};
    std::atomic<long long> cache_disk{0};
    std::atomic<long long> cache_stores{0};
    std::atomic<long long> prewarm_ok{0};
    std::atomic<long long> prewarm_fail{0};
    std::atomic<long long> retries{0};
    std::atomic<long long> throttled{0};
    std::atomic<long long> wire{0};
    std::atomic<long long> body{0};
};
NetCounters &counters() {
    static NetCounters c;
    return c;
}

void tracePrint(const sxcl_net_sample &s) {
    char masked[SXCL_LOG_URL_MAX + 48];
    (void)sxcl_log_mask_url(s.url[0] ? s.url : "(空)", masked, sizeof(masked));
    qInfo("NETTRACE %s status=%d proto=%s reused=%s dns=%lldms conn=%lldms ttfb=%lldms "
          "total=%lldms bytes=%lld/%lld enc=%s src=%d tries=%d",
          masked, s.status, s.http2 ? "h2" : "h1",
          s.reused == 1 ? "yes" : (s.reused == 0 ? "no" : "?"), (long long)s.dns_ms,
          (long long)s.conn_ms, (long long)s.ttfb_ms, (long long)s.total_ms,
          (long long)s.wire_bytes, (long long)s.body_bytes,
          s.content_encoding[0] ? s.content_encoding : "-", s.from_cache, s.attempts);
}

/* 定稿:写本线程样本(给基准工具)+ 累计统计 + 需要时打一行。 */
void traceFinish(const std::shared_ptr<TraceAcc> &acc) {
    if (!acc || acc->finalized)
        return;
    acc->finalized = true;
    sxcl_net_sample s{};
    {
        const QByteArray u = acc->url.left(SXCL_NET_URL_MAX - 1);
        std::memcpy(s.url, u.constData(), size_t(u.size()));
    }
    s.status = acc->status;
    s.http2 = acc->http2;
    s.reused = acc->reused;
    s.from_cache = acc->from_cache;
    s.attempts = acc->attempts;
    s.dns_ms = acc->dns_ms;
    s.conn_ms = acc->conn_ms;
    s.ttfb_ms = acc->ttfb_ms;
    s.total_ms = (acc->total_ms >= 0) ? acc->total_ms : acc->clock.elapsed();
    s.wire_bytes = acc->wire_bytes;
    s.body_bytes = acc->body_bytes;
    const QByteArray enc = acc->encoding.left(SXCL_NET_ENCODING_MAX - 1);
    std::memcpy(s.content_encoding, enc.constData(), size_t(enc.size()));
    {
        TraceSlot &slot = traceSlot();
        slot.sample = s;
        slot.has = 1;
    }
    NetCounters &c = counters();
    if (acc->from_cache == 0) {
        c.requests.fetch_add(1);
        if (acc->wire_bytes > 0)
            c.wire.fetch_add(acc->wire_bytes);
    } else if (acc->from_cache == 1) {
        c.cache_mem.fetch_add(1);
    } else {
        c.cache_disk.fetch_add(1);
    }
    c.body.fetch_add(acc->body_bytes);
    if (traceOn())
        tracePrint(s);
}

/* ── 运行时开关(默认值 = 最快的那个;环境变量只为取证/对照/回退) ── */
struct NetKnobs {
    bool legacy = false;      /* SXCL_NET_LEGACY=1:退回旧行为(每次请求一个新实例、不缓存、不预热) */
    bool cache = true;        /* SXCL_NET_CACHE=0 关 */
    bool prewarm = true;      /* SXCL_NET_PREWARM=0 关 */
    bool pool = true;         /* SXCL_NET_POOL=0 关(退回"按线程一个实例") */
    int connectMs = 8000;     /* 响应头阶段超时:到点就换下一个源,不干等 30 秒 */
    int queryTtlMs = 60000;   /* 查询结果缓存存活 */
    qint64 iconTtlMs = 2592000000LL; /* 图标缓存存活(30 天:同一张图按 URL 不会变) */
    int entryMaxBytes = 12 * 1024 * 1024;   /* 单条缓存上限 */
    int memBudgetBytes = 32 * 1024 * 1024;  /* 内存缓存总预算 */
    int retryMax = 1;         /* 最多重试几次(共 retryMax+1 次尝试) */
    int retryBackoffMs = 200; /* 首次重试退避 */
    int retryBudgetMs = 6000; /* 重试总预算:花完就不再试 */
    int poolThreads = 4;      /* 共享网络线程数(按 host 粘住) */
    int pendingCap = 512 * 1024; /* 单请求在途缓冲:消费者不读就不再从 socket 收(背压) */
    int maxPerHost = 6;       /* 同一 host 同时最多几条请求(和 Qt 自己的连接上限对齐) */
};

const NetKnobs &knobs() {
    static const NetKnobs k = [] {
        NetKnobs n;
        const bool legacy = netFlagOn("SXCL_NET_LEGACY", false);
        n.cache = netFlagOn("SXCL_NET_CACHE", true);
        n.prewarm = netFlagOn("SXCL_NET_PREWARM", true);
        n.pool = netFlagOn("SXCL_NET_POOL", true);
        n.connectMs = netIntEnv("SXCL_NET_CONNECT_MS", 8000);
        n.queryTtlMs = netIntEnv("SXCL_NET_QUERY_TTL_MS", 60000);
        n.retryMax = netIntEnv("SXCL_NET_RETRY", 1);
        n.poolThreads = netIntEnv("SXCL_NET_POOL_THREADS", 4);
        n.maxPerHost = netIntEnv("SXCL_NET_MAX_PER_HOST", 6);
        if (n.poolThreads > 8)
            n.poolThreads = 8;
        if (legacy) { /* 旧行为:一个都不开 */
            n.legacy = true;
            n.cache = false;
            n.prewarm = false;
            n.pool = false;
            n.retryMax = 0;
        }
        return n;
    }();
    return k;
}

/* 严格按 http/https 判定;其它 scheme 由调用方挡掉 */
bool isHttps(const QUrl &url) { return url.scheme() == QLatin1String("https"); }
int portOf(const QUrl &url) {
    const int p = url.port();
    if (p > 0)
        return p;
    return isHttps(url) ? 443 : 80;
}

/* ── 按线程共享一个 QNetworkAccessManager ──
 * 实测(2026-09-27 基线):模组页每次请求都新建一个实例(搜索/每个图标各一个),
 * 于是同一台主机上 25 次请求 = 25 次 TCP+TLS 握手(conn 400~800ms,reused 0/25)。
 * 这里把实例按线程钉住:线程里所有传输(引擎的每个工作线程、池里的每个网络线程)
 * 共用一份连接缓存,keep-alive 与 HTTP/2 多路复用才谈得上"复用"。 */
struct NamRef {
    QNetworkAccessManager *nam = nullptr;
    int refs = 0;
};

void configureNam(QNetworkAccessManager *nam) {
    nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
    nam->setAutoDeleteReplies(false); /* 我们自己 deleteLater:回复对象要读到读完为止 */
    /* 兜底超时(单请求超时优先):Qt 6 的 transferTimeout 是"没有新字节"的空闲超时,
     * 所以大文件只要在流动就不会被它掐掉。 */
    nam->setTransferTimeout(std::chrono::milliseconds(kDefaultTimeoutMs));
}

struct TlNam {
    NamRef *ref = nullptr;
    ~TlNam() {
        if (ref == nullptr)
            return;
        if (ref->nam != nullptr && QCoreApplication::instance() != nullptr) {
            ref->nam->clearAccessCache();
            delete ref->nam;
        }
        delete ref;
    }
};
TlNam &tlNam() {
    static thread_local TlNam holder;
    return holder;
}

QNetworkAccessManager *sharedNam() {
    TlNam &h = tlNam();
    if (h.ref == nullptr) {
        h.ref = new NamRef();
        h.ref->nam = new QNetworkAccessManager();
        configureNam(h.ref->nam);
    }
    h.ref->refs++;
    return h.ref->nam;
}

void releaseSharedNam() {
    TlNam &h = tlNam();
    if (h.ref != nullptr && h.ref->refs > 0)
        h.ref->refs--;
}

/* ── 预热:先把主机解析掉,再让网络实例把连接(TCP+TLS)建起来 ──
 * "模组页一打开就先握手,别等用户点搜索"——连接是池里那条线程建的,
 * 之后的请求按 host 粘到同一条线程上,于是握手钱只付一次。 */
QMutex g_hostMutex;
QHash<QString, qint64> g_hostDnsMs;      /* host -> 本进程解析耗时(测到的那一次) */
QHash<QString, qint64> g_hostWarmAtMs;   /* host -> 最近一次预热时间 */

int recordDns(const QString &host) {
    QElapsedTimer timer;
    timer.start();
    const QHostInfo info = QHostInfo::fromName(host); /* 阻塞:预热本来就在后台线程 */
    const qint64 ms = timer.elapsed();
    const bool ok = (info.error() == QHostInfo::NoError);
    {
        QMutexLocker lock(&g_hostMutex);
        g_hostDnsMs.insert(host, ok ? ms : -1);
    }
    return ok ? int(ms) : -1;
}

qint64 dnsFor(const QString &host) {
    QMutexLocker lock(&g_hostMutex);
    const auto it = g_hostDnsMs.constFind(host);
    return (it == g_hostDnsMs.constEnd()) ? -1 : it.value();
}

bool hostIsWarm(const QString &host, qint64 freshMs) {
    QMutexLocker lock(&g_hostMutex);
    const auto it = g_hostWarmAtMs.constFind(host);
    if (it == g_hostWarmAtMs.constEnd())
        return false;
    return (QDateTime::currentMSecsSinceEpoch() - it.value()) < freshMs;
}

void markHostWarm(const QString &host) {
    QMutexLocker lock(&g_hostMutex);
    g_hostWarmAtMs.insert(host, QDateTime::currentMSecsSinceEpoch());
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 请求分类 / 缓存 / 共享网络线程池
 *
 * 结构问题(2026-09-27 基线实测):模组页每次请求都 new 一个 QThread + 一个传输 +
 * 一个 QNetworkAccessManager。同一台主机上的请求因此永远落在不同线程上,
 * 连接(TCP+TLS)一次也复用不上(基线:25/25 次请求都做了握手,conn 400~800ms)。
 *
 * 这里把"要快的小请求"(搜索/图标/清单:JSON、带 query、图片)交给一个**共享网络线程池**:
 *   * 池里每条线程长命,自己一个 QNetworkAccessManager;
 *   * 请求按 host **粘**在固定一条线程上 —— 连接就留住了,HTTP/2 还能多路复用;
 *   * 消费者线程只等条件变量,不跑嵌套事件循环,不占界面线程;
 *   * 正文用"高水位/低水位"背压:消费者不读,池线程就不再从 socket 收。
 *
 * 大文件下载(带 Range 的分片、.jar/.zip 这类批量传输)一律走原来的直连路径,
 * 一个字节的行为都不改 —— 提速不许拿下载的稳定性冒险。
 * ══════════════════════════════════════════════════════════════════════════════ */

constexpr int kPendingHighWater = 512 * 1024; /* worker 往缓冲里灌到这么多就停手(背压) */
constexpr int kPendingLowWater = 128 * 1024;  /* 消费者降到这么少就叫 worker 接着灌 */
constexpr int kReplyBufBytes = 256 * 1024;    /* Qt 侧读缓冲上限 */
constexpr qint64 kPoolBufferHardCap = 48LL * 1024 * 1024; /* 分类失手时的兜底,绝不无限吃内存 */

enum ReqClass { ReqBulk = 0, ReqQuery = 1, ReqIcon = 2 };

ReqClass classifyUrl(const QUrl &url) {
    const QString path = url.path().toLower();
    const char *const exts[] = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".svg", ".ico"};
    for (const char *ext : exts) {
        if (path.endsWith(QLatin1String(ext)))
            return ReqIcon;
    }
    const char *const textExts[] = {".json", ".json5", ".xml", ".txt"};
    for (const char *ext : textExts) {
        if (path.endsWith(QLatin1String(ext)))
            return ReqQuery;
    }
    if (!url.query().isEmpty())
        return ReqQuery;
    return ReqBulk;
}

/* 带凭据的请求不进缓存、也不重试:登录链的响应是**跟人走**的,复用了就是事故。 */
bool requestIsPrivate(const sxcl_http_request *req) {
    if (req->extra_headers != nullptr) {
        for (const char *const *h = req->extra_headers; *h != nullptr; ++h) {
            const QByteArray line(*h);
            const int colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray name = line.left(colon).trimmed().toLower();
            if (name == "authorization" || name == "cookie" || name == "proxy-authorization" ||
                name == "set-cookie")
                return true;
        }
    }
    const QString url = QString::fromUtf8(req->url).toLower();
    const char *const marks[] = {"token", "oauth", "login", "session", "account", "/auth",
                                 "profile", "entitlement", "sessionticket"};
    for (const char *mark : marks) {
        if (url.contains(QLatin1String(mark)))
            return true;
    }
    return false;
}

/* 缓存与"走不走池"共用同一条判定:GET、没有 Range、没有请求体、不带凭据。 */
bool requestIsShareable(const sxcl_http_request *req, const QUrl &url) {
    const QByteArray method = (req->method && *req->method) ? QByteArray(req->method) : QByteArray("GET");
    if (method != "GET" && method != "HEAD")
        return false;
    if (req->range_start >= 0 || req->body != nullptr)
        return false;
    if (requestIsPrivate(req))
        return false;
    if (url.host().isEmpty())
        return false;
    return true;
}

/* ── 结果缓存:查询 60 秒内存;图标内存 + 磁盘(用户缓存目录)两级 ── */
struct CacheEntry {
    QByteArray body;
    int status = 200;
    qint64 content_length = -1;
    qint64 wire_bytes = -1;
    qint64 expires_at_ms = 0;
    qint64 stored_at_ms = 0;
    int from_disk = 0;
};

QMutex g_cacheMutex;
QHash<QByteArray, CacheEntry> g_cache;
qint64 g_cacheBytes = 0;
QByteArray g_cacheDirOverride;

QByteArray urlKey(const QUrl &url) {
    return url.toEncoded(QUrl::RemoveUserInfo | QUrl::RemoveFragment);
}

QString iconCacheDir() {
    QString dir;
    {
        QMutexLocker lock(&g_cacheMutex);
        dir = QString::fromUtf8(g_cacheDirOverride);
    }
    if (dir.isEmpty())
        dir = qEnvironmentVariable("SXCL_NET_CACHE_DIR").trimmed();
    if (dir.isEmpty()) {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        dir = base.isEmpty() ? (QDir::tempPath() + QLatin1String("/sxcl-net-cache"))
                             : (base + QLatin1String("/net"));
    }
    dir += QLatin1String("/icons");
    (void)QDir().mkpath(dir);
    return dir;
}

QString cacheFileName(const QByteArray &key) {
    return iconCacheDir() + QLatin1Char('/') +
           QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex()) +
           QLatin1String(".bin");
}

void memStore(const QByteArray &key, const CacheEntry &entry) {
    QMutexLocker lock(&g_cacheMutex);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        g_cacheBytes -= it->body.size();
        it.value() = entry;
    } else {
        g_cache.insert(key, entry);
    }
    g_cacheBytes += entry.body.size();
    while (g_cacheBytes > knobs().memBudgetBytes && !g_cache.isEmpty()) {
        auto oldest = g_cache.begin();
        for (auto i = g_cache.begin(); i != g_cache.end(); ++i) {
            if (i->stored_at_ms < oldest->stored_at_ms)
                oldest = i;
        }
        g_cacheBytes -= oldest->body.size();
        g_cache.erase(oldest);
    }
}

bool memLookup(const QByteArray &key, CacheEntry *out) {
    QMutexLocker lock(&g_cacheMutex);
    auto it = g_cache.find(key);
    if (it == g_cache.end())
        return false;
    if (it->expires_at_ms <= QDateTime::currentMSecsSinceEpoch()) {
        g_cacheBytes -= it->body.size();
        g_cache.erase(it);
        return false;
    }
    *out = it.value();
    return true;
}

bool diskLookup(const QByteArray &key, CacheEntry *out) {
    const QString path = cacheFileName(key);
    QFileInfo info(path);
    if (!info.exists() || info.size() <= 0 || info.size() > knobs().entryMaxBytes)
        return false;
    if (info.lastModified().toMSecsSinceEpoch() + knobs().iconTtlMs < QDateTime::currentMSecsSinceEpoch())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray body = file.readAll();
    file.close();
    if (body.isEmpty())
        return false;
    out->body = body;
    out->status = 200;
    out->content_length = body.size();
    out->from_disk = 1;
    return true;
}

void diskStore(const QByteArray &key, const CacheEntry &entry) {
    QFile file(cacheFileName(key));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    (void)file.write(entry.body);
    file.close();
}

bool cacheLookup(const QUrl &url, CacheEntry *out) {
    const QByteArray key = urlKey(url);
    if (memLookup(key, out))
        return true;
    if (diskLookup(key, out))
        return true;
    return false;
}

void cacheMaybeStore(const QUrl &url, ReqClass cls, const QByteArray &body, int status,
                     const QByteArray &contentType, const QByteArray &cacheControl) {
    if (!knobs().cache || status != 200 || body.isEmpty())
        return;
    if (body.size() > knobs().entryMaxBytes)
        return;
    const QByteArray cc = cacheControl.toLower();
    if (cc.contains("no-store") || cc.contains("no-cache"))
        return;
    ReqClass kind = cls;
    if (contentType.startsWith("image/"))
        kind = ReqIcon;
    if (kind == ReqBulk)
        return;
    CacheEntry entry;
    entry.body = body;
    entry.status = status;
    entry.content_length = body.size();
    entry.stored_at_ms = QDateTime::currentMSecsSinceEpoch();
    const qint64 ttl = (kind == ReqIcon) ? knobs().iconTtlMs : knobs().queryTtlMs;
    entry.expires_at_ms = entry.stored_at_ms + ttl;
    memStore(urlKey(url), entry);
    if (kind == ReqIcon)
        diskStore(urlKey(url), entry);
    counters().cache_stores.fetch_add(1);
}

/* ── 同源并发闸门:一个 host 同时最多 maxPerHost 条(到点放行,绝不干等) ── */
QMutex g_gateMutex;
QWaitCondition g_gateCv;
QHash<QString, int> g_gateCount;

void gateAcquire(const QString &host, int cap, qint64 maxWaitMs) {
    QElapsedTimer timer;
    timer.start();
    QMutexLocker lock(&g_gateMutex);
    for (;;) {
        int &n = g_gateCount[host];
        if (n < cap) {
            ++n;
            return;
        }
        if (timer.elapsed() >= maxWaitMs) {
            ++n;
            counters().throttled.fetch_add(1);
            return;
        }
        g_gateCv.wait(&g_gateMutex, 50);
    }
}

void gateRelease(const QString &host) {
    QMutexLocker lock(&g_gateMutex);
    auto it = g_gateCount.find(host);
    if (it != g_gateCount.end()) {
        if (it.value() > 1)
            it.value() -= 1;
        else
            g_gateCount.erase(it);
    }
    g_gateCv.wakeAll();
}

/* ── 一个请求任务(池里跑) ── */
struct NetJob {
    QUrl url;
    QByteArray method;
    QList<QPair<QByteArray, QByteArray>> headers;
    int timeoutMs = 0;
    bool forceHttp1 = false;
    int gateHeld = 0;

    QMutex m;
    QWaitCondition cv;
    QByteArray pending;
    QNetworkReply *reply = nullptr; /* 只在 worker 线程里碰 */
    QObject *workerObj = nullptr;
    bool headersReady = false;
    bool finished = false;
    bool ioError = false;
    bool cancelled = false;
    bool gateReleased = false;
    int status = 0;
    int http2 = 0;
    int reused = -1;
    qint64 contentLength = -1;
    qint64 totalLength = -1;
    qint64 wireBytes = -1;
    qint64 bodyBytes = 0;
    int acceptRanges = 0;
    int isRange = 0;
    QByteArray contentEncoding;
    QByteArray cacheControl;
    QByteArray contentType;
    QList<QPair<QByteArray, QByteArray>> headerPairs;
    bool wantHeaders = false;
    QByteArray maskedUrl;
    QElapsedTimer clock;
    QElapsedTimer lastData;
    qint64 connMs = -1;
    qint64 ttfbMs = -1;
    int stallMs = 60000;
};

class NetWorker;

struct PoolWorker {
    QThread *thread = nullptr;
    NetWorker *worker = nullptr;
    int load = 0;
};

extern QMutex g_poolMutex;
extern QVector<PoolWorker *> g_pool;
extern QHash<QString, int> g_hostWorker;

class NetWorker : public QObject {
public:
    QNetworkAccessManager *nam = nullptr;
    QHash<QNetworkReply *, QSharedPointer<NetJob>> active;
    QTimer *watchdog = nullptr;
    int *load = nullptr; /* 指向 PoolWorker::load(挑选线程用) */

    void init() {
        nam = new QNetworkAccessManager(this);
        configureNam(nam);
        watchdog = new QTimer(this);
        watchdog->setInterval(120);
        QObject::connect(watchdog, &QTimer::timeout, this, [this]() { sweep(); });
        watchdog->start();
    }

    /* 背压:消费者降到低水位就叫这里接着抽。(只在 worker 线程) */
    void drain(const QSharedPointer<NetJob> &job) {
        QNetworkReply *reply = job->reply;
        if (reply == nullptr)
            return;
        {
            QMutexLocker lock(&job->m);
            if (job->pending.size() >= kPendingHighWater)
                return;
            if (job->cancelled)
                return;
            /* 连接被对端掐了、回复又没结束:再读只会换来一行
             * "QIODevice::read (QSslSocket): device not open" + 空数据 —— 如实按传输中断收尾,
             * 不再空转刷屏(与直连路径 harvestReply 同一个判据)。 */
            if (!reply->isFinished() && !reply->isOpen()) {
                job->ioError = true;
                job->finished = true;
                job->cv.wakeAll();
                return;
            }
        }
        const QByteArray chunk = reply->readAll();
        if (!chunk.isEmpty()) {
            QMutexLocker lock(&job->m);
            job->pending += chunk;
            job->bodyBytes += chunk.size();
            job->lastData.restart();
            job->cv.wakeAll();
        }
    }

    void resumeJob(const QSharedPointer<NetJob> &job) { drain(job); }

    void abortJob(const QSharedPointer<NetJob> &job) {
        if (job->reply != nullptr && !job->reply->isFinished())
            job->reply->abort();
        settle(job, true);
    }

    /* 预热:解析主机(计时)后对根路径发一个 HEAD。
     * 为什么不是 connectToHostEncrypted:实测(2026-09-27)那个"预连接"建出来的连接
     * 不会被后面的 get() 认领(第一个请求照样 reused=no、照样付 600~700ms 握手)。
     * 一个 HEAD 是**真请求**:回复一读完,QNetworkAccessManager 就把这条连接
     * 留在连接缓存里,h2 也顺带用 ALPN 谈好了 —— 后面的搜索/图标直接复用。 */
    void warm(const QUrl &target) {
        const QString host = target.host();
        const int port = portOf(target);
        const bool tls = isHttps(target);
        const int dnsMs = recordDns(host); /* 阻塞:预热本来就在后台线程,顺手把 DNS 也量了 */
        if (dnsMs < 0) {
            counters().prewarm_fail.fetch_add(1);
            return;
        }
        QUrl url = target;
        url.setQuery(QString()); /* 只预热路径:不带 query,免得真去搜一遍 */
        url.setFragment(QString());
        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(8000);
        QNetworkReply *reply = nam->head(req);
        if (reply == nullptr) {
            counters().prewarm_fail.fetch_add(1);
            return;
        }
        const QString warmHost = host;
        QObject::connect(reply, &QNetworkReply::finished, reply, [reply, warmHost]() {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            (void)reply->readAll(); /* 读干:连接才会回到 keep-alive 缓存里 */
            if (traceOn())
                qInfo("NETTRACE-PREWARM-DONE %s status=%d err=%d", qPrintable(warmHost), status,
                      int(reply->error()));
            reply->deleteLater();
        });
        markHostWarm(host);
        counters().prewarm_ok.fetch_add(1);
        if (traceOn())
            qInfo("NETTRACE-PREWARM %s:%d dns=%dms tls=%d", qPrintable(host), port, dnsMs, tls ? 1 : 0);
    }

    void startJob(const QSharedPointer<NetJob> &job) {
        QNetworkRequest qreq(job->url);
        qreq.setAttribute(QNetworkRequest::Http2AllowedAttribute, job->forceHttp1 ? false : true);
        qreq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        qreq.setTransferTimeout(job->timeoutMs > 0 ? job->timeoutMs : kDefaultTimeoutMs);
        for (const QPair<QByteArray, QByteArray> &h : job->headers)
            qreq.setRawHeader(h.first, h.second);

        QNetworkReply *reply = nullptr;
        if (job->method == "HEAD")
            reply = nam->head(qreq);
        else
            reply = nam->get(qreq);
        if (reply == nullptr) {
            QMutexLocker lock(&job->m);
            job->ioError = true;
            job->cv.wakeAll();
            return;
        }
        reply->setReadBufferSize(kReplyBufBytes);
        {
            QMutexLocker lock(&job->m);
            job->reply = reply;
            job->lastData.start();
        }
        active.insert(reply, job);

        QObject::connect(reply, &QNetworkReply::encrypted, reply, [job]() {
            QMutexLocker lock(&job->m);
            if (job->connMs < 0)
                job->connMs = job->clock.elapsed();
        });
        QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [this, job]() { onMeta(job); });
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [this, job]() { drain(job); });
        QObject::connect(reply, &QNetworkReply::finished, reply, [this, job]() { onFinished(job); });
    }

    void onMeta(const QSharedPointer<NetJob> &job) {
        QNetworkReply *reply = job->reply;
        if (reply == nullptr)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        {
            QMutexLocker lock(&job->m);
            if (job->ttfbMs < 0)
                job->ttfbMs = job->clock.elapsed();
        }
        /* 3xx 不是最终状态:Qt 正按 RedirectPolicy 继续跟(镜像站每个文件先 302 再 200)。 */
        const bool redirecting = (status == 301 || status == 302 || status == 303 || status == 307 ||
                                  status == 308) && !reply->isFinished();
        if (status == 0 || redirecting)
            return;
        QMutexLocker lock(&job->m);
        if (job->headersReady)
            return;
        job->status = status;
        job->http2 = reply->attribute(QNetworkRequest::Http2WasUsedAttribute).toBool() ? 1 : 0;
        job->reused = (job->connMs >= 0) ? 0 : 1;
        job->contentEncoding = reply->rawHeader("Content-Encoding");
        job->contentType = reply->rawHeader("Content-Type");
        job->cacheControl = reply->rawHeader("Cache-Control");
        if (job->wantHeaders)
            job->headerPairs = reply->rawHeaderPairs();
        const QVariant len = reply->header(QNetworkRequest::ContentLengthHeader);
        job->contentLength = len.isValid() ? len.toLongLong() : -1;
        job->wireBytes = job->contentLength;
        const QByteArray acceptRanges = reply->rawHeader("Accept-Ranges").trimmed().toLower();
        job->acceptRanges = (acceptRanges == QByteArrayLiteral("bytes")) ? 1 : 0;
        job->isRange = (status == 206) ? 1 : 0;
        job->headersReady = true;
        job->cv.wakeAll();
    }

    void onFinished(const QSharedPointer<NetJob> &job) {
        QNetworkReply *reply = job->reply;
        if (reply == nullptr)
            return;
        /* 设备已经关掉的回复不要再读(Qt 每次都会回一行 "device not open" 并返回空)。
         * 正常收尾时设备还开着,这一读照旧把最后一批字节拿干净 —— 不跳过任何数据。 */
        const QByteArray rest = reply->isOpen() ? reply->readAll() : QByteArray();
        const QNetworkReply::NetworkError err = reply->error();
        const QVariant statusAttr = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        bool ioError = false;
        if (err != QNetworkReply::NoError && err != QNetworkReply::OperationCanceledError) {
            if (!statusAttr.isValid() || statusAttr.toInt() == 0)
                ioError = true;
        }
        {
            QMutexLocker lock(&job->m);
            if (!rest.isEmpty()) {
                job->pending += rest;
                job->bodyBytes += rest.size();
                job->lastData.restart();
            }
            if (job->status == 0 && statusAttr.isValid())
                job->status = statusAttr.toInt();
            job->finished = true;
            if (ioError && !job->cancelled)
                job->ioError = true;
            job->cv.wakeAll();
        }
        settle(job, false);
    }

    /* 收尾:释放回复与同源闸门(任务对象由 shared_ptr 兜着,消费者还能读到收尾后的状态) */
    void settle(const QSharedPointer<NetJob> &job, bool abortReply) {
        QNetworkReply *reply = nullptr;
        {
            QMutexLocker lock(&job->m);
            reply = job->reply;
            job->reply = nullptr;
            if (!job->gateReleased) {
                job->gateReleased = true;
                job->gateHeld = 0;
            }
        }
        if (abortReply && reply != nullptr && !reply->isFinished())
            reply->abort();
        if (reply != nullptr) {
            active.remove(reply);
            reply->deleteLater();
        }
        if (!job->url.host().isEmpty())
            gateRelease(job->url.host());
        if (load != nullptr) {
            QMutexLocker lock(&g_poolMutex);
            if (*load > 0)
                *load -= 1;
        }
    }

    /* 看门狗:取消的、响应头迟迟不到的、正文停摆的,一律收掉 —— 池线程绝不干等。 */
    void sweep() {
        const QList<QSharedPointer<NetJob>> jobs = active.values();
        for (const QSharedPointer<NetJob> &job : jobs) {
            bool kill = false;
            {
                QMutexLocker lock(&job->m);
                if (job->cancelled)
                    kill = true;
                else if (!job->headersReady && job->clock.elapsed() > knobs().connectMs)
                    kill = true;
                else if (job->headersReady && !job->finished && job->lastData.isValid() &&
                         job->lastData.elapsed() > job->stallMs)
                    kill = true;
            }
            if (kill) {
                QNetworkReply *reply = job->reply;
                {
                    QMutexLocker lock(&job->m);
                    if (!job->headersReady)
                        job->ioError = true;
                    else
                        job->ioError = true;
                    job->finished = true;
                    job->cv.wakeAll();
                }
                if (reply != nullptr && !reply->isFinished())
                    reply->abort();
                settle(job, false);
            }
        }
    }
};

QMutex g_poolMutex;
QVector<PoolWorker *> g_pool;
QHash<QString, int> g_hostWorker;

void poolShutdown(int timeout_ms);

/* 同一台主机的前几条请求**粘**在一条网络线程上(连接复用从第二条起就兑现);
 * 粘住的那条排到 2 条以上时,后来的走最闲的线程 —— 20 个图标不会被挤在一条 TCP 上。
 * 不论走哪条线程,同源并发仍然由 gateAcquire 卡在 maxPerHost(默认 6)条。 */
constexpr int kStickySoftLoad = 2;

PoolWorker *newPoolWorker() {
    PoolWorker *fresh = new PoolWorker();
    fresh->thread = new QThread();
    fresh->thread->setObjectName(QStringLiteral("sxcl-net"));
    fresh->worker = new NetWorker();
    fresh->worker->load = &fresh->load;
    fresh->worker->moveToThread(fresh->thread);
    NetWorker *w = fresh->worker;
    QObject::connect(fresh->thread, &QThread::started, w, [w]() { w->init(); });
    fresh->thread->start();
    g_pool.append(fresh);
    return fresh;
}

int leastLoadedWorker() {
    int best = 0;
    for (int i = 1; i < g_pool.size(); ++i) {
        if (g_pool.at(i)->load < g_pool.at(best)->load)
            best = i;
    }
    return best;
}

PoolWorker *poolWorkerFor(const QString &host) {
    static const bool hooked = [] {
        std::atexit([]() { poolShutdown(800); });
        return true;
    }();
    (void)hooked;
    QMutexLocker lock(&g_poolMutex);
    const auto known = g_hostWorker.constFind(host);
    if (known != g_hostWorker.constEnd() && known.value() < g_pool.size()) {
        PoolWorker *sticky = g_pool.at(known.value());
        if (sticky->load < kStickySoftLoad)
            return sticky;
        if (g_pool.size() <= 1)
            return sticky;
        return g_pool.at(leastLoadedWorker());
    }
    if (g_pool.size() < knobs().poolThreads) {
        PoolWorker *fresh = newPoolWorker();
        g_hostWorker.insert(host, g_pool.size() - 1);
        return fresh;
    }
    const int best = leastLoadedWorker();
    g_hostWorker.insert(host, best);
    return g_pool.at(best);
}

bool poolAvailable() {
    return knobs().pool && QCoreApplication::instance() != nullptr;
}

/* 是不是界面(GUI)线程。**界面线程永远不走池、也不重试**:
 * 池路径等的是条件变量,不跑事件循环 —— 界面线程一旦等在那里,连重绘都轮不上;
 * 直连路径wait的是嵌套事件循环,界面照样能刷新(Qt 自己的看门狗也不会判"卡住")。
 * 实测(2026-09-27,ui_no_hang 的 versions 一路):429 限流 + 换源那几秒,
 * 池路径会在界面线程上留下 3 秒的"无响应",直连路径不会。 */
bool onGuiThread() {
    QCoreApplication *app = QCoreApplication::instance();
    return app != nullptr && QThread::currentThread() == app->thread();
}

/* 直连路径:调用方线程 + 本线程共享的那个实例。Range 分片下载与批量镜像走它 ——
 * 与 2026-09-27 之前的行为逐字一致(只多了取证埋点),提速不许拿下载冒险。 */
int qtRequestDirect(QtTransport *t, const sxcl_http_request *req, const QUrl &url,
                    const std::shared_ptr<TraceAcc> &acc, sxcl_http_response *resp,
                    sxcl_http_body **body) {
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

    /* 相位信号:encrypted 只在**真做了 TLS 握手**时才发 —— 它没发就是连接复用过的
     * (同一 host 的第二次请求不再付握手钱)。metaDataChanged/readyRead 记首字节。 */
    QObject::connect(reply, &QNetworkReply::encrypted, reply, [acc]() {
        if (acc->conn_ms < 0)
            acc->conn_ms = acc->clock.elapsed();
    });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [acc]() {
        if (!acc->headers_seen) {
            acc->headers_seen = true;
            acc->ttfb_ms = acc->clock.elapsed();
        }
    });
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [acc]() {
        if (!acc->headers_seen) {
            acc->headers_seen = true;
            acc->ttfb_ms = acc->clock.elapsed();
        }
    });

    // 循环等"有响应头":每轮最多泵 50ms,超时由这里判(不让心跳误判成没响应)
    QElapsedTimer clock;
    clock.start();
    const int deadlineMs = req->timeout_ms > 0 ? int(req->timeout_ms) + 1000 : kDefaultTimeoutMs + 1000;
    int status = 0;
    for (;;) {
        /* **取消必须能立刻收场**:这一段等在"响应头到达"之前(DNS/连接/TLS 阶段),而
         * cancel_all 只 abort 已经登记 body 的请求(见 qtCancelAll)—— 登录/轮询这类
         * 30 秒超时的请求一旦卡在这里,cancel() 就撤不掉它:调用方(AccountTask 等)只能等满超时,
         * 退出路径于是被按住十几秒(现场:关掉登录窗退进程,进程多活 10 秒以上)。
         * 出口与 qtRead 里的取消一致:返回 SXCL_NET_ERR_CANCELLED。
         * 注:池路径不需要这一手 —— NetWorker::sweep() 每 120ms 扫一次 cancelled。 */
        if (t->cancelled) {
            reply->abort();
            reply->deleteLater();
            return SXCL_NET_ERR_CANCELLED;
        }
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
        {   // 运行日志:**warn 级**(连响应都没拿到 = 真故障,不是逐文件明细)
            char masked[SXCL_LOG_URL_MAX + 48];
            (void)sxcl_log_mask_url(req->url, masked, sizeof(masked));
            sxcl_log_write(SXCL_LOG_WARN, "net", "%s %s -> 拿不到响应 error=%d(%s) 耗时=%lldms",
                           method.constData(), masked, int(reply->error()),
                           qPrintable(reply->errorString()), (long long)clock.elapsed());
        }
        acc->status = 0;
        acc->wire_bytes = -1;
        acc->total_ms = acc->clock.elapsed();
        traceFinish(acc);
        reply->abort();
        reply->deleteLater();
        return t->cancelled ? SXCL_NET_ERR_CANCELLED : SXCL_NET_ERR_CONNECT;
    }

    // ── 运行日志(debug 级):每个请求一行 ──
    // 这里是**所有 Qt 传输**的总口(下载引擎的每个文件也走它),所以默认级别(INFO)不记,
    // 只在 SXCL_LOG_LEVEL=debug 时留档:方法 / 打码后的 URL / 状态码 / 字节数 / 耗时 / Range。
    // 默认级别下"每个文件的逐条下载明细"不落盘,这就是 docs/17 清单里那条"不记"的落地位置。
    if (sxcl_log_enabled(SXCL_LOG_DEBUG)) {
        char masked[SXCL_LOG_URL_MAX + 48];
        (void)sxcl_log_mask_url(req->url, masked, sizeof(masked));
        const QVariant lengthHeader = reply->header(QNetworkRequest::ContentLengthHeader);
        const qint64 contentLength = lengthHeader.isValid() ? lengthHeader.toLongLong() : -1;
        sxcl_log_write(SXCL_LOG_DEBUG, "net", "%s %s -> %d 字节=%lld 耗时=%lldms range=%lld",
                       method.constData(), masked, status, (long long)contentLength,
                       (long long)clock.elapsed(), (long long)req->range_start);
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

    acc->status = status;
    acc->http2 = reply->attribute(QNetworkRequest::Http2WasUsedAttribute).toBool() ? 1 : 0;
    acc->encoding = reply->rawHeader("Content-Encoding");
    {
        bool okLen = false;
        const qint64 declared = reply->rawHeader("Content-Length").trimmed().toLongLong(&okLen);
        acc->wire_bytes = okLen ? declared : -1;
    }
    /* 明文 http 没有握手信号,复用测不到(报 -1);https 没收到 encrypted 就是复用。 */
    if (url.scheme() == QLatin1String("https"))
        acc->reused = (acc->conn_ms >= 0) ? 0 : 1;
    acc->dns_ms = hostIsWarm(url.host(), 300000) ? 0 : dnsFor(url.host());

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
    b->acc = acc;
    /* 停滞阈值 = 请求超时的 2 倍(引擎给的 30s -> 60s 无数据就当这条路死了)。
     * 比超时宽松是有意的:慢到 32KB/s 的源每 1~2 秒也会来一批字节,60 秒没动静才是真停摆。 */
    b->stallMs = (req->timeout_ms > 0 ? int(req->timeout_ms) : kDefaultTimeoutMs) * 2;
    if (b->stallMs < 30000) {
        b->stallMs = 30000;
    }
    b->lastData.start();
    {
        char masked[SXCL_LOG_URL_MAX + 48];
        (void)sxcl_log_mask_url(req->url, masked, sizeof(masked));
        b->maskedUrl = QByteArray(masked);
    }
    b->pending = reply->readAll(); // 头到达时往往已经带了第一批正文
    b->cacheUrl = urlKey(url);
    b->cacheStatus = status;
    b->cacheContentType = reply->rawHeader("Content-Type");
    b->cacheControl = reply->rawHeader("Cache-Control");
    t->bodies.insert(b);
    *body = reinterpret_cast<sxcl_http_body *>(b);
    return SXCL_NET_OK;
}

/* 池路径:搜索 / 图标 / 清单这类"要快的小请求"。发在共享网络线程上,按 host 粘住,
 * 于是同一条 TCP+TLS 连接(HTTP/2 时还是同一条流)被后续请求反复用。 */
int qtRequestPooled(QtTransport *t, const sxcl_http_request *req, const QUrl &url, ReqClass cls,
                    const std::shared_ptr<TraceAcc> &acc, sxcl_http_response *resp,
                    sxcl_http_body **body) {
    QSharedPointer<NetJob> job(new NetJob());
    job->url = url;
    job->method = acc->method;
    job->timeoutMs = req->timeout_ms > 0 ? int(req->timeout_ms) : 0;
    job->forceHttp1 = req->force_http1 != 0;
    job->stallMs = (req->timeout_ms > 0 ? int(req->timeout_ms) : kDefaultTimeoutMs) * 2;
    if (job->stallMs < 30000)
        job->stallMs = 30000;
    job->wantHeaders = (req->on_header != nullptr);
    if (req->extra_headers != nullptr) {
        for (const char *const *h = req->extra_headers; *h != nullptr; ++h) {
            const QByteArray line(*h);
            const int colon = line.indexOf(':');
            if (colon > 0)
                job->headers.append(qMakePair(line.left(colon).trimmed(), line.mid(colon + 1).trimmed()));
        }
    }
    {
        char masked[SXCL_LOG_URL_MAX + 48];
        (void)sxcl_log_mask_url(req->url, masked, sizeof(masked));
        job->maskedUrl = QByteArray(masked);
    }
    const QString host = url.host();
    gateAcquire(host, knobs().maxPerHost, 3000); /* 同源并发上限;到点放行,绝不干等 */
    PoolWorker *worker = poolWorkerFor(host);
    {
        QMutexLocker lock(&g_poolMutex);
        worker->load += 1;
    }
    job->workerObj = worker->worker;
    job->clock.start();
    QMetaObject::invokeMethod(
        worker->worker, [w = worker->worker, job]() { w->startJob(job); }, Qt::QueuedConnection);

    /* 等响应头:连接阶段超时(默认 8s)一到就走人 —— 上层立刻换下一个源,
     * 不把"这个源连不上"拖成 30 秒的白屏。 */
    int rc = SXCL_NET_OK;
    bool timedOut = false;
    {
        QMutexLocker lock(&job->m);
        while (!job->headersReady && !job->finished && !job->ioError && !job->cancelled) {
            if (job->clock.elapsed() >= knobs().connectMs) {
                timedOut = true;
                job->cancelled = true;
                break;
            }
            job->cv.wait(&job->m, 25);
        }
        if (!job->headersReady) {
            rc = t->cancelled ? SXCL_NET_ERR_CANCELLED : SXCL_NET_ERR_CONNECT;
        }
        if (rc != SXCL_NET_OK) {
            acc->status = 0;
            acc->wire_bytes = -1;
            acc->total_ms = job->clock.elapsed();
            acc->dns_ms = hostIsWarm(host, 300000) ? 0 : dnsFor(host);
            traceFinish(acc);
            if (timedOut) {
                sxcl_log_write(SXCL_LOG_WARN, "net", "%s -> 连不上/响应头 %dms 没到,放弃本次尝试",
                               job->maskedUrl.constData(), knobs().connectMs);
            }
        }
    }
    if (rc != SXCL_NET_OK) {
        QMetaObject::invokeMethod(
            worker->worker, [w = worker->worker, job]() { w->abortJob(job); }, Qt::QueuedConnection);
        return rc;
    }

    {
        QMutexLocker lock(&job->m);
        acc->status = job->status;
        acc->http2 = job->http2;
        acc->reused = job->reused;
        acc->encoding = job->contentEncoding;
        acc->wire_bytes = job->wireBytes;
        acc->conn_ms = job->connMs;
        acc->ttfb_ms = job->ttfbMs;
        acc->dns_ms = hostIsWarm(host, 300000) ? 0 : dnsFor(host);
        resp->status = job->status;
        resp->content_length = job->contentLength;
        resp->total_length = job->totalLength;
        resp->accept_ranges = job->acceptRanges;
        resp->is_range_response = job->isRange;
        resp->range_start = -1;
        resp->range_end = -1;
    }
    /* 响应头回调(正版登录要读 Retry-After):头在 job 里,出了锁再回调。 */
    if (req->on_header != nullptr) {
        QList<QPair<QByteArray, QByteArray>> pairs;
        {
            QMutexLocker lock(&job->m);
            pairs = job->headerPairs;
        }
        for (const QPair<QByteArray, QByteArray> &p : pairs)
            req->on_header(req->header_userdata, p.first.constData(), p.second.constData());
    }
    QtBody *b = new QtBody();
    b->job = job;
    b->acc = acc;
    b->stallMs = job->stallMs;
    b->lastData.start();
    b->maskedUrl = job->maskedUrl;
    b->cacheUrl = urlKey(url);
    b->cacheStatus = job->status;
    b->cacheContentType = job->contentType;
    b->cacheControl = job->cacheControl;
    /* 注意:响应头到达时往往已经带了第一批正文。那些字节**留在 job->pending 里**,
     * 由 qtRead 从 job 取 —— 抄进 b->pending 就等于把它们丢了(实测:20 个图标里 4 个读成 0 字节)。 */
    t->bodies.insert(b);
    *body = reinterpret_cast<sxcl_http_body *>(b);
    (void)cls;
    return SXCL_NET_OK;
}

void qtCloseBody(void *ctx, sxcl_http_body *body);

/* 路由器:先查缓存;要快的小请求走共享网络线程池,大文件走直连。
 * 服务器 5xx/429 这种**还没把正文交出去**的失败,按退避重试;连接类失败不重试 ——
 * 让上层立刻换下一个源,这才是"更快回退"。 */
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
    if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
        return SXCL_NET_ERR_UNSUPPORTED;

    auto acc = std::make_shared<TraceAcc>();
    acc->clock.start();
    acc->url = QByteArray(req->url);
    acc->method = (req->method && *req->method) ? QByteArray(req->method) : QByteArray("GET");

    const bool shareable = requestIsShareable(req, url);
    const ReqClass cls = classifyUrl(url);
    const bool pooled = shareable && cls != ReqBulk && poolAvailable() && !onGuiThread();

    /* 结果复用:同一查询 60 秒内不再上网;图标两级(内存 + 用户缓存目录的磁盘)。 */
    if (shareable && knobs().cache) {
        CacheEntry entry;
        if (cacheLookup(url, &entry)) {
            acc->from_cache = entry.from_disk ? 2 : 1;
            acc->status = entry.status;
            acc->wire_bytes = -1;
            QtBody *b = new QtBody();
            b->pending = entry.body;
            b->finished = true;
            b->acc = acc;
            b->stallMs = kDefaultTimeoutMs;
            b->lastData.start();
            b->cacheWanted = false;
            b->cacheDone = true;
            t->bodies.insert(b);
            resp->status = entry.status;
            resp->content_length = entry.content_length;
            resp->accept_ranges = 0;
            *body = reinterpret_cast<sxcl_http_body *>(b);
            return SXCL_NET_OK;
        }
    }

    int rc = SXCL_NET_ERR_CONNECT;
    for (int attempt = 1;; ++attempt) {
        acc->attempts = attempt;
        acc->finalized = false; /* 重试时重新定稿:上一次只用于日志与统计 */
        if (pooled)
            rc = qtRequestPooled(t, req, url, cls, acc, resp, body);
        else
            rc = qtRequestDirect(t, req, url, acc, resp, body);
        if (rc != SXCL_NET_OK)
            return t->cancelled ? SXCL_NET_ERR_CANCELLED : rc;
        /* 缓存:读干了才写(见下 bodyMaybeStoreCache);这里只登记意图。 */
        if (shareable && knobs().cache && cls != ReqBulk) {
            QtBody *b = reinterpret_cast<QtBody *>(*body);
            b->cacheWanted = true;
            b->cacheDone = false;
            b->cacheClass = int(cls);
            b->cacheBuf.reserve(
                size_t(qBound<qint64>(qint64(4096), resp->content_length, qint64(1 << 20))));
        }
        const bool retryable = shareable && !onGuiThread() &&
                               (resp->status == 408 || resp->status == 429 || resp->status >= 500);
        if (!retryable || attempt > knobs().retryMax || acc->clock.elapsed() >= knobs().retryBudgetMs)
            return SXCL_NET_OK;
        counters().retries.fetch_add(1);
        qtCloseBody(t, *body); /* 没把正文交出去,丢掉不算浪费 */
        *body = nullptr;
        QThread::msleep(unsigned(knobs().retryBackoffMs * attempt));
        if (t->cancelled)
            return SXCL_NET_ERR_CANCELLED;
    }
}

/* 正文读干后写缓存(半途放弃的不写)。 */
void bodyMaybeStoreCache(QtBody *b) {
    if (b->cacheDone)
        return;
    b->cacheDone = true;
    if (!b->cacheWanted)
        return;
    cacheMaybeStore(QUrl(QString::fromUtf8(b->cacheUrl)), ReqClass(b->cacheClass), b->cacheBuf,
                    b->cacheStatus, b->cacheContentType, b->cacheControl);
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
        if (b->job) {
            /* ── 池路径:字节由共享网络线程灌进 job,这里只等着取 ──
             * 不跑嵌套事件循环:调用方线程干净地睡在条件变量上,池线程负责把 socket 抽干。 */
            QSharedPointer<NetJob> job = b->job;
            int64_t got = -2; /* -2 = 还没有,继续等 */
            bool resume = false;
            {
                QMutexLocker lock(&job->m);
                if (!job->pending.isEmpty()) {
                    const int take = int(qMin<qint64>(qint64(len), qint64(job->pending.size())));
                    std::memcpy(buf, job->pending.constData(), size_t(take));
                    job->pending.remove(0, take);
                    got = take;
                    resume = (job->pending.size() < kPendingLowWater && job->reply != nullptr);
                } else if (job->cancelled) {
                    got = SXCL_NET_ERR_CANCELLED;
                } else if (job->ioError) {
                    got = SXCL_NET_ERR_IO;
                } else if (job->finished) {
                    got = 0;
                }
            }
            if (resume) {
                QObject *w = job->workerObj;
                if (w != nullptr) {
                    QMetaObject::invokeMethod(
                        w, [job]() {
                            auto *worker = static_cast<NetWorker *>(job->workerObj);
                            if (worker != nullptr)
                                worker->resumeJob(job);
                        },
                        Qt::QueuedConnection);
                }
            }
            if (got != -2) {
                if (got > 0) {
                    if (b->acc)
                        b->acc->body_bytes += got;
                    if (b->cacheWanted && b->cacheBuf.size() <= knobs().entryMaxBytes)
                        b->cacheBuf.append(static_cast<const char *>(buf), int(got));
                }
                if (got == 0)
                    bodyMaybeStoreCache(b);
                return got;
            }
            {
                QMutexLocker lock(&job->m);
                if (job->pending.isEmpty() && !job->finished && !job->ioError && !job->cancelled) {
                    if (job->lastData.isValid() && job->lastData.elapsed() >= job->stallMs) {
                        job->cancelled = true;
                        b->ioError = true;
                    } else {
                        job->cv.wait(&job->m, 50);
                    }
                }
            }
            if (b->ioError) {
                QObject *w = job->workerObj;
                if (w != nullptr) {
                    QMetaObject::invokeMethod(
                        w, [job]() {
                            auto *worker = static_cast<NetWorker *>(job->workerObj);
                            if (worker != nullptr)
                                worker->abortJob(job);
                        },
                        Qt::QueuedConnection);
                }
                return SXCL_NET_ERR_IO;
            }
            if (t->cancelled) {
                QMutexLocker lock(&job->m);
                job->cancelled = true;
                return SXCL_NET_ERR_CANCELLED;
            }
            continue;
        }
        if (!b->pending.isEmpty()) {
            const int take = int(qMin<qint64>(qint64(len), qint64(b->pending.size())));
            std::memcpy(buf, b->pending.constData(), size_t(take));
            b->pending.remove(0, take);
            if (b->acc)
                b->acc->body_bytes += take;
            if (b->cacheWanted && b->cacheBuf.size() <= knobs().entryMaxBytes)
                b->cacheBuf.append(static_cast<const char *>(buf), int(take));
            return take;
        }
        if (b->finished) {
            bodyMaybeStoreCache(b);
            return 0;
        }
        if (b->ioError)
            return SXCL_NET_ERR_IO;
        /* 先把回复的当前状态同步过来:信号是一次性的,已经结束的回复不会再发信号 */
        harvestReply(b);
        if (b->ioError)
            return SXCL_NET_ERR_IO;
        if (!b->pending.isEmpty())
            continue;
        if (b->finished) {
            bodyMaybeStoreCache(b);
            return 0;
        }
        if (t->cancelled)
            return SXCL_NET_ERR_CANCELLED;
        /* 空转上限:连续 N 轮(每轮最多 50ms)一个字节都没收到,就当这条连接半死。
         * 实测(2026-09-22 晚,界面点「启动」→ 补全文件 + 限速下载):服务端把空闲连接关掉之后,
         * Qt 的回复**既不发 finished 也不置 error**,readAll() 每次都打一行
         * "QIODevice::read (QSslSocket): device not open" 并返回空 —— 我们的循环就空转,
         * stderr 刷屏、界面线程被队列信号拖住(连截图定时器都轮不上)。
         * 阈值 200 轮 ≈ 10 秒:比它更慢的源本来就会被 60 秒的停滞看门狗兜住,
         * 而且报 IO 错误只是"这次尝试作废" —— 引擎保留已下部分,换路或原路续传,不会丢进度。 */
        /* 停滞看门狗:连接还在,但一个字节都不来。不判这条会永久挂住(见 QtBody 里的说明)。
         * 报 IO 错误,让引擎按"传输中断"处理 —— 保留已下部分,换路或续传,而不是干等。 */
        if (b->lastData.isValid() && b->lastData.elapsed() >= b->stallMs) {
            sxcl_log_write(SXCL_LOG_WARN, "net", "%s -> 传输停滞 %lldms 无数据,放弃本次尝试",
                           b->maskedUrl.constData(), (long long)b->lastData.elapsed());
            b->reply->abort();
            b->ioError = true;
            return SXCL_NET_ERR_IO;
        }
        pumpReply(b->reply, 50); /* 每轮最多 50ms,由上面的状态判断决定是否继续 */
    }
}

void qtCloseBody(void *ctx, sxcl_http_body *body) {
    QtTransport *t = asTransport(ctx);
    QtBody *b = asBody(body);
    if (!t || !b)
        return;
    t->bodies.remove(b);
    if (b->acc && !b->acc->finalized) {
        b->acc->total_ms = b->acc->clock.elapsed();
        traceFinish(b->acc);
    }
    if (b->job) {
        /* 池路径:没读完就关 = 放弃。叫 worker 收掉回复,连接回连接池(keep-alive 不白费)。 */
        QSharedPointer<NetJob> job = b->job;
        bool needAbort = false;
        {
            QMutexLocker lock(&job->m);
            if (!job->finished) {
                job->cancelled = true;
                needAbort = true;
            }
        }
        QObject *w = job->workerObj;
        if (needAbort && w != nullptr) {
            QMetaObject::invokeMethod(
                w, [job]() {
                    auto *worker = static_cast<NetWorker *>(job->workerObj);
                    if (worker != nullptr)
                        worker->abortJob(job);
                },
                Qt::QueuedConnection);
        }
    }
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
        if (b->job) { /* 池路径:取消 = 叫 worker 收回复;连接回池,不白扔 */
            QSharedPointer<NetJob> job = b->job;
            {
                QMutexLocker lock(&job->m);
                job->cancelled = true;
                job->cv.wakeAll();
            }
            QObject *w = job->workerObj;
            if (w != nullptr) {
                QMetaObject::invokeMethod(
                    w, [job]() {
                        auto *worker = static_cast<NetWorker *>(job->workerObj);
                        if (worker != nullptr)
                            worker->abortJob(job);
                    },
                    Qt::QueuedConnection);
            }
        }
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
    if (t->nam != nullptr) {
        if (t->ownNam) {
            // 自己持有的(旧行为):先断掉所有 keep-alive 连接再销毁,否则 Qt 内部线程还在等,
            // 退出时会打印 "QWaitCondition: Destroyed while threads are still waiting"。
            t->nam->clearAccessCache();
            delete t->nam;
        } else {
            /* 共享的那个**故意不销毁**:连接、DNS、HTTP/2 会话都留在里面给同线程的下一次请求用
             * (这正是"第二次不再握手"的来源)。线程退出时由 TLS 里的持有者收尾。 */
            releaseSharedNam();
        }
        t->nam = nullptr;
    }
    /* 再把 Qt **全局线程池**里的活等完。DNS 查询(QHostInfo)就跑在那个池子上,
     * 池子是懒创建的进程级单例,退出时才析构 —— 那时线程还挂在它的等待条件上,
     * 于是打印 "QWaitCondition: Destroyed while threads are still waiting"(看着像我们的线程泄漏,
     * 其实是 Qt 静态析构的固有现象)。这里主动等一次,池子的线程就干净退出了。
     * 超时给 2s:查询通常几百毫秒就该完;真卡住也不能让收尾挂死(那比一行告警糟糕得多)。 */
    if (QThreadPool::globalInstance() != nullptr) {
        (void)QThreadPool::globalInstance()->waitForDone(2000);
    }
    delete t;
}

/* 收尾:把共享网络线程停干净。池线程是长命的 —— 它们的价值就是"连接留着下次用";
 * 进程要走了就得先请它们下班,否则 Qt 会在静态析构里撞上还活着的 QThread。 */
void poolShutdown(int timeout_ms) {
    QVector<PoolWorker *> workers;
    {
        QMutexLocker lock(&g_poolMutex);
        workers = g_pool;
        g_pool.clear();
        g_hostWorker.clear();
    }
    /* 收尾必须在**那条线程自己**里做:QObject(QTimer 是它的子对象)换线程销毁,
     * Qt 会当场打 "Timers cannot be stopped from another thread"(实测:sxcl-dl 退出时刷屏)。
     * 所以这里只投一个请求,让池线程自己停表、退事件循环、deleteLater 自己。 */
    for (PoolWorker *one : workers) {
        if (one == nullptr)
            continue;
        NetWorker *w = one->worker;
        if (one->thread != nullptr) {
            if (w != nullptr) {
                QMetaObject::invokeMethod(
                    w,
                    [w]() {
                        if (w->watchdog != nullptr)
                            w->watchdog->stop();
                        w->active.clear();
                        if (w->thread() != nullptr)
                            w->thread()->quit();
                        w->deleteLater();
                    },
                    Qt::QueuedConnection);
            }
            if (!one->thread->wait(timeout_ms > 0 ? timeout_ms : 1500)) {
                one->thread->quit();
                (void)one->thread->wait(500);
            }
        }
    }
    /* QThread 对象与 PoolWorker 故意不删:它们是进程级单例,收尾时进程马上就走 ——
     * 从别的线程 delete 一个 QThread 同样是"换线程销毁",只会换来更多告警。 */
    for (PoolWorker *one : workers)
        one->worker = nullptr;
}

} // namespace

extern "C" void sxcl_transport_qt_bootstrap(void) {
    if (QCoreApplication::instance() != nullptr) {
        /* 已经引导过:补一次预热(幂等,已热的 host 直接跳过)。
         * 模组页第一次要网络的时候就走这里 —— 握手在用户点「搜索」之前就已经开始。 */
        (void)sxcl_net_prewarm_defaults();
        return;
    }
    // 故意不释放:进程级单例,与 Qt 的常规用法一致
    static int argc = 1;
    static char name[] = "sxcl-dl";
    static char *argv[] = {name, nullptr};
    new QCoreApplication(argc, argv);
    (void)sxcl_net_prewarm_defaults();
}

/* 收尾:把共享网络线程也停干净(纯 C 调用方在退出前调一次)。
 * 池线程是长命的 —— 它们的价值就是"连接留着下次用";进程要走了就得先请它们下班,
 * 否则 Qt 会在静态析构里撞上还活着的 QThread。 */
extern "C" void sxcl_transport_qt_drain(int timeout_ms) {
    if (QCoreApplication::instance() == nullptr) {
        return; // 没引导过 Qt:没有池子可等
    }
    poolShutdown(timeout_ms);
    QThreadPool *pool = QThreadPool::globalInstance();
    if (pool != nullptr) {
        (void)pool->waitForDone(timeout_ms > 0 ? timeout_ms : 1500);
    }
}

extern "C" int sxcl_net_trace_last(sxcl_net_sample *out) {
    if (out == nullptr)
        return 0;
    TraceSlot &slot = traceSlot();
    if (slot.has == 0)
        return 0;
    *out = slot.sample;
    return 1;
}

extern "C" void sxcl_net_stats_get(sxcl_net_stats *out) {
    if (out == nullptr)
        return;
    NetCounters &c = counters();
    out->requests = c.requests.load();
    out->cache_hits_mem = c.cache_mem.load();
    out->cache_hits_disk = c.cache_disk.load();
    out->cache_stores = c.cache_stores.load();
    out->prewarm_ok = c.prewarm_ok.load();
    out->prewarm_fail = c.prewarm_fail.load();
    out->retries = c.retries.load();
    out->throttled = c.throttled.load();
    out->bytes_wire = c.wire.load();
    out->bytes_body = c.body.load();
}

extern "C" void sxcl_net_stats_reset(void) {
    NetCounters &c = counters();
    c.requests.store(0);
    c.cache_mem.store(0);
    c.cache_disk.store(0);
    c.cache_stores.store(0);
    c.prewarm_ok.store(0);
    c.prewarm_fail.store(0);
    c.retries.store(0);
    c.throttled.store(0);
    c.wire.store(0);
    c.body.store(0);
}

/* 预热:解析主机 + 让**将要服务这台主机的那条池线程**先把连接建起来。
 * 只排队,不阻塞调用方(界面线程上调用也安全)。 */
static int prewarmOne(const QUrl &url) {
    const QString host = url.host();
    if (host.isEmpty() || !poolAvailable())
        return 0;
    if (hostIsWarm(host, 300000)) /* 5 分钟内热过就不再重复 */
        return 0;
    PoolWorker *worker = poolWorkerFor(host);
    QMetaObject::invokeMethod(
        worker->worker, [w = worker->worker, url]() { w->warm(url); }, Qt::QueuedConnection);
    markHostWarm(host);
    return 1;
}

extern "C" int sxcl_net_prewarm(const char *const *urls, size_t count) {
    if (urls == nullptr || !knobs().prewarm)
        return 0;
    int queued = 0;
    for (size_t i = 0; i < count; ++i) {
        if (urls[i] == nullptr)
            continue;
        const QUrl url(QString::fromUtf8(urls[i]));
        if (!url.isValid() || url.host().isEmpty())
            continue;
        if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
            continue;
        queued += prewarmOne(url);
    }
    return queued;
}

/* 模组页要用的那一组:搜索 API、图标 CDN、版本清单、镜像。幂等。 */
extern "C" int sxcl_net_prewarm_defaults(void) {
    if (!knobs().prewarm)
        return 0;
    /* 只列**不跳转**的路径:实测 2026-09-27,预热打到会 301/307 的路径上
     * (api.modrinth.com/ 是 301、cdn.modrinth.com/ 是 307、bmclapi2 根是 302),
     * 连接不会被后面的请求认领 —— 第一个搜索照样付 700ms 握手。
     * 打到 404/405/200 这种"就是最终响应"的路径上,连接才留在缓存里。
     * cdn.modrinth.com 没有这种路径(全站 307),图标那条路靠"第一批第一个建连、
     * 其余复用"自己解决。 */
    static const char *const kWarmUrls[] = {
        "https://api.modrinth.com/v2/search",
        "https://mod.mcimirror.top/curseforge/v1/mods/search",
        "https://api.curseforge.com/v1/mods/search",
        "https://media.forgecdn.net/robots.txt",
        "https://edge.forgecdn.net/robots.txt",
        "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json",
        "https://launchermeta.mojang.com/mc/game/version_manifest.json",
        "https://bmclapi2.bangbang93.com/mc/game/version_manifest.json",
    };
    int queued = 0;
    for (const char *one : kWarmUrls) {
        const QUrl url(QString::fromLatin1(one));
        if (url.host().isEmpty())
            continue;
        queued += prewarmOne(url);
    }
    return queued;
}

extern "C" int sxcl_net_cache_clear(void) {
    int removed = 0;
    {
        QMutexLocker lock(&g_cacheMutex);
        removed += g_cache.size();
        g_cache.clear();
        g_cacheBytes = 0;
    }
    const QDir dir(iconCacheDir());
    const QStringList files = dir.entryList(QStringList() << QStringLiteral("*.bin"), QDir::Files);
    for (const QString &one : files) {
        if (QFile::remove(dir.filePath(one)))
            removed += 1;
    }
    return removed;
}

extern "C" int sxcl_net_cache_dir_set(const char *dir) {
    QMutexLocker lock(&g_cacheMutex);
    g_cacheDirOverride = (dir != nullptr) ? QByteArray(dir) : QByteArray();
    return 0;
}

extern "C" void sxcl_net_cache_counts(int64_t *mem_entries, int64_t *disk_files) {
    if (mem_entries != nullptr) {
        QMutexLocker lock(&g_cacheMutex);
        *mem_entries = g_cache.size();
    }
    if (disk_files != nullptr) {
        const QDir dir(iconCacheDir());
        *disk_files = dir.entryList(QStringList() << QStringLiteral("*.bin"), QDir::Files).size();
    }
}

extern "C" sxcl_transport *sxcl_transport_qt_create(void) {
    QtTransport *t = new QtTransport();
    if (knobs().legacy) {
        /* 旧行为(基准对照/回退):每次新建一个实例 —— 连接一次也复用不上。 */
        t->nam = new QNetworkAccessManager();
        configureNam(t->nam);
        t->ownNam = true;
    } else {
        t->nam = sharedNam();
        t->ownNam = false;
    }
    t->pub.ctx = t;
    t->pub.request = &qtRequest;
    t->pub.read = &qtRead;
    t->pub.close_body = &qtCloseBody;
    t->pub.cancel_all = &qtCancelAll;
    t->pub.destroy = &qtDestroy;
    return &t->pub;
}
