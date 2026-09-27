/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "skin_store.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QThread>

#include <cstdlib>
#include <cstring>
#include <memory>

#include "workers/ui_paths.h"

#include "sxcl/http.h"

namespace sxcl::ui {

/* 取消位 + 在飞的那条传输。requestStop() 从界面线程调:
 * 先置位(工作线程在两个请求之间会看到),再 cancel_all 把**正在等响应**的那一条立刻撤掉 ——
 * 只置位的话,退出时界面线程还要等它把 10 秒超时走完。 */
struct SkinFetchCancel {
    std::atomic<bool> stopped{false};
    QMutex mutex;
    sxcl_transport *transport = nullptr;

    void requestStop() {
        stopped.store(true);
        QMutexLocker locker(&mutex);
        if (transport != nullptr)
            transport->cancel_all(transport->ctx);
    }
};

namespace {

const char *kProfileUrlPrefix = "https://sessionserver.mojang.com/session/minecraft/profile/";

/* 档案接口要的是**不带横线**的 UUID。账户里存的可能是带横线的形式,统一去掉。 */
QString bareUuid(const QString &uuid) {
    QString out = uuid;
    out.remove(QLatin1Char('-'));
    return out;
}

/* 一次 GET:返回正文(二进制安全)。失败返回空。 */
QByteArray httpGet(sxcl_transport *transport, const QString &url) {
    if (transport == nullptr || url.isEmpty())
        return QByteArray();
    const QByteArray urlBytes = url.toUtf8();
    sxcl_http_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.max_bytes = 4u * 1024u * 1024u; // 皮肤与档案都远小于这个数
    opts.timeout_ms = 10000;
    char *out = nullptr;
    size_t outLen = 0;
    char err[SXCL_HTTP_ERROR_MAX];
    err[0] = '\0';
    const int rc = sxcl_http_get_text_ex(transport, urlBytes.constData(), nullptr, &opts, &out,
                                         &outLen, err, sizeof(err));
    if (rc != SXCL_HTTP_OK || out == nullptr) {
        if (out != nullptr)
            free(out);
        return QByteArray();
    }
    const QByteArray body(out, int(outLen));
    free(out);
    return body;
}

/* 档案 -> 皮肤 PNG 的地址。没有 Java 版档案 / 没有皮肤 = 空串(不是错误,界面画占位)。 */
QString skinUrlFromProfile(const QByteArray &profileJson) {
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(profileJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return QString();
    const QJsonArray properties = doc.object().value(QStringLiteral("properties")).toArray();
    for (const QJsonValue &value : properties) {
        const QJsonObject property = value.toObject();
        if (property.value(QStringLiteral("name")).toString() != QLatin1String("textures"))
            continue;
        const QByteArray decoded = QByteArray::fromBase64(
            property.value(QStringLiteral("value")).toString().toLatin1());
        QJsonParseError innerError{};
        const QJsonDocument inner = QJsonDocument::fromJson(decoded, &innerError);
        if (innerError.error != QJsonParseError::NoError)
            return QString();
        const QJsonObject skin =
            inner.object().value(QStringLiteral("textures")).toObject().value(QStringLiteral("SKIN")).toObject();
        return skin.value(QStringLiteral("url")).toString();
    }
    return QString();
}

/* 取证通路(默认不设 = 走真网络):SXCL_UI_SKIN_FILE=<png> —— 直接把本地这张贴图当成
 * "刚从会话服务取回来的皮肤"。为什么要有它:皮肤那条链**既要联网、又要一个真登录过的账户**,
 * 验收机器上两样都不保证有;这条通路只换"图从哪来",后面拼头像 / 拼全身像的代码一行不改
 * (与 dialogs/auth_replay.c 是同一条口径:换来源,不换逻辑)。 */
QString localSkinOverride() { return qEnvironmentVariable("SXCL_UI_SKIN_FILE"); }

/* 走完整条链。**只许在工作线程调**(它自己建/销毁 transport)。 */
QImage fetchSkin(const QString &uuid, const QString &cachePath, SkinFetchCancel *cancel) {
    // ⓪ 取证通路:本地那张贴图(设了它就是它,不再碰网络与缓存)
    const QString local = localSkinOverride();
    if (!local.isEmpty()) {
        QImage image(local);
        return image;
    }
    // ① 缓存:断网也要能出图,所以先看磁盘
    if (QFileInfo::exists(cachePath)) {
        QImage cached(cachePath);
        if (!cached.isNull())
            return cached;
    }
    if (cancel->stopped.load())
        return QImage();

#if defined(SXCL_UI_HAVE_QT_TRANSPORT)
    sxcl_transport_qt_bootstrap();
    sxcl_transport *transport = sxcl_transport_qt_create();
#else
    sxcl_transport *transport = nullptr;
#endif
    if (transport == nullptr)
        return QImage();
    { // 登记"在飞的那条":界面线程的 cancelFetch() 才能把它撤掉(线程安全)
        QMutexLocker locker(&cancel->mutex);
        cancel->transport = transport;
    }

    const QString profileUrl = QString::fromLatin1(kProfileUrlPrefix) + bareUuid(uuid);
    const QByteArray profile = httpGet(transport, profileUrl);
    if (cancel->stopped.load()) {
        QMutexLocker locker(&cancel->mutex);
        cancel->transport = nullptr;
        transport->destroy(transport->ctx);
        return QImage();
    }
    const QString skinUrl = skinUrlFromProfile(profile);
    const QByteArray png = skinUrl.isEmpty() ? QByteArray() : httpGet(transport, skinUrl);
    {
        QMutexLocker locker(&cancel->mutex);
        cancel->transport = nullptr;
    }
    transport->destroy(transport->ctx);

    if (png.isEmpty())
        return QImage();
    QImage image;
    if (!image.loadFromData(png, "PNG"))
        return QImage();
    if (!cachePath.isEmpty()) {
        QDir().mkpath(QFileInfo(cachePath).absolutePath());
        image.save(cachePath, "PNG"); // 存不下来也不影响这次显示
    }
    return image;
}

} // namespace


QString skinCachePath(const QString &uuid) {
    const QString settings = uiSettingsFilePath();
    if (settings.isEmpty() || uuid.isEmpty())
        return QString();
    return QFileInfo(settings).absolutePath() + QStringLiteral("/skin-cache/") + bareUuid(uuid) +
           QStringLiteral(".png");
}

SkinStore &SkinStore::instance() {
    static SkinStore store;
    return store;
}

SkinStore::SkinStore() = default;

SkinStore::~SkinStore() {
    cancelFetch();
    if (m_thread != nullptr) {
        // 有界等待(与 account.cpp 同一个口径):超时就脱手,绝不让退出卡在网络上。
        if (m_thread->wait(3000))
            delete m_thread;
        else
            m_thread->setParent(nullptr);
        m_thread = nullptr;
    }
}

void SkinStore::setAccount(const QString &uuid) {
    /* 取证通路那一份(SXCL_UI_SKIN_FILE):没登录也要能把它画出来 —— 给它一个占位账户名,
     * 别的路径(skinCachePath / 网络)都走不到(见 fetchSkin 的 ⓪)。 */
    const QString effective =
        (!uuid.isEmpty() || localSkinOverride().isEmpty()) ? uuid : QStringLiteral("local-skin");
    if (effective == m_uuid && (!m_skin.isNull() || m_thread != nullptr))
        return;
    m_uuid = effective;
    m_skin = QImage();
    emit changed(); // 换人先清屏:旧账户的脸绝不能停在新账户上
    if (!effective.isEmpty())
        start(effective);
}

void SkinStore::cancelFetch() {
    if (m_cancel)
        m_cancel->requestStop();
}

void SkinStore::start(const QString &uuid) {
    cancelFetch(); // 上一次(如果有)作废:置位 + 撤掉它在飞的请求
    if (m_thread != nullptr) {
        // 不在这里等它:它有 cancel 位,自己会很快收尾(transport 那头有取消)。
        m_thread->setParent(nullptr);
        connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
        m_thread = nullptr;
    }
    auto cancel = std::make_shared<SkinFetchCancel>();
    m_cancel = cancel;
    const QString cachePath = skinCachePath(uuid);
    /* 线程可能在窗口关掉之后才回来:结果投递前先看 this 还在不在(QPointer 在 QObject 析构时
     * 自动置空),否则就是往一个已经没了的对象上投。 */
    QPointer<SkinStore> guard(this);
    QThread *thread = QThread::create([guard, uuid, cachePath, cancel] {
        const QImage image = fetchSkin(uuid, cachePath, cancel.get());
        if (cancel->stopped.load() || image.isNull() || guard.isNull())
            return;
        QMetaObject::invokeMethod(
            guard.data(),
            [guard, uuid, image] {
                if (!guard.isNull())
                    guard->deliver(uuid, image);
            },
            Qt::QueuedConnection);
    });
    m_thread = thread;
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread] {
        if (m_thread == thread)
            m_thread = nullptr;
    });
    thread->start();
}

void SkinStore::deliver(const QString &uuid, const QImage &skin) {
    if (uuid != m_uuid)
        return; // 已经换人了:这一份不要了
    m_skin = skin;
    emit changed();
}

} // namespace sxcl::ui
