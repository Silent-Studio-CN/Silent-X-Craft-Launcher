/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 一次性后台任务:把一段**阻塞**代码放到工作线程,结果回**界面线程**。
//
// 为什么要有它(而不是每页各写一遍 QThread::create):本仓库已经有四处手写的
// "起线程 -> QMetaObject::invokeMethod 回界面"(versions_page / download_config_page /
// account / workers),再抄第五第六遍时最容易写漏的是**生命周期那几行** ——
// 页面已经析构了、工作线程还在回填,就是"退出时崩"。这里把这几行收成一处:
//
//   auto *task = new BgTask(this);                     // parent = 页面(界面线程对象)
//   task->start([this] { m_java = discoverJava(); },   // 工作线程:不许碰任何控件
//               [this] { fillCards(m_java); });        // 界面线程:回填控件
//
// 三条口径(与 workers/mods_worker 同一套):
//   * work 里**绝不允许**碰 QWidget / QPixmap / QPixmap 缓存 —— 那是界面线程的东西;
//   * done 一定在 owner 所在线程(界面线程)里跑,且一定在 work 返回之后(队列连接);
//   * BgTask 挂在页面上:页面析构时先置取消位、再等线程收尾(最多 8 秒),不让工作线程
//     回过一个已经没有的页面。
#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class QThread; // **全局的** QThread(写进 sxcl::ui 里会变成另一个类型,实测报了一串 C2027)

namespace sxcl::ui {

class BgTask : public QObject {
public:
    explicit BgTask(QObject *owner);
    ~BgTask() override;

    /** 起一个工作线程跑 work,完事后把 done 排回界面线程。已在跑时这次调用被忽略
     *  (页面自己的"加载中"状态会挡住重复点击,这里再兜一道)。
     *  label 只进取证输出:每个任务起跑时打一行"工作线程 vs 界面线程",用户报
     *  "点一下未响应"时,这一行就是"这条活到底在不在界面线程上"的判据。 */
    void start(const QString &label, std::function<void()> work, std::function<void()> done);

    bool running() const;
    /** 工作线程里用它问一句"还要不要继续"(页面已经走了就别再算了)。 */
    bool cancelled() const;
    void cancel();

private:
    struct State {
        std::atomic<bool> cancelled{false};
    };

    QObject *m_owner = nullptr;
    std::shared_ptr<State> m_state;
    QThread *m_thread = nullptr;
};

} // namespace sxcl::ui
