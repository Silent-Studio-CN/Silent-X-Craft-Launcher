/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* 网络层离线基准(tools/net_bench.ps1 调用)。
 *
 * 为什么放在传输层:模组搜索/图标/清单的每一次请求都从这里过,"每次请求一个
 * QNetworkAccessManager"这类结构问题也只能在这里量出来。这个程序**照抄产品路径**:
 *   一个用例 = 新建传输(和 mods_worker 一样)+ 一个自己的线程(和 ModsWorker 的 QThread 一样)
 *   + 发请求 + 读完正文 + 关掉 + 销毁传输;完了从 sxcl_net_trace_last 取一份实测样本。
 *
 * 用法:
 *   net_bench --plan <tsv> [--passes 2] [--thread-per-request 1] [--prewarm 1]
 *             [--timeout-ms 15000] [--max-bytes 8388608] [--label 名]
 * 计划文件(TSV,一行一个用例,# 开头是注释):
 *   suite <TAB> name <TAB> url [<TAB> "Header: v;Header2: v"]
 * 输出(TSV 到 stdout):
 *   SAMPLE <TAB> pass <TAB> suite <TAB> name <TAB> ok <TAB> status <TAB> dns <TAB> conn <TAB>
 *          ttfb <TAB> total <TAB> reused <TAB> src <TAB> wire <TAB> body <TAB> enc <TAB>
 *          http2 <TAB> tries <TAB> magic
 *   STATS  <TAB> pass <TAB> suite <TAB> requests <TAB> mem <TAB> disk <TAB> stores <TAB>
 *          prewarm_ok <TAB> prewarm_fail <TAB> retries <TAB> throttled <TAB> wire <TAB> body
 */

#include "sxcl/net.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QTextStream>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

/* ── 日志桩:基准工具不链接 libsxcl,只把三个被传输层用到的入口顶上 ── */
extern "C" void sxcl_log_write(int level, const char *module, const char *fmt, ...) {
    (void)level;
    (void)module;
    (void)fmt;
}
extern "C" int sxcl_log_enabled(int level) {
    (void)level;
    return 0;
}
extern "C" int sxcl_log_mask_url(const char *url, char *out, size_t out_len) {
    if (url == nullptr || out == nullptr || out_len == 0) {
        return -1;
    }
    const size_t n = std::strlen(url);
    const size_t take = (n < out_len - 1) ? n : out_len - 1;
    std::memcpy(out, url, take);
    out[take] = '\0';
    return int(take);
}

namespace {

struct Case {
    std::string suite;
    std::string name;
    std::string url;
    std::string headers;
};

struct Row {
    std::string suite;
    std::string name;
    int ok = 0;
    int status = 0;
    long long dns = -1;
    long long conn = -1;
    long long ttfb = -1;
    long long total = -1;
    int reused = -1;
    int src = 0;
    long long wire = -1;
    long long body = 0;
    std::string enc;
    int http2 = 0;
    int tries = 0;
    std::string magic;
    std::string note;
};

std::vector<std::string> split(const std::string &text, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (const char ch : text) {
        if (ch == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    out.push_back(cur);
    return out;
}

std::string trim(const std::string &text) {
    size_t a = 0;
    size_t b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r' || text[a] == '\n')) {
        ++a;
    }
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r' || text[b - 1] == '\n')) {
        --b;
    }
    return text.substr(a, b - a);
}

std::string magicOf(const char *data, size_t len) {
    if (data == nullptr || len < 4) {
        return "-";
    }
    const unsigned char *p = reinterpret_cast<const unsigned char *>(data);
    if (p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') {
        return "png";
    }
    if (p[0] == 0xFF && p[1] == 0xD8) {
        return "jpg";
    }
    if (p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F') {
        return "webp";
    }
    if (p[0] == 'G' && p[1] == 'I' && p[2] == 'F') {
        return "gif";
    }
    if (p[0] == '{' || p[0] == '[') {
        return "json";
    }
    if (p[0] == '<') {
        return "xml";
    }
    return "bin";
}

/* 一个用例:照抄产品路径(自己的线程 + 自己的传输 + 读完 + 关掉)。 */
Row runCase(const Case &c, int timeoutMs, size_t maxBytes) {
    Row row;
    row.suite = c.suite;
    row.name = c.name;

    std::vector<QByteArray> rawHeaders;
    std::vector<const char *> headerPtrs;
    if (!c.headers.empty()) {
        for (const std::string &one : split(c.headers, ';')) {
            const std::string t = trim(one);
            if (!t.empty()) {
                rawHeaders.push_back(QByteArray::fromStdString(t));
            }
        }
        for (const QByteArray &one : rawHeaders) {
            headerPtrs.push_back(one.constData());
        }
    }
    const char *const *headers = headerPtrs.empty() ? nullptr : headerPtrs.data();

    sxcl_transport *tr = sxcl_transport_qt_create();
    if (tr == nullptr) {
        row.note = "no-transport";
        return row;
    }
    sxcl_http_request req;
    std::memset(&req, 0, sizeof(req));
    req.url = c.url.c_str();
    req.method = "GET";
    req.range_start = -1;
    req.range_end = -1;
    req.extra_headers = headers;
    req.timeout_ms = timeoutMs;

    sxcl_http_response resp;
    std::memset(&resp, 0, sizeof(resp));
    sxcl_http_body *body = nullptr;
    const int rc = tr->request(tr->ctx, &req, &resp, &body);
    if (rc != SXCL_NET_OK || body == nullptr) {
        row.note = "request-failed";
        if (body != nullptr) {
            tr->close_body(tr->ctx, body);
        }
        if (tr->destroy != nullptr) {
            tr->destroy(tr->ctx);
        }
        sxcl_net_sample s{};
        if (sxcl_net_trace_last(&s) != 0) {
            row.status = s.status;
            row.http2 = s.http2;
            row.reused = s.reused;
            row.src = s.from_cache;
            row.dns = s.dns_ms;
            row.conn = s.conn_ms;
            row.ttfb = s.ttfb_ms;
            row.total = s.total_ms;
            row.wire = s.wire_bytes;
        }
        return row;
    }
    row.status = resp.status;

    /* 把正文读干(和 http.c / 下载引擎一样:一直读到 0)。函数里两次 malloc/delete,
     * 只为拿到"真的收到多少字节"和文件头(magic)。 */
    std::vector<char> buf;
    size_t cap = 65536;
    buf.resize(cap);
    size_t len = 0;
    const size_t hardCap = (maxBytes > 0) ? maxBytes : (8u * 1024u * 1024u);
    for (;;) {
        if (len == cap) {
            if (cap >= hardCap) {
                break;
            }
            cap = (cap * 2 > hardCap) ? hardCap : cap * 2;
            buf.resize(cap);
        }
        const int64_t got = tr->read(tr->ctx, body, buf.data() + len, cap - len);
        if (got <= 0) {
            if (got < 0) {
                row.note = "read-failed";
            }
            break;
        }
        len += size_t(got);
        if (len >= hardCap) {
            break;
        }
    }
    tr->close_body(tr->ctx, body);
    if (tr->destroy != nullptr) {
        tr->destroy(tr->ctx);
    }

    sxcl_net_sample s{};
    if (sxcl_net_trace_last(&s) != 0) {
        row.status = s.status;
        row.http2 = s.http2;
        row.reused = s.reused;
        row.src = s.from_cache;
        row.dns = s.dns_ms;
        row.conn = s.conn_ms;
        row.ttfb = s.ttfb_ms;
        row.total = s.total_ms;
        row.wire = s.wire_bytes;
        row.body = s.body_bytes;
        row.enc = std::string(s.content_encoding);
        row.tries = s.attempts;
        if (row.body == 0) {
            row.body = (long long)len;
        }
    }
    row.magic = magicOf(buf.data(), len);
    row.ok = (row.status >= 200 && row.status < 300) && (len > 0) ? 1 : 0;
    return row;
}

void printRow(const Row &r, int pass) {
    std::printf("SAMPLE\t%d\t%s\t%s\t%d\t%d\t%lld\t%lld\t%lld\t%lld\t%d\t%d\t%lld\t%lld\t%s\t%d\t%d\t%s\t%s\n",
                pass, r.suite.c_str(), r.name.c_str(), r.ok, r.status, r.dns, r.conn, r.ttfb,
                r.total, r.reused, r.src, r.wire, r.body, r.enc.empty() ? "-" : r.enc.c_str(),
                r.http2, r.tries, r.magic.c_str(), r.note.c_str());
}

/* 一批用例**同时**发(模组页就是这样:一张结果表 20 个图标一起上)。
 * 这是"第二次进页面 0 请求"和"连接复用"最直接的证据。 */
void runBatch(const std::vector<Case> &cases, const std::string &suite, int pass, int timeoutMs,
              long long maxBytes) {
    std::vector<Row> rows(cases.size());
    std::vector<std::thread> threads;
    QElapsedTimer clock;
    clock.start();
    for (size_t i = 0; i < cases.size(); ++i) {
        threads.emplace_back([&rows, &cases, i, timeoutMs, maxBytes]() {
            rows[i] = runCase(cases[i], timeoutMs, maxBytes);
        });
    }
    for (std::thread &t : threads) {
        t.join();
    }
    const qint64 wall = clock.elapsed();
    int ok = 0;
    long long body = 0;
    for (const Row &r : rows) {
        if (r.ok != 0) {
            ++ok;
        }
        body += r.body;
    }
    sxcl_net_stats st{};
    sxcl_net_stats_get(&st);
    std::printf("BATCH\t%d\t%s\t%lld\t%d\t%d\t%lld\t%lld\t%lld\t%lld\n", pass, suite.c_str(),
                (long long)wall, ok, int(cases.size()), (long long)st.requests,
                (long long)st.cache_hits_mem, (long long)st.cache_hits_disk, body);
    std::fflush(stdout);
}

void printStats(int pass, const std::string &suite) {
    sxcl_net_stats st{};
    sxcl_net_stats_get(&st);
    std::printf("STATS\t%d\t%s\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\t%lld\n", pass,
                suite.c_str(), (long long)st.requests, (long long)st.cache_hits_mem,
                (long long)st.cache_hits_disk, (long long)st.cache_stores,
                (long long)st.prewarm_ok, (long long)st.prewarm_fail, (long long)st.retries,
                (long long)st.throttled, (long long)st.bytes_wire, (long long)st.bytes_body);
}

} // namespace

int main(int argc, char **argv) {
    std::string plan;
    std::string label = "run";
    int passes = 2;
    int threadPerRequest = 1;
    int prewarm = 1;
    int timeoutMs = 15000;
    int batch = 1;
    int prewarmWaitMs = 700;
    std::string batchSuite = "icon";
    long long maxBytes = 8LL * 1024 * 1024;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = (i + 1) < argc;
        if (a == "--plan" && hasNext) {
            plan = argv[++i];
        } else if (a == "--label" && hasNext) {
            label = argv[++i];
        } else if (a == "--passes" && hasNext) {
            passes = std::atoi(argv[++i]);
        } else if (a == "--thread-per-request" && hasNext) {
            threadPerRequest = std::atoi(argv[++i]);
        } else if (a == "--prewarm" && hasNext) {
            prewarm = std::atoi(argv[++i]);
        } else if (a == "--timeout-ms" && hasNext) {
            timeoutMs = std::atoi(argv[++i]);
        } else if (a == "--max-bytes" && hasNext) {
            maxBytes = std::atoll(argv[++i]);
        } else if (a == "--prewarm-wait-ms" && hasNext) {
            prewarmWaitMs = std::atoi(argv[++i]);
        } else if (a == "--batch" && hasNext) {
            batch = std::atoi(argv[++i]);
        } else if (a == "--batch-suite" && hasNext) {
            batchSuite = argv[++i];
        }
    }
    if (plan.empty()) {
        std::fprintf(stderr, "net_bench: --plan <tsv> 必须有\n");
        return 2;
    }
    QFile file(QString::fromStdString(plan));
    if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "net_bench: 计划文件打不开: %s\n", plan.c_str());
        return 2;
    }
    std::vector<Case> cases;
    {
        QTextStream in(&file);
        while (!in.atEnd()) {
            const std::string line = trim(in.readLine().toStdString());
            if (line.empty() || line[0] == '#') {
                continue;
            }
            const std::vector<std::string> f = split(line, '\t');
            if (f.size() < 3) {
                continue;
            }
            Case c;
            c.suite = trim(f[0]);
            c.name = trim(f[1]);
            c.url = trim(f[2]);
            c.headers = (f.size() > 3) ? trim(f[3]) : std::string();
            if (!c.url.empty()) {
                cases.push_back(c);
            }
        }
    }
    if (cases.empty()) {
        std::fprintf(stderr, "net_bench: 计划里没有用例\n");
        return 2;
    }

    sxcl_transport_qt_bootstrap();
    if (prewarm != 0) {
        (void)sxcl_net_prewarm_defaults();
        /* 预热是"页一打开就开始握手",用户点搜索至少要几百毫秒之后。这里照抄那个节奏:
         * 不等这一下,量的就是"预热还没连上就被请求插队"的假数。 */
        if (prewarmWaitMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(prewarmWaitMs));
    }

    std::printf("# net_bench label=%s passes=%d cases=%zu thread_per_request=%d prewarm=%d\n",
                label.c_str(), passes, cases.size(), threadPerRequest, prewarm);
    std::fflush(stdout);

    for (int pass = 1; pass <= passes; ++pass) {
        std::string suiteNow;
        for (const Case &c : cases) {
            if (suiteNow != c.suite) {
                if (!suiteNow.empty()) {
                    printStats(pass, suiteNow);
                }
                suiteNow = c.suite;
                sxcl_net_stats_reset();
            }
            if (threadPerRequest != 0) {
                /* 产品路径:一次请求一个线程(mods_worker 每次 new 一个 QThread)。 */
                Row row;
                std::thread worker([&row, &c, timeoutMs, maxBytes]() { row = runCase(c, timeoutMs, maxBytes); });
                worker.join();
                printRow(row, pass);
            } else {
                printRow(runCase(c, timeoutMs, maxBytes), pass);
            }
            std::fflush(stdout);
        }
        if (!suiteNow.empty()) {
            printStats(pass, suiteNow);
        }
        /* 冷批 / 热批:和界面一样,一张结果表的图标是**同时**发的。
         * 冷批前清一次缓存,热批紧跟着 —— 两者的请求数差就是"第二次 0 请求"。 */
        if (pass == 1 && batch != 0) {
            std::vector<Case> batchCases;
            for (const Case &c : cases) {
                if (c.suite == batchSuite) {
                    batchCases.push_back(c);
                }
            }
            if (!batchCases.empty()) {
                (void)sxcl_net_cache_clear();
                sxcl_net_stats_reset();
                runBatch(batchCases, batchSuite + "-cold", 1, timeoutMs, maxBytes);
                sxcl_net_stats_reset();
                runBatch(batchCases, batchSuite + "-warm", 2, timeoutMs, maxBytes);
            }
        }
    }
    std::printf("# net_bench done\n");
    return 0;
}
