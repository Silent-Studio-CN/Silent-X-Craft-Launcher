/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 模组页的两件事，都在**工作线程**里做（界面线程一个字节都不等）：
//   * 取文本：Modrinth 的搜索/版本 JSON（走核心库的 sxcl_install_http_get_text，
//     与我们自己的引擎同一套 DNS/UA/超时口径）；
//   * 下文件：把挑中的那个 jar 下到 <实例>/mods（带官方 sha1 强校验 + "已存在就跳过"）。
//
// 为什么单独一个 worker 而不是塞进 install_worker：模组下载**不碰安装编排**
// （没有阶段表、没有 natives、没有版本 JSON），硬塞进去只会把那条已经很长的链子搅浑。
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QThread;

namespace sxcl::ui {

class ModsWorker : public QObject {
    Q_OBJECT
public:
    enum Op { FetchText, DownloadFile };

    struct Request {
        Op op = FetchText;
        QString url;          // FetchText / DownloadFile 的地址
        QString dest;         // DownloadFile 落到哪
        QString sha1;         // 可空:官方摘要(有就强校验)
        qint64 size = 0;      // 0 = 不知道
        QString settingsFile; // 从它读下载参数(workers/限速/分片/缓存);空 = 默认
        // 额外请求头("名字: 值"),只对 FetchText 有意义。
        // **CurseForge 的 key 只走这里** —— 它必须放在 x-api-key 头里,绝不进 URL
        // (URL 会被写进日志/历史/错误消息,key 一旦进去就等于泄露)。
        QStringList headers;
    };

    explicit ModsWorker(Request request, QObject *parent = nullptr);
    ~ModsWorker() override;

    void start();                 // 起线程就返回
    bool running() const;
    void cancel();                // 线程安全

signals:
    // text 只在 FetchText 时有意义;bytes 只在 DownloadFile 时有意义
    void finished(bool ok, const QString &error, const QString &text, qint64 bytes);

private slots:
    void run();

private:
    QThread *m_thread = nullptr;
    Request m_request;
    class Impl;
};

} // namespace sxcl::ui
