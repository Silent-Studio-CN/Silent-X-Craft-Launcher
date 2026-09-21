/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <mutex>

#include "sxcl/install.h"

class QThread;

namespace sxcl::ui {

// ── 一次安装请求(界面侧算好;worker 自己**不读控件、不弹窗**) ──

struct InstallRequest {
    QString gameDir;        // 游戏根目录(内含 versions/ libraries/ assets/)
    QString versionId;      // 要装的 Minecraft 版本号,如 "1.20.1"(去清单里找它)
    QString instanceName;   // 本地实例名(版本目录名);空 = 用 versionId
    QString loaderType;     // "none" / "forge" / "neoforge" / "fabric" / "optifine"
    QString loaderVersion;  // 加载器版本(给安装器用)
    QString javaPath;       // java 可执行路径(含加载器时必填:安装器要它)
    QString settingsFile;   // 下载参数(workers/限速/分片/缓存)从它读;空 = 不读
    QString assetsLevel;    // "default" / "none" / "index" / "full"
    int keepInstaller = 0;  // 1 = 留着安装器 jar(排查用)
};

// 一次进度事件(队列投递到界面线程的**值拷贝**,不含任何指针)
struct InstallProgress {
    int stageIndex = 0;      // 在本计划阶段表里的下标(从 0 起)
    int stageTotal = 0;      // 本计划的阶段总数
    QString stageId;         // "manifest"/"version_json"/…(稳定字符串名)
    QString stageName;       // 中文显示名("获取版本清单"/…)
    int stagePercent = 0;    // 本阶段 0..100
    int percent = 0;         // 整体 0..100(单调不减)
    qint64 bytesDone = 0;
    qint64 bytesTotal = 0;
    qint64 filesDone = 0;
    qint64 filesTotal = 0;
    qint64 filesSkipped = 0; // 已存在且校验通过、一个字节都没下的
    qint64 filesFailed = 0;
    double speedBps = 0.0;   // 本层从字节差算出的速度;<=0 = 算不出来
    qint64 etaSeconds = -1;  // 剩余秒数;-1 = 算不出来
    QString status;          // 核心库给的人话状态(永远非空)
    QString current;         // 当前文件(可空)
};

class InstallWorker : public QObject {
    Q_OBJECT
public:
    explicit InstallWorker(InstallRequest request, QObject *parent = nullptr);
    ~InstallWorker() override;

    // 起线程并跑。**立刻返回**,主线程不阻塞。重复调用无效果。
    void start();
    // 请求取消。**线程安全**,可以在界面线程直接调;核心库会在当前文件的边界上停下,
    // 清理未完成产物后以 SXCL_INSTALL_ERR_CANCELLED 收尾。
    void cancel();
    // 工作线程还在跑吗
    bool running() const;
    // 取消已请求(界面用来把按钮改成"正在取消…")
    bool cancelRequested() const { return m_cancel.load(); }

signals:
    // 计划就绪:本计划**实际会跑**的阶段(顺序即执行顺序)。
    // 传 QStringList 而不是自定义结构体,是为了不依赖元类型注册 —— 这两个是内置类型。
    void planReady(const QStringList &stageIds, const QStringList &stageNames);
    // 阶段切换(STAGE_BEGIN / STAGE_END);stageIndex 是本计划阶段表里的下标
    void stageChanged(int stageIndex, int stageTotal, const QString &stageId,
                      const QString &stageName);
    // 阶段内进度(已节流;取消/失败前的最后一次一定发得出去)
    void progress(const sxcl::ui::InstallProgress &update);
    // 一条日志(阶段起止 / 清理 / 每个落定的文件)
    void logLine(const QString &line);
    // 结束。三者互斥:
    //   ok=true              -> 装好了(code = SXCL_INSTALL_OK)
    //   cancelled=true       -> 用户取消(code = SXCL_INSTALL_ERR_CANCELLED),**不是成功**
    //   ok=false&&!cancelled -> 失败;message 是核心库给的人话原因(不是"失败"两个字)
    // detail 是给"展开看细节"用的补充(失败阶段 / 跳过文件数 / 字节数)。
    void finished(bool ok, bool cancelled, int code, bool retryable, const QString &message,
                  const QString &detail);

private:
    void run();  // 在工作线程里执行(阻塞到装完)

    // 核心库回调(userdata = this;**可能来自引擎的工作线程**)
    static void cbProgress(void *userdata, const sxcl_install_progress *progress);
    static int cbCancelled(void *userdata);
    void onProgress(const sxcl_install_progress *progress, bool force);
    void emitLog(const QString &line);

    InstallRequest m_request;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_started{false};
    QThread *m_thread = nullptr;

    // 节流 + 速度(回调可能来自多个线程,统一用一把锁保护)
    std::mutex m_mutex;
    qint64 m_lastEmitMs = 0;
    qint64 m_lastBytes = 0;
    qint64 m_lastBytesMs = 0;
    double m_speedBps = 0.0;
    int m_lastStage = -1;
    QStringList m_stageIds;
    QStringList m_stageNames;
};

} // namespace sxcl::ui

// 队列连接需要它(值拷贝语义的进度事件)
Q_DECLARE_METATYPE(sxcl::ui::InstallProgress)
