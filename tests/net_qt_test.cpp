/* Qt 传输后端测试:离线路径必须确定性通过;联网路径由环境变量开关控制。
 * 联网用例默认不开(CI 上网络抖动会变成假失败),本地联调时设:
 *   $env:SXCL_NET_ONLINE_TEST = "1"
 */
#include <QByteArray>
#include <QCoreApplication>
#include <QProcessEnvironment>

#include <cstdio>
#include <cstring>

#include "sxcl/net.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const char *what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  [!!] %s\n", what);
    }
}

static void checkEq(long long got, long long want, const char *what) {
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  [!!] %s: got %lld want %lld\n", what, got, want);
    }
}

static int doRequest(sxcl_transport *t, const char *url, int64_t rangeStart, int64_t rangeEnd,
                     int64_t timeoutMs, sxcl_http_response *resp, sxcl_http_body **body,
                     int forceHttp1 = 0) {
    sxcl_http_request req;
    std::memset(&req, 0, sizeof(req));
    req.url = url;
    req.method = "GET";
    req.range_start = rangeStart;
    req.range_end = rangeEnd;
    req.timeout_ms = timeoutMs;
    req.force_http1 = forceHttp1;
    return t->request(t->ctx, &req, resp, body);
}

static int64_t readAll(sxcl_transport *t, sxcl_http_body *body) {
    int64_t got = 0;
    char buf[16384];
    for (;;) {
        const int64_t n = t->read(t->ctx, body, buf, sizeof(buf));
        if (n <= 0)
            break;
        got += n;
    }
    return got;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    sxcl_transport *t = sxcl_transport_qt_create();
    check(t != nullptr, "创建 Qt 传输后端");
    if (!t) {
        return 1;
    }

    sxcl_http_response resp;
    sxcl_http_body *body = nullptr;

    // 1) 参数校验
    checkEq(doRequest(t, nullptr, -1, -1, 3000, &resp, &body), SXCL_NET_ERR_BAD_ARG, "空 URL 报参数错");
    checkEq(doRequest(t, "file:///C:/windows/win.ini", -1, -1, 3000, &resp, &body),
            SXCL_NET_ERR_UNSUPPORTED, "非 http/https 协议被拒(防止元数据指向本地文件)");

    // 2) 连不上的地址:必须是 CONNECT 类错误,不能假装成功
    const int rcDown = doRequest(t, "http://127.0.0.1:1/", -1, -1, 4000, &resp, &body);
    check(rcDown == SXCL_NET_ERR_CONNECT, "连不上的端口报 CONNECT 错误");
    if (body) {
        t->close_body(t->ctx, body);
        body = nullptr;
    }

    // 3) 联网用例(可选)
    const bool online = !QProcessEnvironment::systemEnvironment()
                             .value(QStringLiteral("SXCL_NET_ONLINE_TEST"))
                             .isEmpty();
    if (online) {
        const char *manifest = "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
        const int rc = doRequest(t, manifest, -1, -1, 20000, &resp, &body);
        checkEq(rc, SXCL_NET_OK, "取版本清单:传输层成功");
        checkEq(resp.status, 200, "版本清单 HTTP 200");
        // 非 Range 请求允许走 gzip:CDN 压缩后不给 Content-Length 是正常的,
        // 引擎靠元数据里的期望大小 + SHA-1 校验兜底,不依赖这个头。
        std::printf("  [info] 清单 content_length=%lld(压缩响应可能为 -1)\n",
                    static_cast<long long>(resp.content_length));

        QByteArray payload;
        if (body) {
            char buf[16384];
            for (;;) {
                const int64_t n = t->read(t->ctx, body, buf, sizeof(buf));
                if (n <= 0)
                    break;
                payload.append(buf, int(n));
            }
            t->close_body(t->ctx, body);
            body = nullptr;
        }
        check(payload.size() > 1024, "正文读回来了(>1KB)");
        check(payload.indexOf(QByteArrayLiteral("\"latest\"")) >= 0, "正文是版本清单 JSON");

        // 4) Range 请求:两种 HTTP 版本各测一次,确认哪条路真的兑现 Range
        for (int forceH1 = 0; forceH1 <= 1; ++forceH1) {
            const char *tag = forceH1 ? "强制 h1.1" : "允许 h2";
            const int rcRange = doRequest(t, manifest, 0, 255, 20000, &resp, &body, forceH1);
            checkEq(rcRange, SXCL_NET_OK, tag);
            if (rcRange != SXCL_NET_OK)
                continue;
            const int status = resp.status;
            const int64_t total = resp.total_length;
            const int64_t rs = resp.range_start;
            const int64_t re = resp.range_end;
            int64_t got = 0;
            if (body) {
                got = readAll(t, body);
                t->close_body(t->ctx, body);
                body = nullptr;
            }
            std::printf("  [info] %s Range: status=%d 实收=%lld Content-Range=%lld-%lld/%lld\n", tag,
                        status, (long long)got, (long long)rs, (long long)re, (long long)total);
            checkEq(status, 206, "Range 请求返回 206(Range 必须被真正兑现)");
            checkEq(got, 256, "只收到请求的 256 字节");
        }
    } else {
        std::printf("  (跳过联网用例,设 SXCL_NET_ONLINE_TEST=1 可开启)\n");
    }

    t->destroy(t->ctx);
    std::printf("Qt 传输后端测试: 通过 %d 项, 失败 %d 项\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
