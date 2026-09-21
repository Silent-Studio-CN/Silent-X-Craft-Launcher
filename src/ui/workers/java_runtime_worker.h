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

#include "sxcl/java_runtime.h"

#include "install_worker.h" // sxcl::ui::InstallProgress(**同一套事件**,界面只用认一种)

class QThread;

namespace sxcl::ui {

// 一次「预置」(一次装齐官方 JRE)的请求。
struct JavaPresetRequest {
    QStringList mcVersions;   // 想玩的 MC 版本(空 = 基线 8/17/21 + 清单里最新的 25)
    bool includeNewest = true;
    bool force = false;       // true = 已装好的也重装
    QString platform;         // 空 = 本机
    QString targetRoot;       // 空 = 核心库的默认 runtime 根(测试/取证可重定向)
    QString settingsFile;     // 下载参数(workers/限速/分片/缓存);空 = 默认设置文件
};

// 把阻塞的 sxcl_java_runtime_install_preset 放到工作线程:
//   * 先出计划(要装什么/多大/装过没)并报给界面 —— 用户要求「装之前先算大小」;
//   * 再逐个组件装,进度沿用 install_worker 的那一套事件(阶段/百分比/速度/剩余);
//   * 取消是**组件边界 + 文件边界**上生效的(核心库的 is_cancelled)。
// 线程模型与 InstallWorker 逐条一致:QThread 不给 parent、析构里 quit+wait+delete。
class JavaRuntimeWorker : public QObject {
    Q_OBJECT
public:
    explicit JavaRuntimeWorker(JavaPresetRequest request, QObject *parent = nullptr);
    ~JavaRuntimeWorker() override;

    // 起线程并跑。**立刻返回**,主线程不阻塞。重复调用无效果。
    void start();
    // 请求取消。**线程安全**,可在界面线程直接调。
    void cancel();
    bool running() const;
    bool cancelRequested() const { return m_cancel.load(); }

signals:
    // 计划就绪:components[i] 与 labels[i] 一一对应(label 里带主版本/大小/是否已装)。
    void planReady(const QStringList &components, const QStringList &labels);
    // 开始装第 index 个组件(0 起);skipped = 已装好、整个跳过
    void componentChanged(int index, int total, const QString &component, const QString &version,
                          bool skipped);
    // 阶段内进度(已节流;与 InstallWorker 同一个结构、同一个口径)
    void progress(const sxcl::ui::InstallProgress &update);
    // 一条日志(阶段起止 / 每个组件的结果)
    void logLine(const QString &line);
    // 结束。ok / cancelled / 失败 三者互斥;message 是人话原因。
    void finished(bool ok, bool cancelled, int code, const QString &message, const QString &detail);

private:
    void run(); // 在工作线程里执行(阻塞到装完)

    static void cbProgress(void *ud, const sxcl_java_runtime_progress *progress);
    static int cbCancelled(void *ud);
    void onProgress(const sxcl_java_runtime_progress *progress, bool force);
    void emitLog(const QString &line);

    JavaPresetRequest m_request;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_started{false};
    QThread *m_thread = nullptr;

    mutable std::mutex m_mutex;
    qint64 m_lastEmitMs = 0;
    qint64 m_lastBytes = 0;
    qint64 m_lastBytesMs = 0;
    double m_speedBps = 0.0;
    int m_lastComponent = -1;
    int m_lastStage = -1;
};

// ── 进程内「正在跑的预置」 ──
// 首启自动预置与设置页手动预置**共用同一条**:界面任何一处都能问"在跑吗"、都能取消。
// 只归界面线程用(不要在工作线程里调)。
JavaRuntimeWorker *activeJavaPreset();
void setActiveJavaPreset(JavaRuntimeWorker *worker);
bool cancelActiveJavaPreset();

} // namespace sxcl::ui
