/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 后台任务实现(设计理由见 bg_task.h)。

#include "bg_task.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <cstdio>
#include <utility>

namespace sxcl::ui {

BgTask::BgTask(QObject *owner) : QObject(owner), m_owner(owner) {}

BgTask::~BgTask() {
    cancel();
    if (m_thread == nullptr)
        return;
    /* 收尾:让工作线程自己跑完(它是 QThread::create 的"跑完就结束"形态,没有事件循环要 quit)。
     * 等不到就**不删**它 —— 删一个还在跑的 QThread 会让进程直接终止;
     * 工作线程那边靠 QPointer 自查,页面已经没了就不会再回填。 */
    if (!m_thread->wait(8000)) {
        std::fprintf(stderr, "[sxcl-ui] BgTask: 工作线程 8 秒内没结束(不回填,进程退出时一并收掉)\n");
        m_thread->setParent(nullptr); // 不随本对象删除
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
