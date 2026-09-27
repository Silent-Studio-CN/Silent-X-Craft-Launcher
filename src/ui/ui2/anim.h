/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

/* 新界面的动画基座(docs/27-UI重写方案.md §10.2 / §10.2.1 / §13.1)。纯 Qt + 我们自己的纯 C 核心,
 * 不依赖 libqf —— 新界面任何一层都能用。
 *
 * §13.1 那六条就落在这个文件里(每一条都能在 CI 里断言):
 *   1. 时间线编排:Anim::to(对象, 属性, 终值, 时长).then(下一段).after(ms).parallel(组)
 *      —— "先淡入、再抬升、最后出图标"是一句话,页面里不许回调套回调;
 *   2. 自家缓动表:Ease::at(Ease::Out, p) 是全站唯一的曲线来源(形状自己调,公式与推导见 anim.cpp);
 *      页面只写 Ease::Out / Ease::Back 这种语义名,既不写数字,也不碰 Qt 的 QEasingCurve 枚举;
 *   3. 列表错峰模板:Anim::stagger(rows, 100, 25) —— 一行行冒出来,而且**一条时间线**驱动所有行;
 *   4. 全局动效开关:Motion(设置键 ui.motion = follow|standard|reduced|off),
 *      生效档位 = min(设置, 系统无障碍) —— 与系统取**更严**的那个;
 *   5. 性能纪律:全局只有一个心跳 QTimer(Motion::heartbeatCount() 恒为 1,可查),
 *      一条错峰时间线 = 一个对象(Anim::trackCount() 条轨道,不是 N 个动画对象);
 *      动画只写**属性**(自绘控件在自己的 paintEvent 里读),不碰布局、不 setGeometry;
 *   6. 可断言:进度/每段起始偏移/帧数/时间线个数全部可查(见 Anim::progress / offsetsMs
 *      与 Motion::frameCount / timelinesAlive),真机截图与像素扫描见 tools/ui2_anim.ps1。
 *
 * 用法(页面里就这一行,没有回调):
 *   Anim::to(card, "reveal", 1.0, 180).then(Anim::to(card, "lift", 0.0, 120)).after(60)
 *       .parallel(Anim::to(icon, "reveal", 1.0, 120)).start();
 *   Anim::stagger(rows, 100, 25).start();   // 默认驱动行的 "reveal"(0 -> 1)
 */

#include <QByteArray>
#include <QObject>
#include <QSharedPointer>
#include <QVariant>
#include <QVector>

#include <functional>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace sxcl::ui2 {

/* ── 1) 语义曲线名 + 全站唯一的曲线表 ──────────────────────────────────────
 * 页面只写语义名(Ease::Out),形状在 anim.cpp 一张表里(自己定的公式,不是搬别人的参数)。
 * 约定(§4):过渡 150–220ms、不用弹跳、缩放不超过 1.02 —— 所以 Back 的过冲峰值被钉在 1.02 以内。 */
struct Ease {
    enum Name {
        Out = 0,     /* 出场(默认):快起慢收,初速 3、末速 0 */
        In,          /* 入场:慢起快收(Out 的镜像) */
        InOut,       /* 两端缓:位移类的默认手感 */
        FluentOut,   /* 出 Fluent:尾巴更长、收得更稳(外壳折叠 / 大块位移) */
        FluentIn,    /* 进 Fluent:起步更软 */
        InOutFluent, /* 进出一对 Fluent:两头都带劲,中段最快 */
        Back,        /* 出场带一点点过冲再回位(峰值 1.018,不弹跳;只给位置/缩放类用) */
        BackIn,      /* 入场:从负方向滑进来(只给位置类用,别给不透明度用) */
        Glide,       /* "按初速那条"的无参形态(拖拽/惯性/跟手);要精确参数用 Ease::glide() */
    };

    /** 曲线求值:progress ∈ [0,1] -> [0,1](Back 系允许到 1.02,BackIn 允许到 -0.02)。
     *  纯函数,无状态;每帧每轨道调一次,不分配内存。 */
    static double at(Name name, double progress);

    /** "按初速那条"曲线:初速 v0(单位/秒)+ 总距离 distance + 总时长 ms -> 归一化曲线。
     *  归一化初速 = v0 × (ms/1000) / distance:甩得越快越陡、尾巴越长;v0 <= 0 退化成匀速。 */
    static double glide(double v0, double distance, int ms, double progress);

    /** 语义名(打日志/断言用,稳定字符串)。 */
    static const char *name(Name name);
};

/* ── 2) 全局动效档位(§13.1 第 4 条) ────────────────────────────────────────
 * 设置键 ui.motion,四个值:
 *   follow   跟随系统(默认)
 *   standard 标准
 *   reduced  减弱   —— 时长 ×0.35、错峰延迟归零(去位移、去排队,保留最短的淡入)
 *   off      关闭   —— 终值立即就位,0 帧动画
 * 生效档位 = min(设置, 系统无障碍的"减少动态效果"):系统说弱就弱,设置说关就关。
 *
 * 验证用的环境变量(与仓库其它验收同一个口径,不进产品逻辑):
 *   SXCL_UI_MOTION         = follow|standard|reduced|off   覆盖设置值
 *   SXCL_UI_MOTION_SYSTEM  = standard|reduced              钉死"系统无障碍"那一侧
 *   SXCL_UI2_ANIM_TRACE    = 1                             每帧往 stderr 打一行进度与各轨道值
 *   SXCL_UI_SETTINGS       = <文件>                        设置文件重定向(核心库一直在用的键) */
class Motion {
public:
    enum Level { Off = 0, Reduced = 1, Standard = 2 };

    /** 设置键("ui.motion")。 */
    static const char *key();
    /** 设置里的原始值(follow|standard|reduced|off;读不到就是默认 follow)。 */
    static const char *settingValue();
    /** 生效档位(设置与系统取更严的那个)。第一次调用会自动 refresh()。 */
    static Level level();
    /** 重新读设置 + 系统无障碍(设置页改完动效开关后调它)。 */
    static void refresh();
    /** 档位的稳定文本(打日志/断言用)。 */
    static const char *levelName(Level level);

    /* ── 心跳与计数:性能纪律与验收的"数字说话"入口 ── */
    /** 全局心跳 QTimer 被**创建过**几个 —— 必须恒为 1(所有动画共用一个心跳)。 */
    static int heartbeatCount();
    /** 心跳当前是否在跑(没动画时停掉,省电;停掉不等于销毁)。 */
    static bool heartbeatActive();
    /** 当前活着的时间线个数(错峰模板无论多少行都只算 1 条)。 */
    static int timelinesAlive();
    /** 心跳累计帧数(motion=off 时不动,可用来断言"0 帧")。 */
    static qint64 frameCount();
    /** 单调毫秒时钟(心跳、时间线、测试共用一个来源)。 */
    static qint64 nowMs();
    /** 测试/验收钉子:换成假时钟(传空 function 恢复真实时钟)。假钟下用 pump() 手动走帧。 */
    static void setClockForTest(std::function<qint64()> clock);
    /** 走一帧:心跳定时器与测试**共用**的唯一入口(tick 所有时间线 + 帧计数 + 空闲停表)。 */
    static void pump();
};

/* ── 3) 时间线 ──────────────────────────────────────────────────────────────
 * Anim 是"段表"的句柄(拷贝共享同一份段表);段与段之间由 then/after/parallel 编排。
 * 语义:
 *   then(next)     把 next 接在**当前末尾**之后(串行)
 *   after(ms)      在当前末尾插一段**等待**,之后的 then 从等待结束处开始
 *   parallel(组)   组的各段与**最后一段同时**开始(并行组)
 *   ease(...)      改最后一段的曲线
 * 只驱动属性(默认走 Qt 属性系统):自绘控件声明 Q_PROPERTY 并在 paintEvent 里读,
 * 动画因此**永远不会**触发布局 —— 这是 §10.2.1 性能纪律里"不频繁 setGeometry"的落实方式。 */
class Anim {
public:
    Anim() = default;

    /** 一段动画:target 的 property 在 ms 毫秒内从**当前值**(start() 那一刻取)走到 to。
     *  target 活得比动画短也没关系(内部是弱引用,目标没了就跳过)。 */
    static Anim to(QObject *target, const char *property, const QVariant &to, int ms,
                   Ease::Name ease = Ease::Out);

    /** 列表错峰模板(§10.2.1 第 3 条):第 i 行在 i*delayMs 时刻开始,各走 ms 毫秒到 1.0。
     *  全部行共用**一条**时间线(Anim::trackCount() = 行数),不给每行建一个动画对象;
     *  被显式隐藏的行不建轨道。property 默认 "reveal"。
     *  传给它的行集应当就是"屏幕内可见的那些":长列表先用 rowsInView() 过滤,滚出屏幕的行连轨道都不建
     *  —— 这就是 §10.2.1「同时存在的动画对象 ≤ 屏内可见行数」的落点。 */
    static Anim stagger(const QVector<QObject *> &rows, int ms, int delayMs,
                        Ease::Name ease = Ease::Out, const char *property = "reveal");

    /** 从 rows 里挑出**与 viewport 相交**、且没有被显式隐藏的行(±margin 像素预热)。
     *  viewport 传空 = 原样返回(不过滤)。非控件对象一律保留(它们没有几何可言)。 */
    static QVector<QObject *> rowsInView(const QVector<QObject *> &rows, QWidget *viewport,
                                         int margin = 0);

    Anim &then(const Anim &next);
    Anim &after(int ms);
    Anim &parallel(const Anim &group);
    Anim &ease(Ease::Name ease);
    /** 最后一段改走"按初速那条"曲线(拖拽松手后的惯性:初速越大越陡)。 */
    Anim &glide(double v0, double distance);

    /** 交给全局心跳(第 0 帧当帧落地;motion=off 时直接落到终值,0 帧)。 */
    Anim &start();
    /** 停下(保持当前值,不跳终值)。 */
    void stop();

    /* ── 可查的量(验收/测试用) ── */
    bool running() const;
    /** 整条时间线的线性进度 [0,1](未开始 0;走完 1;motion=off 立即 1)。 */
    double progress() const;
    /** 总时长(ms,含等待;已经过档位换算)。 */
    int durationMs() const;
    int stepCount() const;
    /** 轨道总数(= 错峰模板里的行数)。 */
    int trackCount() const;
    /** 有轨道的那几段的起始偏移表(错峰验收就是直接断言这张表 —— 不靠 sleep)。 */
    QVector<int> offsetsMs() const;
    /** 各轨道的当前值(只读回属性,用于断言/日志)。 */
    QVector<double> values() const;

private:
    QSharedPointer<class AnimPlan> m_plan;
    QSharedPointer<class AnimRun> m_run;
    bool m_applied = false; /* motion=off 时"已直接就位" */
};

} // namespace sxcl::ui2
