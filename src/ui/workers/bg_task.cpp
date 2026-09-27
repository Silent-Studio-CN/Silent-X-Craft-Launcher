/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 后台任务实现(设计理由见 bg_task.h)。

#include "bg_task.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <cstdio>
#include <utility>

namespace sxcl::ui {
namespace {

/* 析构等待的**硬上限**(用户 2026-09-27,父任务查到的根因)。
 *
 * 原样是"最多等 8 秒":设置页的 java-detect 在退出时还没跑完,退出路径就被按住 ——
 * 实测 select 路由退出卡 3104 ms 并落了一份 hang 报告(看门狗阈值 3 秒)。
 * 退出路径上**绝不允许**等一条"可能几十秒"的后台活,所以这里改成 300 ms 硬上限:
 *   * 到期就不等了 —— 线程自己跑完(QThread::create 的跑完即止形态,没有事件循环要 quit),
 *     对象脱手(setParent(nullptr) 之后不删它,免得删一个在跑的 QThread 直接终止进程);
 *   * 线程那边的回填本来就靠 QPointer 自查(self.isNull() 就不回填),页面没了它一个字都不写。 */
constexpr int kJoinTimeoutMs = 300;

} // namespace

BgTask::BgTask(QObject *owner) : QObject(owner), m_owner(owner) {}

BgTask::~BgTask() {
    cancel();
    if (m_thread == nullptr)
        return;
    if (!m_thread->isRunning()) { // 已经跑完了:直接收壳,不用等、也不用打读数
        delete m_thread;
        m_thread = nullptr;
        return;
    }
    const qint64 startedMs = QDateTime::currentMSecsSinceEpoch();
    /* 收尾:让工作线程自己跑完(它是 QThread::create 的"跑完就结束"形态,没有事件循环要 quit)。
     * 等不到就**不删**它 —— 删一个还在跑的 QThread 会让进程直接终止;
     * 工作线程那边靠 QPointer 自查,页面已经没了就不会再回填。 */
    const bool finished = m_thread->wait(kJoinTimeoutMs);
    const qint64 waitedMs = QDateTime::currentMSecsSinceEpoch() - startedMs;
    /* 验收读数(退出路径的耗时就是这么量的;SXCL_UI_TRACE 不是必需 —— 这一行只在退出路径
     * 真的要等的时候才出现,而且它自己就是"退出被后台活拖住多久"的数字)。 */
    std::fprintf(stderr, "[sxcl-ui] bg-task: 析构等待=%lldms 上限=%dms 线程结束=%s\n",
                 static_cast<long long>(waitedMs), kJoinTimeoutMs, finished ? "是" : "否(脱手)");
    if (!finished) {
        m_thread->setParent(nullptr); // 不随本对象删除(对象脱手,线程自己收尾)
        m_thread = nullptr;
        return;
    }
    delete m_thread;
    m_thread = nullptr;
}

bool BgTask::running() const { return m_thread != nullptr && m_thread->isRunning(); }

bool BgTask::cancelled() const { return m_state != nullptr && m_state->cancelled.load(); }

void BgTask::cancel() {
    if (m_state != nullptr)
        m_state->cancelled.store(true);
}

void BgTask::start(const QString &label, std::function<void()> work, std::function<void()> done) {
    if (running())
        return;
    if (m_thread != nullptr) { // 上一轮已经跑完:把它的壳收掉(对象本身可以接着用)
        delete m_thread;
        m_thread = nullptr;
    }
    m_state = std::make_shared<State>();
    const std::shared_ptr<State> state = m_state;
    const QPointer<BgTask> self(this);
    const QString tag = label.isEmpty() ? QStringLiteral("(未命名)") : label;
    m_thread = QThread::create([self, state, tag, work = std::move(work), done = std::move(done)] {
        /* 取证(用户报"点一下未响应"时的判据):这条活真的不在界面线程上。
         * 界面线程 = QCoreApplication 所在线程;两者不同才算走对了。 */
        QThread *gui = QCoreApplication::instance() != nullptr
                           ? QCoreApplication::instance()->thread()
                           : nullptr;
        std::fprintf(stderr, "[sxcl-ui] bg-task[%s]: 工作线程=%p 界面线程=%p 同一条线程=%s\n",
                     tag.toUtf8().constData(), (void *)QThread::currentThread(), (void *)gui,
                     QThread::currentThread() == gui ? "是(不对!)" : "否");
        work();
        if (state->cancelled.load() || self.isNull())
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, done] {
                if (self.isNull())
                    return;
                done();
            },
            Qt::QueuedConnection);
    });
    m_thread->start();
}

} // namespace sxcl::ui
