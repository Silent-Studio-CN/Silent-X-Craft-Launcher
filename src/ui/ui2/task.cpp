/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 任务原语实现(设计理由见 task.h)。

#include "task.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <utility>

namespace sxcl::ui2 {
namespace {

std::atomic<int> g_mainThreadViolations{0};

QThread *guiThread() {
    return QCoreApplication::instance() != nullptr ? QCoreApplication::instance()->thread()
                                                   : nullptr;
}

/** 线程 id(判据行里那个"工作线程 id 与界面线程 id 不同"的 id)。
 *  Windows 的 Qt::HANDLE 是 void*,POSIX 那边是整型 —— 两条路各转各的。 */
quintptr currentThreadIdValue() {
    const Qt::HANDLE handle = QThread::currentThreadId();
#if defined(Q_OS_WIN)
    return reinterpret_cast<quintptr>(handle);
#else
    return static_cast<quintptr>(handle);
#endif
}

const char *kindName(bool net) { return net ? "net" : "fs"; }

} // namespace

bool Task::onGuiThread() { return QThread::currentThread() == guiThread(); }

void requireWorkerThread(const char *who) {
    if (!Task::onGuiThread())
        return;
    g_mainThreadViolations.fetch_add(1);
    std::fprintf(stderr,
                 "[sxcl-ui2] MAIN-THREAD-VIOLATION[%s]: 阻塞源被界面线程直接调用了(必须走 "
                 "Task,验收脚本断言这一行 count=0)\n",
                 who != nullptr ? who : "?");
}

int mainThreadViolations() { return g_mainThreadViolations.load(); }

Task::Task(QObject *owner, QString label, Kind kind)
    : QObject(owner), m_label(std::move(label)), m_kind(kind) {}

Task::~Task() {
    cancel(); // 1) 先置取消位(工作线程下一次问 cancelled() 就会看到)
    if (m_thread == nullptr)
        return;
    /* 2) 再回收线程。等不到就**不删** —— 删一个还在跑的 QThread 会让进程直接终止;
     * 工作线程那边靠 QPointer 自查,owner 已经没了就不会再回填(与 workers/bg_task 同一条纪律)。 */
    if (!m_thread->wait(8000)) {
        std::fprintf(stderr,
                     "[sxcl-ui2] task[%s]: 工作线程 8 秒内没结束(不回填,进程退出时一并收掉)\n",
                     m_label.toUtf8().constData());
        m_thread->setParent(nullptr);
        m_thread = nullptr;
        return;
    }
    delete m_thread;
    m_thread = nullptr;
}

bool Task::running() const { return m_thread != nullptr && m_thread->isRunning(); }

bool Task::cancelled() const { return m_state != nullptr && m_state->cancelled.load(); }

void Task::cancel() {
    if (m_state != nullptr)
        m_state->cancelled.store(true);
}

bool Task::start(Work work, Done done) {
    if (running() || m_thread != nullptr)
        return false; // 一次性
    m_state = std::make_shared<State>();
    const std::shared_ptr<State> state = m_state;
    const QPointer<Task> self(this);
    const QString tag = m_label.isEmpty() ? QStringLiteral("(未命名)") : m_label;
    const bool net = (m_kind == Kind::Net);
    const unsigned long long guiId = static_cast<unsigned long long>(currentThreadIdValue());
    const bool selfDelete = m_selfDelete;
    m_thread = QThread::create([self, state, tag, net, guiId, selfDelete, work = std::move(work),
                               done = std::move(done)] {
        /* 判据一:这条活真的不在界面线程上(用户报"点一下未响应"时就看这一行)。 */
        const unsigned long long workId =
            static_cast<unsigned long long>(currentThreadIdValue());
        /* 判据行(ASCII 可 grep,验收脚本就按 work_thread_id / gui_thread_id / same_thread 断言):
         * 这条活真的不在界面线程上 —— 用户报"点一下未响应"时,先看这一行。 */
        std::fprintf(stderr,
                     "[sxcl-ui2] task[%s]: kind=%s work_thread_id=0x%llx gui_thread_id=0x%llx "
                     "same_thread=%s(工作线程 id 与界面线程 id %s)\n",
                     tag.toUtf8().constData(), kindName(net), workId, guiId,
                     workId == guiId ? "yes" : "no",
                     workId == guiId ? "相同,走错了!)" : "不同,阻塞活确实在别的线程上");
        work();
        if (state->cancelled.load() || self.isNull()) {
            std::fprintf(stderr, "[sxcl-ui2] task[%s]: 已取消/宿主已析构,不回填\n",
                         tag.toUtf8().constData());
            return;
        }
        if (state->doneQueued.exchange(true))
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [self, done, tag, selfDelete, guiId] {
                if (self.isNull())
                    return;
                /* 判据二:回填真的落在界面线程上(不是工作线程直接改控件)。 */
                const unsigned long long here =
                    static_cast<unsigned long long>(currentThreadIdValue());
                std::fprintf(stderr,
                             "[sxcl-ui2] task[%s]: done_on_gui_thread=%s thread_id=0x%llx"
                             "(回填 %s界面线程)\n",
                             tag.toUtf8().constData(), here == guiId ? "yes" : "no", here,
                             here == guiId ? "落在" : "**不在**");
                done();
                if (selfDelete && !self.isNull())
                    self->deleteLater();
            },
            Qt::QueuedConnection);
    });
    m_thread->start();
    return true;
}

Task *Task::net(QObject *owner, const QString &label, Work work, Done done) {
    return run(owner, label, Kind::Net, std::move(work), std::move(done));
}

Task *Task::fs(QObject *owner, const QString &label, Work work, Done done) {
    return run(owner, label, Kind::Fs, std::move(work), std::move(done));
}

Task *Task::run(QObject *owner, const QString &label, Kind kind, Work work, Done done) {
    auto *task = new Task(owner, label, kind);
    task->m_selfDelete = true; // 一次性入口:跑完(done 之后)自删
    if (!task->start(std::move(work), std::move(done))) {
        delete task;
        return nullptr;
    }
    return task;
}

} // namespace sxcl::ui2
