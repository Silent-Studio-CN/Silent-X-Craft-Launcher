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
#include <cstdint>

extern "C" {
struct sxcl_task; /* 只在两个静态回调的签名里用到(engine.h) */
}

class QThread;

namespace sxcl::ui {

// 一次启动请求(界面侧算好;身份字段与 CLI 的 --account 完全同义,见 tools/sxcl-dl/main.c:898-921)
struct LaunchRequest {
    QString gameDir;       // 游戏根目录
    QString versionName;   // 版本名(对应 versions/<名字>/<名字>.json)
    QString offlineName;   // 离线用户名;空 = 核心库默认 "Player"
    // 正版身份(都可空;给了 accessToken 就必须同时给 uuid + playerName,见 launch.h:462-470)
    QString playerName;
    QString uuid;
    QString accessToken;   // **真凭据,只在内存里传,绝不落日志/命令行**
    QString userType;
    QString xuid;
    QString clientId;
    QString javaPath;      // 空 = 让核心库探测
    int memoryMb = 0;      // <=0 = 核心库按位数取默认
    QString settingsFile;  // 读实例设置(渲染后端);空 = 不读
    QString backend;       // 渲染后端覆盖;空 = 读设置
    int dryRun = 0;        // 1 = 只准备,不起进程
    int timeoutMs = 0;     // <=0 = 不限时
};

class LaunchWorker : public QObject {
    Q_OBJECT
public:
    explicit LaunchWorker(LaunchRequest request, QObject *parent = nullptr);
    ~LaunchWorker() override;

    void start();            // 起线程并跑,立刻返回
    void cancel();           // 线程安全:置取消位 + **已知 PID 就直接结束它**(见文件头)
    bool running() const;
    bool cancelRequested() const { return m_cancel.load(); }

    // 游戏进程的 PID(还没起来 = <=0)。**进程结束后依然保留**,所以界面能用它核对
    // "崩的是不是那个进程"。值来自 on_started 回调,不遍历进程表、不按名字猜。
    int64_t runningPid() const { return m_pid.load(); }
    // 只结束**这个**子进程(启动器自己不受影响)。true = 终止请求已发出(不代表已死透)。
    // 没有 PID 时返回 false —— 调用方退回 on_line 那条路。
    bool killRunningProcess();

    // 页面上那 5 行阶段的名字(**只此一份**,页面照它画行)
    static QStringList phaseNames();

signals:
    // 阶段切换:index 是阶段下标(Python launch_page.py:608-626 的语义:上一阶段置完成、
    // 当前置进行中)
    void phaseChanged(int index, int total, const QString &name);
    // Java 探测结果(核心库选中的那一个)
    void javaInfo(const QString &path, int major, const QString &version, int is64Bit);
    // 启动前「补全文件」的实时读数(docs/24 的 P0)。
    //   finished = 已落定的件数(**含"已存在"命中的**),failed = 里面失败的件数,
    //   label = 正在处理的那一件,bytesDone/bytesTotal = 这一件的进度。
    // 由**下载引擎的工作线程**发出,界面按队列连接收(自动回到界面线程)。
    void completeProgress(int finished, int failed, const QString &label, qint64 bytesDone,
                          qint64 bytesTotal);
    // 最终命令行(**核心库打码后的**,逐行;见文件头)。accessToken 已在核心内部换成 ***。
    void commandLine(const QStringList &lines);
    // 进程输出的一行 + 核心库的归类结果(logscan)
    void logLine(const QString &text, const QString &kind, int severity, int isStderr);
    // 进程**真的起来了**:界面据此显示"运行中 PID xxx",并能单独结束它。
    void processStarted(qint64 pid);
    // 结束:
    //   ok=true  && dryRun=true  -> 准备完成,没起进程
    //   ok=true  && dryRun=false -> 进程正常退出(exitCode 0)
    //   ok=false && cancelled    -> 用户取消(核心库 killed_by_client)
    //   ok=false && !cancelled   -> 没起来 / 非 0 退出 / 超时;message 是真实原因
    void finished(bool ok, bool cancelled, bool dryRun, int exitCode, int timedOut,
                  int killedByClient, const QString &conclusionKey, const QString &message,
                  const QString &detail);

private:
    void run();
    static int cbLine(void *userdata, int is_stderr, const char *line);
    static int cbStarted(void *userdata, int64_t pid);
    static void cbCompleteProgress(void *userdata, const sxcl_task *task);
    static int cbCompleteCancelled(void *userdata);
    int onLine(int isStderr, const char *line);
    int onStarted(int64_t pid);

    LaunchRequest m_request;
    std::atomic<bool> m_cancel{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_started{false};
    std::atomic<bool> m_sawProcessOutput{false};
    // 当前这一遍是不是"收最终命令行"的那一遍(dry_run=1)。
    // **不能用 m_request.dryRun 代替**:那个是界面给的"本次只准备不起进程",
    // 真启动时它也是 0,会把第一遍的回调误当成游戏输出(实测踩过)。
    std::atomic<bool> m_collectingCommand{false};
    // 游戏进程 PID(on_started 回调写入;**进程结束后不重置** —— 见 runningPid 的说明)
    std::atomic<int64_t> m_pid{-1};
    QThread *m_thread = nullptr;
    QStringList m_pendingCommand; // 第一遍收下的命令行(只在工作线程里碰)
};

} // namespace sxcl::ui
