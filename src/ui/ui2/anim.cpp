/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

/* 新界面的动画基座实现(docs/27 §10.2 / §10.2.1 / §13.1)。
 *
 * 这一份是**自研**的:没有搬 PCL(ModAnimation.vb,VB/WPF 的 DependencyProperty 那套)的函数名、
 * 枚举与数据结构 —— 我们这边是 Qt 的属性系统 + 一条时间线 + 一个心跳,曲线公式自己推(见下面 Curve 段)。
 *
 * 三条结构上的取舍:
 *   1) 段表(AnimPlan)与运行实例(AnimRun)分开:段表是"页面写的那句话",运行实例才有 t0/进度;
 *   2) 所有时间线挂在**一个** Registry 上,由**一个** QTimer 心跳统一推帧 —— 不做 N 个 QTimer;
 *   3) 帧里只写属性(默认走 Qt 属性系统),不碰布局 —— 自绘控件在自己的 paintEvent 里读它。
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include "anim.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QMetaType>
#include <QPointer>
#include <QStyleHints>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "sxcl/settings.h"

#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace sxcl::ui2 {

/* ══ 曲线表(全站唯一来源) ══════════════════════════════════════════════════
 *
 * 全部是"幂次形"与一条三次过冲式,形状自己调:
 *   powOut(p,k) = 1-(1-p)^k    —— 初速 k、末速 0 的收尾(出场用;k 越大尾巴越长)
 *   powIn(p,k)  = p^k          —— 初速 0、末速 k 的起步(入场用)
 *   两端缓 用 smootherstep(6p^5-15p^4+10p^3),更快的一档用"两半各一条四次幂"(在 p=0.5 处导数连续)
 *   Back       = 1 + (c+1)(p-1)^3 + c(p-1)^2,c = 1.1 -> 峰值 1.018(§4:缩放不超过 1.02、不用弹跳)
 *   Glide      = 1-(1-p)^s,s = 初速×时长/距离(归一化初速)—— "按初速那条"
 *
 * 这些曲线都满足 at(0)=0、at(1)=1,且在 [0,1] 上单调(Back 系过冲但在 1.02 以内)。
 * 不依赖 QEasingCurve 的枚举:页面只认 Ease::Out 这种语义名,换形状只改这一处。 */
namespace {

double clamp01(double p) {
    return p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
}
double powOut(double p, double k) {
    return 1.0 - std::pow(1.0 - p, k);
}
double powIn(double p, double k) {
    return std::pow(p, k);
}
double smootherStep(double p) {
    return p * p * p * (p * (p * 6.0 - 15.0) + 10.0);
}
/* 两半各一条四次幂:at(0.5)=0.5,且在 0.5 处左右导数都是 4(比 smootherstep 更"有劲") */
double sharperStep(double p) {
    if (p < 0.5)
        return 8.0 * p * p * p * p;
    const double q = 1.0 - p;
    return 1.0 - 8.0 * q * q * q * q;
}
/* 过冲回位的三次式(自己定的常数 c):
 *   f(p) = 1 + (c+1)(p-1)^3 + c(p-1)^2,极值点 x=1-p=2c/(3(c+1))
 *   c = 0.7 时峰值 = 1.0176 —— 落在 §4「不用弹跳、缩放不超过 1.02」以内;
 *   c 再大(经典 back 的 1.70158)会冲到 1.1,那是弹跳,不要。 */
double backCore(double p, double c) {
    const double q = p - 1.0;
    return 1.0 + (c + 1.0) * q * q * q + c * q * q;
}
const double kBackC = 0.7;

} // namespace

double Ease::at(Name name, double progress) {
    const double p = clamp01(progress);
    switch (name) {
    case Out:         return powOut(p, 3.0);
    case In:          return powIn(p, 3.0);
    case InOut:       return smootherStep(p);
    case FluentOut:   return powOut(p, 5.0);
    case FluentIn:    return powIn(p, 5.0);
    case InOutFluent: return sharperStep(p);
    case Back:        return backCore(p, kBackC);
    case BackIn:      return 1.0 - backCore(1.0 - p, kBackC);
    case Glide:       return glide(2.0, 1.0, 1000, p); /* 无参形态:归一化初速 2.0(1 秒走完 1 个距离) */
    }
    return powOut(p, 3.0);
}

double Ease::glide(double v0, double distance, int ms, double progress) {
    const double p = clamp01(progress);
    /* 归一化初速 = 初速 × 时长 / 总距离(单位在比值里自己抵消);夹到 [1,6]:
     *   1   = 匀速(松手时已经没速度了)
     *   6   = 甩得很猛(起步极陡、尾巴很长)
     * 非正/NaN 一律退化成匀速,绝不让曲线炸掉。 */
    double slope = 2.0;
    if (ms > 0 && distance != 0.0)
        slope = v0 * (static_cast<double>(ms) / 1000.0) / distance;
    if (!(slope > 1.0))
        slope = 1.0;
    if (slope > 6.0)
        slope = 6.0;
    return 1.0 - std::pow(1.0 - p, slope);
}

const char *Ease::name(Name name) {
    switch (name) {
    case Out:         return "out";
    case In:          return "in";
    case InOut:       return "inout";
    case FluentOut:   return "fluent-out";
    case FluentIn:    return "fluent-in";
    case InOutFluent: return "inout-fluent";
    case Back:        return "back";
    case BackIn:      return "back-in";
    case Glide:       return "glide";
    }
    return "out";
}

/* ══ 段表 / 运行实例 ════════════════════════════════════════════════════════ */

class AnimTrack {
public:
    QPointer<QObject> target;
    QByteArray property;
    QVariant to;
    Ease::Name ease = Ease::Out;
    bool glide = false;
    double v0 = 0.0;
    double distance = 1.0;
    bool isInt = false;
};

class AnimStep {
public:
    int at = 0;  /* 相对时间线起点的开始时刻 */
    int dur = 0; /* 时长(ms) */
    QVector<AnimTrack> tracks;
};

class AnimPlan {
public:
    QVector<AnimStep> steps;

    int endMs() const {
        int end = 0;
        for (const AnimStep &s : steps)
            end = qMax(end, s.at + s.dur);
        return end;
    }
    int trackCount() const {
        int n = 0;
        for (const AnimStep &s : steps)
            n += s.tracks.size();
        return n;
    }
};

class AnimRun {
public:
    QSharedPointer<AnimPlan> plan;
    qint64 t0 = 0;
    QVector<bool> entered;
    QVector<QVector<double>> from;
    bool done = false;

    void tick(qint64 now);
};

namespace {

/* ── 注册表:全局唯一的活时间线表 + 全局唯一的心跳 ── */
struct Registry {
    QVector<QSharedPointer<AnimRun>> live;
    QTimer *heartbeat = nullptr;
    int heartbeatCreated = 0; /* 心跳 QTimer 被构造过几次 —— 验收断言它恒为 1 */
    qint64 frames = 0;
    std::function<qint64()> clock; /* 假时钟(测试);空 = 真实单调钟 */
    bool resolved = false;
    int level = int(Motion::Standard);
};

Registry &reg() {
    static Registry r;
    return r;
}

bool traceOn() {
    return qEnvironmentVariableIntValue("SXCL_UI2_ANIM_TRACE") == 1;
}

double readValue(const AnimTrack &t) {
    if (t.target.isNull())
        return 0.0;
    const QVariant v = t.target->property(t.property.constData());
    return v.isValid() ? v.toDouble() : 0.0;
}

void writeValue(const AnimTrack &t, double v) {
    if (t.target.isNull())
        return;
    if (t.isInt)
        t.target->setProperty(t.property.constData(), static_cast<int>(v < 0 ? v - 0.5 : v + 0.5));
    else
        t.target->setProperty(t.property.constData(), v);
}

double progressOf(const AnimRun &run, qint64 now) {
    const int total = run.plan ? run.plan->endMs() : 0;
    if (total <= 0)
        return 1.0;
    return clamp01(static_cast<double>(now - run.t0) / static_cast<double>(total));
}

void traceRun(const AnimRun &run, qint64 now) {
    if (!traceOn())
        return;
    std::fprintf(stderr, "[anim] t=%lld p=%.3f", static_cast<long long>(now - run.t0),
                 progressOf(run, now));
    for (const AnimStep &s : run.plan->steps) {
        for (const AnimTrack &t : s.tracks)
            std::fprintf(stderr, " %s=%.3f", t.property.constData(), readValue(t));
    }
    std::fprintf(stderr, "\n");
}

} // namespace

void AnimRun::tick(qint64 now) {
    const qint64 t = now - t0;
    bool allDone = true;
    for (int i = 0; i < plan->steps.size(); ++i) {
        const AnimStep &s = plan->steps[i];
        if (!entered[i]) {
            if (t < static_cast<qint64>(s.at)) {
                allDone = false;
                continue;
            }
            /* 段一开始才取"当前值"当起点 —— then 串起来改同一个属性时,第二段自动接第一段的落点 */
            entered[i] = true;
            for (int k = 0; k < s.tracks.size(); ++k)
                from[i][k] = readValue(s.tracks[k]);
        }
        double p = 1.0;
        if (s.dur > 0) {
            p = static_cast<double>(t - s.at) / static_cast<double>(s.dur);
            p = clamp01(p);
        }
        for (int k = 0; k < s.tracks.size(); ++k) {
            const AnimTrack &track = s.tracks[k];
            const double a = from[i][k];
            const double b = track.to.toDouble();
            const double shaped = track.glide ? Ease::glide(track.v0, track.distance, s.dur, p)
                                              : Ease::at(track.ease, p);
            writeValue(track, a + (b - a) * shaped);
        }
        if (t < static_cast<qint64>(s.at) + s.dur)
            allDone = false;
    }
    done = allDone;
}

/* ══ 动效档位(ui.motion 与系统无障碍取更严的) ══════════════════════════════ */

namespace {

QString settingRaw() {
    const QByteArray env = qgetenv("SXCL_UI_MOTION").trimmed().toLower();
    if (!env.isEmpty())
        return QString::fromLatin1(env); /* 验收钉子:临时改一次,不动用户配置 */
    QString path = qEnvironmentVariable("SXCL_UI_SETTINGS");
    if (path.isEmpty()) {
        char buf[1024];
        char err[256];
        if (sxcl_settings_default_path(buf, sizeof(buf), err, sizeof(err)) == SXCL_SETTINGS_OK)
            path = QString::fromUtf8(buf);
    }
    if (path.isEmpty())
        return QStringLiteral("follow");
    sxcl_settings *st = sxcl_settings_open(path.toUtf8().constData());
    if (st == nullptr)
        return QStringLiteral("follow");
    const char *v = sxcl_settings_get(st, "ui.motion", "follow");
    const QString out = QString::fromUtf8(v != nullptr ? v : "follow").trimmed().toLower();
    sxcl_settings_free(st);
    return out;
}

/* 系统无障碍的"减少动态效果":
 *   1) SXCL_UI_MOTION_SYSTEM 钉死(验收用);
 *   2) 先问 Qt 有没有语义属性(将来版本若补上,这里不用改代码);
 *   3) Windows 上取"在 Windows 中显示动画"(SPI_GETCLIENTAREAANIMATION)的反面;
 *   4) 其它平台没有统一接口 —— 当"标准"(设置里仍可手动选减弱/关闭)。 */
bool systemReduced() {
    const QByteArray pin = qgetenv("SXCL_UI_MOTION_SYSTEM").trimmed().toLower();
    if (!pin.isEmpty())
        return pin == "reduced";
    if (QGuiApplication::instance() != nullptr) {
        const QVariant v = QGuiApplication::styleHints()->property("reduceMotion");
        if (v.isValid())
            return v.toBool();
    }
#if defined(Q_OS_WIN)
    BOOL anim = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0))
        return anim == FALSE;
#endif
    return false;
}

/* 档位换算:时长与错峰延迟在**建段表时**换算好,页面不用管。 */
int scaleDuration(int ms) {
    if (Motion::level() == Motion::Reduced)
        return qMax(1, static_cast<int>(ms * 0.35));
    return qMax(0, ms);
}

int scaleDelay(int ms) {
    if (Motion::level() == Motion::Reduced)
        return 0; /* 减弱:不排队,一行行冒出来本身也是"动" */
    return qMax(0, ms);
}

bool canAnimate() {
    QCoreApplication *app = QCoreApplication::instance();
    if (app == nullptr)
        return false; /* 没有事件循环:就地落地,不给用户留半截画面 */
    return QThread::currentThread() == app->thread();
}

} // namespace

const char *Motion::key() {
    return "ui.motion";
}

const char *Motion::settingValue() {
    static QByteArray cached;
    cached = settingRaw().toUtf8();
    return cached.constData();
}

void Motion::refresh() {
    Registry &r = reg();
    const bool sys = systemReduced();
    const QString s = settingRaw();
    Level lv = Standard;
    if (s == QLatin1String("off"))
        lv = Off;
    else if (s == QLatin1String("reduced"))
        lv = Reduced;
    else if (s == QLatin1String("standard"))
        lv = Standard;
    else
        lv = sys ? Reduced : Standard; /* follow(默认):听系统的 */
    if (sys && lv > Reduced)
        lv = Reduced; /* 与系统取更严的那个 */
    r.level = int(lv);
    r.resolved = true;
}

Motion::Level Motion::level() {
    Registry &r = reg();
    if (!r.resolved)
        refresh();
    return static_cast<Level>(r.level);
}

const char *Motion::levelName(Level level) {
    switch (level) {
    case Off:      return "off";
    case Reduced:  return "reduced";
    case Standard: return "standard";
    }
    return "standard";
}

int Motion::heartbeatCount() {
    return reg().heartbeatCreated;
}

bool Motion::heartbeatActive() {
    return reg().heartbeat != nullptr && reg().heartbeat->isActive();
}

int Motion::timelinesAlive() {
    return reg().live.size();
}

qint64 Motion::frameCount() {
    return reg().frames;
}

qint64 Motion::nowMs() {
    Registry &r = reg();
    if (r.clock)
        return r.clock();
    static QElapsedTimer timer = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return timer.elapsed();
}

void Motion::setClockForTest(std::function<qint64()> clock) {
    reg().clock = std::move(clock);
}

void Motion::pump() {
    Registry &r = reg();
    ++r.frames;
    const qint64 now = nowMs();
    int keep = 0;
    for (int i = 0; i < r.live.size(); ++i) {
        /* 值拷贝(不是引用):属性 setter 里再起一条动画会让 live 扩容,引用会踩空 */
        const QSharedPointer<AnimRun> run = r.live.at(i);
        run->tick(now);
        traceRun(*run, now);
        if (!run->done)
            r.live[keep++] = run;
    }
    r.live.resize(keep);
    if (r.live.isEmpty() && r.heartbeat != nullptr)
        r.heartbeat->stop(); /* 停表不销毁:心跳始终只有一个 */
}

namespace {

QTimer *ensureHeartbeat() {
    Registry &r = reg();
    if (r.heartbeat == nullptr) {
        r.heartbeat = new QTimer(); /* 进程级单例,故意不给 parent(活得比任何窗口长) */
        r.heartbeat->setTimerType(Qt::PreciseTimer);
        r.heartbeat->setInterval(16); /* 60Hz 一档;心跳推的是时间线,不是每帧都必须画 */
        QObject::connect(r.heartbeat, &QTimer::timeout, r.heartbeat, [] { Motion::pump(); });
        ++r.heartbeatCreated;
    }
    return r.heartbeat;
}

} // namespace

/* ══ Anim ══════════════════════════════════════════════════════════════════ */

Anim Anim::to(QObject *target, const char *property, const QVariant &to, int ms, Ease::Name ease) {
    Anim a;
    a.m_plan = QSharedPointer<AnimPlan>::create();
    AnimStep s;
    s.at = 0;
    s.dur = scaleDuration(ms);
    AnimTrack t;
    t.target = target;
    t.property = QByteArray(property != nullptr ? property : "reveal");
    t.to = to;
    t.ease = ease;
    const int tid = to.typeId();
    t.isInt = (tid == QMetaType::Int || tid == QMetaType::LongLong || tid == QMetaType::UInt ||
               tid == QMetaType::Bool);
    s.tracks.append(t);
    a.m_plan->steps.append(s);
    return a;
}

Anim Anim::stagger(const QVector<QObject *> &rows, int ms, int delayMs, Ease::Name ease,
                   const char *property) {
    Anim a;
    a.m_plan = QSharedPointer<AnimPlan>::create();
    const int dur = scaleDuration(ms);
    const int step = scaleDelay(delayMs);
    const QByteArray prop(property != nullptr ? property : "reveal");
    int index = 0;
    for (QObject *row : rows) {
        if (row == nullptr)
            continue;
        /* 被显式隐藏的行(折叠/收起)不建轨道:同时存在的动效只跟屏内可见行数走 */
        if (const auto *w = qobject_cast<const QWidget *>(row); w != nullptr && w->isHidden())
            continue;
        AnimStep s;
        s.at = index * step;
        s.dur = dur;
        AnimTrack t;
        t.target = row;
        t.property = prop;
        t.to = 1.0;
        t.ease = ease;
        t.isInt = false;
        s.tracks.append(t);
        a.m_plan->steps.append(s);
        ++index;
    }
    return a;
}

QVector<QObject *> Anim::rowsInView(const QVector<QObject *> &rows, QWidget *viewport, int margin) {
    if (viewport == nullptr)
        return rows;
    const QRect view = viewport->rect().adjusted(-margin, -margin, margin, margin);
    QVector<QObject *> out;
    out.reserve(rows.size());
    for (QObject *row : rows) {
        if (row == nullptr)
            continue;
        auto *w = qobject_cast<QWidget *>(row);
        if (w == nullptr) { /* 没有几何的对象:交给调用方判断 */
            out.append(row);
            continue;
        }
        if (w->isHidden())
            continue;
        const QRect rect(w->mapTo(viewport, QPoint(0, 0)), w->size());
        if (rect.intersects(view))
            out.append(row);
    }
    return out;
}

Anim &Anim::then(const Anim &next) {
    if (!m_plan || !next.m_plan)
        return *this;
    const int base = m_plan->endMs();
    for (const AnimStep &s : next.m_plan->steps) {
        AnimStep copy = s;
        copy.at = s.at + base;
        m_plan->steps.append(copy);
    }
    return *this;
}

Anim &Anim::after(int ms) {
    if (!m_plan)
        return *this;
    AnimStep gap;
    gap.at = m_plan->endMs();
    gap.dur = scaleDelay(ms);
    m_plan->steps.append(gap);
    return *this;
}

Anim &Anim::parallel(const Anim &group) {
    if (!m_plan || !group.m_plan)
        return *this;
    const int anchor = m_plan->steps.isEmpty() ? 0 : m_plan->steps.last().at;
    for (const AnimStep &s : group.m_plan->steps) {
        AnimStep copy = s;
        copy.at = s.at + anchor;
        m_plan->steps.append(copy);
    }
    return *this;
}

Anim &Anim::ease(Ease::Name ease) {
    if (m_plan && !m_plan->steps.isEmpty()) {
        for (AnimTrack &t : m_plan->steps.last().tracks)
            t.ease = ease;
    }
    return *this;
}

Anim &Anim::glide(double v0, double distance) {
    if (m_plan && !m_plan->steps.isEmpty()) {
        for (AnimTrack &t : m_plan->steps.last().tracks) {
            t.glide = true;
            t.v0 = v0;
            t.distance = distance;
        }
    }
    return *this;
}

Anim &Anim::start() {
    if (!m_plan || m_plan->steps.isEmpty())
        return *this;

    const auto applyFinal = [this] {
        for (const AnimStep &s : m_plan->steps) {
            for (const AnimTrack &t : s.tracks)
                writeValue(t, t.to.toDouble());
        }
        m_applied = true;
    };

    /* 关闭动效 / 没有事件循环 / 不在界面线程:终值立即就位,一帧都不跑(§13.1 第 4 条) */
    if (Motion::level() == Motion::Off || !canAnimate()) {
        applyFinal();
        m_run.reset();
        if (traceOn()) {
            std::fprintf(stderr, "[anim] motion=%s -> 立即就位(0 帧)\n", Motion::levelName(Motion::level()));
        }
        return *this;
    }

    /* 同一条时间线被重复 start:先把上一趟下线,别留两条在推同一批属性 */
    if (m_run) {
        Registry &r = reg();
        for (int i = 0; i < r.live.size(); ++i) {
            if (r.live.at(i) == m_run) {
                r.live.removeAt(i);
                break;
            }
        }
        m_run.reset();
    }

    auto run = QSharedPointer<AnimRun>::create();
    run->plan = m_plan;
    run->t0 = Motion::nowMs();
    run->entered.resize(m_plan->steps.size());
    run->from.resize(m_plan->steps.size());
    for (int i = 0; i < m_plan->steps.size(); ++i)
        run->from[i].resize(m_plan->steps.at(i).tracks.size());
    m_run = run;
    m_applied = false;
    reg().live.append(run);
    ensureHeartbeat()->start();
    Motion::pump(); /* 第 0 帧:start() 当帧就把起点/终点值落下去 */

    if (traceOn()) {
        std::fprintf(stderr, "[anim] start steps=%d tracks=%d dur=%dms level=%s offsets=",
                     static_cast<int>(m_plan->steps.size()), m_plan->trackCount(), m_plan->endMs(),
                     Motion::levelName(Motion::level()));
        const QVector<int> offs = offsetsMs();
        for (int i = 0; i < offs.size(); ++i)
            std::fprintf(stderr, "%s%d", i == 0 ? "" : ",", offs.at(i));
        std::fprintf(stderr, "\n");
    }
    return *this;
}

void Anim::stop() {
    if (!m_run)
        return;
    Registry &r = reg();
    for (int i = 0; i < r.live.size(); ++i) {
        if (r.live.at(i) == m_run) {
            r.live.removeAt(i);
            break;
        }
    }
    m_run->done = true;
    m_run.reset();
}

bool Anim::running() const {
    return m_run && !m_run->done;
}

double Anim::progress() const {
    if (m_run)
        return progressOf(*m_run, Motion::nowMs());
    return m_applied ? 1.0 : 0.0;
}

int Anim::durationMs() const {
    return m_plan ? m_plan->endMs() : 0;
}

int Anim::stepCount() const {
    return m_plan ? m_plan->steps.size() : 0;
}

int Anim::trackCount() const {
    return m_plan ? m_plan->trackCount() : 0;
}

QVector<int> Anim::offsetsMs() const {
    QVector<int> out;
    if (!m_plan)
        return out;
    for (const AnimStep &s : m_plan->steps) {
        if (!s.tracks.isEmpty())
            out.append(s.at);
    }
    return out;
}

QVector<double> Anim::values() const {
    QVector<double> out;
    if (!m_plan)
        return out;
    for (const AnimStep &s : m_plan->steps) {
        for (const AnimTrack &t : s.tracks)
            out.append(readValue(t));
    }
    return out;
}

} // namespace sxcl::ui2
