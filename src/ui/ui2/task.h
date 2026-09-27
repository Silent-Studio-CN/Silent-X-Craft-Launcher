/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 任务原语(docs/27 §6 的 task/):**阻塞活(网络 / 文件)只许走它**,结果由框架回到界面线程。
//
// 为什么必须有它(而不是页面里各写一遍 QThread::create):
//   * 页面里直接调阻塞函数 = 界面线程停摆,用户看到的就是"未响应"(docs/27 §14 那一轮的血);
//   * 生命周期那几行最容易写漏:页面已经析构、工作线程还在回填 -> 退出时崩。
//     这里收成一处:**析构先置取消位,再回收线程**(等不到就不删,交给进程退出)。
//
// 两条判据(每次任务起跑都打,ASCII 可 grep):
//   [sxcl-ui2] task[home-versions]: kind=fs 工作线程id=0x... 界面线程id=0x... 同一条线程=否
//   [sxcl-ui2] task[home-versions]: 回填 done 线程id=0x... 是界面线程=是
//
// 页面纪律(本轮硬要求,验收脚本按 grep 断言):pages/** 里不许出现
//   sxcl_http_get_text / sxcl_manifest_* / sxcl_instance_list / QFile ——
//   阻塞源只住在 sources.*(非页面)里,而且只许在 Task 的工作线程里调
//   (sources 自己用 requireWorkerThread() 记违规行,验收脚本断言这一行**一次都不出现**)。
#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class QThread; // 全局的 QThread(写进 sxcl::ui2 里会变成另一个类型)

namespace sxcl::ui2 {

class Task : public QObject {
public:
    enum class Kind { Net, Fs };

    using Work = std::function<void()>; // 工作线程:不许碰任何 QWidget / QPixmap
    using Done = std::function<void()>; // 界面线程:回填控件

    explicit Task(QObject *owner, QString label, Kind kind = Kind::Fs);
    ~Task() override;
    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    /** 起跑(一次性)。已在跑 / 已经跑过都返回 false。 */
    bool start(Work work, Done done);

    /** 便捷入口:建任务 -> 起跑 -> 跑完自删。owner 空 = 挂到 qApp。 */
    static Task *net(QObject *owner, const QString &label, Work work, Done done);
    static Task *fs(QObject *owner, const QString &label, Work work, Done done);

    bool running() const;
    /** 工作线程里问一句"还要不要继续"(页面已经走了就别再算了)。 */
    bool cancelled() const;
    /** 界面线程置取消位(线程安全;析构里也是先走这一步,再回收线程)。 */
    void cancel();

    static bool onGuiThread();

private:
    struct State {
        std::atomic<bool> cancelled{false};
        std::atomic<bool> doneQueued{false};
    };

    static Task *run(QObject *owner, const QString &label, Kind kind, Work work, Done done);

    QString m_label;
    Kind m_kind;
    std::shared_ptr<State> m_state;
    QThread *m_thread = nullptr;
    bool m_selfDelete = false; /**< 一次性入口(net/fs):done 跑完把自己删掉 */
};

/** 阻塞源(docs/27 §14 点名的那几条)自己调一句:在**界面线程**上调用 = 违规。
 *  违规只记一行 + 置进程级计数(验收脚本断言 count=0),不假装成功、也不静默。 */
void requireWorkerThread(const char *who);
/** 违规计数(SXCL_UI_DUMP 的最后一行会打出来)。 */
int mainThreadViolations();

} // namespace sxcl::ui2
