// SXCL C - Android entry point (packaging layer; NOT part of the shared tree).
//
// What this file does, and all it does:
//   1. reads the boot file the Java activity wrote before Qt started
//      (<files>/sxcl_boot.txt: extracted asset root, route, shot flags, scale);
//   2. turns those into the environment variables the unmodified desktop entry
//      already understands (SXCL_THEME_DIR / SXCL_BLOCK_DIR / SXCL_ICON_DIR /
//      SXCL_UI_ROUTE / SXCL_UI_SHOT / SXCL_UI_ACCENT) plus the QPA + scale knobs;
//   3. hands control to src/ui/main.cpp, which is included below under a
//      renamed symbol - so there is exactly one copy of the desktop entry
//      logic and no Android fork of the UI.
//
// ASCII-only (Android toolchains treat non-ASCII build inputs badly).

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QtGlobal>

#include <cstdio>

#ifdef __ANDROID__
#include <QJniObject>
#include <android/log.h>
#include <sxcl/android.h>
#include <sxcl/launch.h>
#include <sxcl/paths.h>
#define SXCL_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "sxcl", __VA_ARGS__)
#else
#define SXCL_LOGI(...)                                     \
    do {                                                   \
        std::fprintf(stderr, "[sxcl] " __VA_ARGS__);       \
        std::fprintf(stderr, "\n");                        \
    } while (0)
#endif


// ---- Android touch adaptation ----------------------------------------------
// Qt Widgets is a mouse-first toolkit: on Android a drag on a scroll area does
// nothing (no kinetic scrolling, no flick). The user-visible symptom reported
// on the device was "上下菜单都划不动". Everything below lives in this
// Android-only packaging file, so the desktop build is untouched.
#ifdef __ANDROID__
#include <QAbstractScrollArea>
#include <QApplication>
#include <QChildEvent>
#include <QEvent>
#include <QComboBox>
#include <QMouseEvent>
#include <QPointer>
#include <QScroller>
#include <QScreen>
#include <QSet>
#include <QScrollerProperties>
#include <QScrollBar>
#include <QTimer>
#include <QVariant>
#include <QWidget>

namespace {

// Fluent-ish feel: small drag threshold, long inertia, 60 fps frames.
void sxclTuneScroller(QScroller *sc) {
    QScrollerProperties p = sc->scrollerProperties();
    p.setScrollMetric(QScrollerProperties::DragStartDistance, QVariant(0.004));
    p.setScrollMetric(QScrollerProperties::DragVelocitySmoothingFactor, QVariant(0.5));
    // Qt 6 removed the per-axis "*DragFlickDeceleration" metrics; the glide uses
    // the stock DecelerationFactor so the inertia feel stays Qt's default.
    p.setScrollMetric(QScrollerProperties::MaximumVelocity, QVariant(3.0));
    p.setScrollMetric(QScrollerProperties::MinimumVelocity, QVariant(0.04));
    p.setScrollMetric(QScrollerProperties::OvershootDragResistanceFactor, QVariant(0.5));
    p.setScrollMetric(QScrollerProperties::OvershootScrollDistanceFactor, QVariant(0.2));
    p.setScrollMetric(QScrollerProperties::FrameRate, QVariant(int(QScrollerProperties::Fps60)));
    sc->setScrollerProperties(p);
}

// Every QAbstractScrollArea (page shells, the navigation panel list, the
// versions list, the keymap lists and the ComboBox popup list) gets a touch
// scroller on its viewport. Floating SmoothScrollBar children are made
// transparent to mouse events so a finger landing near the bar still drags
// the viewport instead of being swallowed by the bar.
// Child-widget click-through switch. OFF unless the launch asks for it: it made plain
// containers transparent, and Qt disables mouse delivery to the widget AND ITS CHILDREN,
// which killed the buttons/lists inside them (user report "点都点不动").
// Every piece of evidence printed below carries this flag, so any coordinate or
// displacement number self-identifies the mode it was taken in.
bool sxclPassthroughEnabled() {
    static const bool on = qEnvironmentVariableIsSet("SXCL_TOUCH_PASSTHROUGH");
    return on;
}

// True when the widget IS the viewport of some QAbstractScrollArea (at any depth).
// Used by the click-through pass: a nested scroll area's viewport is a plain QWidget,
// so it fails the "interactive" test even though it must stay hit-testable.
bool sxclIsScrollAreaViewport(QWidget *w) {
    auto *parentArea = qobject_cast<QAbstractScrollArea *>(w->parentWidget());
    return parentArea != nullptr && parentArea->viewport() == w;
}

// Event names for the input trace (only the ones a drag can produce).
const char *sxclEventName(int type) {
    switch (type) {
    case QEvent::MouseButtonPress:
        return "MousePress";
    case QEvent::MouseMove:
        return "MouseMove";
    case QEvent::MouseButtonRelease:
        return "MouseRelease";
    case QEvent::TouchBegin:
        return "TouchBegin";
    case QEvent::TouchUpdate:
        return "TouchUpdate";
    case QEvent::TouchEnd:
        return "TouchEnd";
    default:
        return "?";
    }
}

// 交互控件(按钮/输入/下拉/列表/滑块/可点条目)要保持可点,不能被设成鼠标穿透。
bool sxclIsInteractiveWidget(QWidget *w) {
    static const char *kKinds[] = {"QAbstractButton", "QLineEdit",      "QAbstractSpinBox",
                                   "QComboBox",       "QAbstractItemView", "QAbstractSlider",
                                   "QTextEdit",       "QPlainTextEdit",  "QTabBar",
                                   "QScrollArea",     "QAbstractScrollArea"};
    for (const char *k : kKinds) {
        if (w->inherits(k))
            return true;
    }
    return false;
}

// Qt 在 Android 上会把一部分拖拽(尤其 adb 注入的 input swipe)合成成 **mouse** 事件,
// 而 QScroller 的 TouchGesture 只吃 touch -> 真手指能滚、注入拖拽纹丝不动(实测:
// 有 1180px 可滚、passthrough 也设了,swipe 仍 0px 且没有 scroll MOVE 日志)。
// 这里把视口上的鼠标拖拽直接喂给同一个 QScroller,两条输入路径都成立。
class SxclViewportDragFilter : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        QWidget *vp = qobject_cast<QWidget *>(obj);
        if (vp == nullptr)
            return false;
        // 实测:按压命中的是 QAbstractScrollArea 容器本身(日志 "MousePress -> ScrollArea [HomePage]"),
        // 不是它的 viewport。所以两种目标都支持:容器 -> 取它 viewport 的 scroller;viewport -> 直接用。
        QScroller *sc = nullptr;
        if (QAbstractScrollArea *area = qobject_cast<QAbstractScrollArea *>(vp))
            sc = QScroller::scroller(area->viewport());
        if (sc == nullptr)
            sc = QScroller::scroller(vp);
        if (sc == nullptr)
            return false;
        // QScroller::handleInput() 这条路在本工程实测走不通(事件命中的是容器、状态机不接管),
        // 所以鼠标拖拽**直接映射到滚动条**:按下记起点与当时的值,移动按像素差设 value。
        // 真手指走 QScroller(惯性/回弹照旧),注入或合成拖拽走这条 —— 两条路都能滚。
        QScrollBar *bar = nullptr;
        if (QAbstractScrollArea *area = qobject_cast<QAbstractScrollArea *>(vp))
            bar = area->verticalScrollBar();
        else if (QAbstractScrollArea *area = qobject_cast<QAbstractScrollArea *>(vp->parentWidget()))
            bar = area->verticalScrollBar();
        if (bar == nullptr)
            return false;

        switch (ev->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(ev);
            m_press = me->position();
            m_startValue = bar->value();
            m_bar = bar;
            m_pressed = true;
            break;
        }
        case QEvent::MouseMove: {
            if (!m_pressed || m_bar == nullptr)
                break;
            auto *me = static_cast<QMouseEvent *>(ev);
            const int dy = int(m_press.y() - me->position().y());
            const int want = m_startValue + dy;
            if (want != m_bar->value())
                m_bar->setValue(want); // valueChanged -> 触发 scroll MOVE 日志与重绘
            break;
        }
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
            m_pressed = false;
            break;
        default:
            break;
        }
        Q_UNUSED(sc);
        return false; // 不吞事件:控件自己的点击/悬停照旧
    }

private:
    QPointF m_press;
    QPointer<QScrollBar> m_bar;
    int m_startValue = 0;
    bool m_pressed = false;
};

void sxclAttachTouchScrolling(const char *why) {
    const QWidgetList all = QApplication::allWidgets();
    int attached = 0;
    for (QWidget *w : all) {
        QAbstractScrollArea *area = qobject_cast<QAbstractScrollArea *>(w);
        if (!area)
            continue;
        QWidget *vp = area->viewport();
        if (!vp)
            continue;
        if (!vp->property("sxclTouchAttached").toBool()) {
            vp->setProperty("sxclTouchAttached", true);
            QScroller::grabGesture(vp, QScroller::TouchGesture);
            // 同一条路也接 mouse 拖拽(见 SxclViewportDragFilter 的说明):
            // viewport 与容器本身**都装** —— 实测按压会命中容器,QScroller 挂在 viewport 上,两头都不能漏。
            vp->installEventFilter(new SxclViewportDragFilter());
            if (!area->property("sxclDragFilter").toBool()) {
                area->setProperty("sxclDragFilter", true);
                area->installEventFilter(new SxclViewportDragFilter());
            }
            attached++;
        }
        if (QScroller *sc = QScroller::scroller(vp))
            sxclTuneScroller(sc);
        // evidence line: is this area actually scrollable (content > viewport)?
        // used by the touch-acceptance table in docs/08 (0 px swipe == content fits)
        if (QScrollBar *vb = area->verticalScrollBar()) {
            const QPoint gp = vp->mapToGlobal(QPoint(0, 0));
            SXCL_LOGI("scrollarea %s [%s] rect=(%d,%d %dx%d) viewport_h=%d content_h=%d scrollable=%d",
                      area->metaObject()->className(),
                      area->objectName().toUtf8().constData(), gp.x(), gp.y(), vp->width(),
                      vp->height(),
                      vp->height(), vp->height() + vb->maximum(), vb->maximum() > 0 ? 1 : 0);
        }
        // 手指落在覆盖在视口上的子控件(卡片/标签)时,拖拽可能根本到不了视口 -> QScroller 看不到手势。
        // 把"非交互"子控件设为鼠标穿透(Qt 仍然先做子节点命中测试,所以卡片里的按钮照旧能点);
        // 自绘的浮动滚动条本来就是非交互的,这里一并覆盖。
        // ⚠ Click-through is OPT-IN (SXCL_TOUCH_PASSTHROUGH=1) as of 2026-09-21.
        // It used to be on by default and it broke TAPS: Qt documents
        // WA_TransparentForMouseEvents as "disables delivery of mouse events to the
        // widget AND ITS CHILDREN", so making a plain container (a card, a group, a row)
        // transparent also killed the buttons and lists inside it. User report:
        // "现在点都点不动" - a broken tap is far worse than a stiff scroll.
        //
        // Only the floating SmoothScrollBar stays transparent unconditionally: it is
        // pure decoration drawn over the viewport and never participates in clicking.
        //
        // The recursive-viewport skip belongs to the same opt-in switch: findChildren()
        // is recursive, so a page shell also visits the viewports of the scroll areas
        // nested inside it (keymap page: two QListWidgets). Those are plain QWidgets and
        // used to be made transparent too, which cost the inner list both its drag and
        // its item clicks. Kept here, but only reachable when the switch is on.
        const bool passthroughEnabled = sxclPassthroughEnabled();
        int passthrough = 0;
        const QList<QWidget *> kids = area->findChildren<QWidget *>();
        for (QWidget *k : kids) {
            const QString cls = QString::fromLatin1(k->metaObject()->className());
            const bool isBar = cls.contains(QLatin1String("ScrollBar"));
            if (!isBar) {
                if (!passthroughEnabled)
                    continue;
                if (sxclIsScrollAreaViewport(k))
                    continue;
                if (sxclIsInteractiveWidget(k))
                    continue;
            }
            if (!k->testAttribute(Qt::WA_TransparentForMouseEvents)) {
                k->setAttribute(Qt::WA_TransparentForMouseEvents, true);
                ++passthrough;
            }
        }
        // 证据:滚动条每一次变化都记一行 -> 设备上滑动后看日志就知道"手势有没有变成滚动"
        if (!area->property("sxclScrollLogged").toBool()) {
            area->setProperty("sxclScrollLogged", true);
            if (QScrollBar *vb2 = area->verticalScrollBar()) {
                // objectName matters here: the keymap page has a shell plus two nested
                // lists, and "which one actually moved" is the whole measurement.
                QObject::connect(vb2, &QScrollBar::valueChanged, vb2, [area](int v) {
                    SXCL_LOGI("scroll MOVE %s [%s] value=%d/%d", area->metaObject()->className(),
                              area->objectName().toUtf8().constData(), v,
                              area->verticalScrollBar()->maximum());
                });
            }
        }
        if (passthrough > 0)
            SXCL_LOGI("passthrough set on %d child widget(s) of %s", passthrough,
                      area->metaObject()->className());
    }
    if (attached > 0)
        SXCL_LOGI("touch scroller attached to %d viewport(s) [%s]", attached, why);
}

class SxclTouchFilter : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        // 拖拽到底打到了谁?设备上 swipe 后没有任何 scroll MOVE、也没有 mouse/touch 转发命中,
        // 所以先在应用级把"事件类型 + 目标控件"打出来(只打前 24 条,避免刷屏)。
        switch (ev->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd: {
            // One line per (event type, receiver class + objectName): enough to answer
            // "does MouseMove arrive at all, and at whom" without drowning in the
            // propagation chain (a press is re-delivered to every ancestor).
            static QSet<QString> seen;
            static int logged = 0;
            QWidget *w = qobject_cast<QWidget *>(obj);
            const QString cls = w ? QString::fromLatin1(w->metaObject()->className())
                                  : QStringLiteral("(non-widget)");
            const QString name = w ? w->objectName() : QString();
            const QString key = QString::number(int(ev->type())) + cls + name;
            if (!seen.contains(key) && logged < 40) {
                seen.insert(key);
                ++logged;
                QPoint local(-1, -1);
                QPoint global(-1, -1);
                if (auto *me = dynamic_cast<QMouseEvent *>(ev)) {
                    local = me->position().toPoint();
                    global = me->globalPosition().toPoint();
                }
                QWidget *top = global.x() >= 0 ? QApplication::widgetAt(global) : nullptr;
                SXCL_LOGI("INPUT %s recv=%s [%s] local=(%d,%d) global=(%d,%d) top=%s [%s] "
                          "viewport=%d scroller=%d passthrough=%d",
                          sxclEventName(int(ev->type())), cls.toUtf8().constData(),
                          name.toUtf8().constData(), local.x(), local.y(), global.x(), global.y(),
                          top != nullptr ? top->metaObject()->className() : "-",
                          top != nullptr ? top->objectName().toUtf8().constData() : "",
                          (w != nullptr && sxclIsScrollAreaViewport(w)) ? 1 : 0,
                          (w != nullptr && QScroller::scroller(w) != nullptr) ? 1 : 0,
                          sxclPassthroughEnabled() ? 1 : 0);
            }
            break;
        }
        default:
            break;
        }
        switch (ev->type()) {
        case QEvent::Show:
        case QEvent::ChildAdded:
        case QEvent::Polish:
            // deferred: the new viewport is not in the tree yet when the event arrives
            QTimer::singleShot(0, qApp, []() { sxclAttachTouchScrolling("deferred"); });
            break;
        default:
            break;
        }
        return QObject::eventFilter(obj, ev);
    }
};

// Acceptance hook: open the Nth visible ComboBox popup and LEAVE IT OPEN, so a
// real finger tap / drag can be tested on the device (the desktop entry's own
// SXCL_UI_POPUP path always grabs-and-quits, which is too short for that).
void sxclMaybeOpenPopup() {
    const QByteArray idx = qgetenv("SXCL_UI_POPUP");
    if (idx.isEmpty() || QApplication::activePopupWidget())
        return;
    bool ok = false;
    const int want = idx.toInt(&ok);
    if (!ok || want < 0)
        return;
    QList<QComboBox *> combos;
    const QWidgetList all = QApplication::allWidgets();
    for (QWidget *w : all) {
        if (QComboBox *c = qobject_cast<QComboBox *>(w)) {
            if (c->isVisible())
                combos.append(c);
        }
    }
    if (want < combos.size()) {
        QComboBox *combo = combos.at(want);
        SXCL_LOGI("opening popup #%d (of %d visible combos) currentIndex=%d", want,
                  int(combos.size()), combo->currentIndex());
        // Acceptance assertion for a REAL finger tap on the popup (item: point-and-select
        // round trip). The whole UI is one SurfaceView, so the view tree cannot be read;
        // this is the only way to see index/text/closed on the device.
        SXCL_LOGI("popup ASSERT open: combo#%d currentIndex=%d text=%s", want,
                  combo->currentIndex(), combo->currentText().toUtf8().constData());
        if (!combo->property("sxclPopupAssert").toBool()) {
            combo->setProperty("sxclPopupAssert", true);
            QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [want](int index) {
                SXCL_LOGI("popup ASSERT index changed: combo#%d -> currentIndex=%d", want, index);
            });
            QObject::connect(combo, &QComboBox::textActivated, combo, [](const QString &text) {
                SXCL_LOGI("popup ASSERT text refilled: %s", text.toUtf8().constData());
            });
        }
        // watchdog: when the popup really closed, and what the combo says by then
        {
            QPointer<QComboBox> guard(combo);
            auto *watch = new QTimer(qApp);
            watch->setInterval(200);
            QObject::connect(watch, &QTimer::timeout, combo, [guard, want, watch]() {
                if (QApplication::activePopupWidget() != nullptr)
                    return;
                watch->stop();
                if (guard != nullptr)
                    SXCL_LOGI("popup ASSERT closed: combo#%d currentIndex=%d text=%s popupGone=1",
                              want, guard->currentIndex(),
                              guard->currentText().toUtf8().constData());
                else
                    SXCL_LOGI("popup ASSERT closed: combo#%d destroyed popupGone=1", want);
                watch->deleteLater();
            });
            watch->start();
        }
        combo->showPopup();
        // log the popup geometry once it is mapped, so the acceptance script can
        // aim a real finger swipe/tap at it without guessing coordinates
        QTimer::singleShot(500, qApp, []() {
            if (QWidget *pop = QApplication::activePopupWidget()) {
                const QPoint gp = pop->mapToGlobal(QPoint(0, 0));
                SXCL_LOGI("popup open rect=(%d,%d %dx%d) logical", gp.x(), gp.y(), pop->width(),
                          pop->height());
                const QList<QAbstractScrollArea *> areas = pop->findChildren<QAbstractScrollArea *>();
                for (QAbstractScrollArea *a : areas) {
                    if (QScrollBar *vb = a->verticalScrollBar()) {
                        SXCL_LOGI("popup list %s viewport_h=%d content_h=%d scrollable=%d",
                                  a->metaObject()->className(), a->viewport()->height(),
                                  a->viewport()->height() + vb->maximum(),
                                  vb->maximum() > 0 ? 1 : 0);
                    }
                }
            } else {
                SXCL_LOGI("popup NOT open after showPopup()");
            }
        });
    }
}

void sxclAndroidTouchSetup() {
    static SxclTouchFilter *filter = nullptr;
    if (!filter)
        filter = new SxclTouchFilter();
    qApp->installEventFilter(filter);
    // one line that pins the mode every later measurement was taken in
    SXCL_LOGI("touch mode: passthrough=%s scroller=TouchGesture dragfilter=scrollbar-map",
              sxclPassthroughEnabled() ? "ON(SXCL_TOUCH_PASSTHROUGH)" : "off");
    // the window itself is built a moment later by main.cpp
    QTimer::singleShot(300, qApp, []() { sxclAttachTouchScrolling("startup"); });
    QTimer::singleShot(1500, qApp, []() {
        sxclAttachTouchScrolling("late");
        sxclMaybeOpenPopup();
        // Evidence for popup placement: the popup clamps against the SCREEN's available
        // area while widget positions live in the WINDOW's logical space. If those two
        // disagree (window not full-screen, rotation, insets) a popup can drift off the
        // screen - this line records both, on the device, in one place.
        if (QScreen *s = QGuiApplication::primaryScreen()) {
            const QRect g = s->geometry();
            const QRect av = s->availableGeometry();
            SXCL_LOGI("screen: geometry=(%d,%d %dx%d) available=(%d,%d %dx%d) dpr=%.2f",
                      g.x(), g.y(), g.width(), g.height(), av.x(), av.y(), av.width(),
                      av.height(), s->devicePixelRatio());
        }
        if (QWidget *aw = QApplication::activeWindow()) {
            const QPoint gp = aw->mapToGlobal(QPoint(0, 0));
            SXCL_LOGI("window: rect=(%d,%d %dx%d) dpr=%.2f", gp.x(), gp.y(), aw->width(),
                      aw->height(), aw->devicePixelRatioF());
        }
    });
    QTimer::singleShot(2200, qApp, []() { sxclMaybeOpenPopup(); });
}

} // namespace

// runs right after QApplication is constructed, i.e. from inside main.cpp's
// "QApplication app(argc, argv);" line - before the window exists
Q_COREAPP_STARTUP_FUNCTION(sxclAndroidTouchSetup)
#endif // __ANDROID__
// ---- the real desktop entry, renamed so we can wrap it ---------------------
#define main sxcl_ui_desktop_main
#include "main.cpp"
#undef main

int sxcl_ui_desktop_main(int argc, char *argv[]);

namespace {

#ifdef __ANDROID__
QString androidContextPath(const char *method) {
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (!activity.isValid())
        return QString();
    QJniObject obj = activity.callObjectMethod(method, "()Ljava/io/File;");
    if (!obj.isValid())
        return QString();
    QJniObject path = obj.callObjectMethod("getAbsolutePath", "()Ljava/lang/String;");
    return path.isValid() ? path.toString() : QString();
}
#else
QString androidContextPath(const char *) { return QString(); }
#endif

struct Boot {
    QString assets;
    QString shots;
    QString route;
    QString scale;
    QString accent;
    QString theme;
    QString popup;
    QString tallmenu;
    QString passthrough;
    QString animtrace;
    bool shot = false;
    bool offscreen = false;
};

Boot readBootFile(const QString &filesDir) {
    Boot b;
    QFile f(filesDir + QStringLiteral("/sxcl_boot.txt"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        SXCL_LOGI("no boot file at %s", f.fileName().toUtf8().constData());
        return b;
    }
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString k = line.left(eq);
        const QString v = line.mid(eq + 1);
        if (k == QLatin1String("assets"))         b.assets = v;
        else if (k == QLatin1String("shots"))     b.shots = v;
        else if (k == QLatin1String("route"))     b.route = v;
        else if (k == QLatin1String("scale"))     b.scale = v;
        else if (k == QLatin1String("accent"))    b.accent = v;
        else if (k == QLatin1String("theme"))     b.theme = v;
        else if (k == QLatin1String("popup"))     b.popup = v;
        else if (k == QLatin1String("tallmenu"))  b.tallmenu = v;
        else if (k == QLatin1String("passthrough")) b.passthrough = v;
        else if (k == QLatin1String("animgtrace")) b.animtrace = v;
        else if (k == QLatin1String("shot"))      b.shot = (v == QLatin1String("1"));
        else if (k == QLatin1String("offscreen")) b.offscreen = (v == QLatin1String("1"));
    }
    return b;
}

} // namespace

// ---- Android runtime diagnostics (detection evidence) ----------------------
// The user-visible complaint that started this work was:
//   "my tablet has HMCL, it cannot be that there is no JAVA ...
//    the game directory was detected, but JAVA was NOT".
// The answer is a filesystem fact, not a bug in the scan loop:
//   * another app's private dir (/data/data/<pkg>) -> stat returns EACCES (sandbox);
//   * shared storage (/storage/emulated) -> mounted noexec, nothing there can exec.
// So instead of silently reporting "no Java found", the launcher prints WHAT it
// looked at and WHY each candidate was rejected, both to logcat (here, tag "sxcl",
// so a device can be checked without touching the UI) and in the settings page.
// ASCII-only source: the Chinese reason strings come from the core library.
#ifdef __ANDROID__
void sxclAndroidProbeLog(const QString &filesDir) {
    const QByteArray files = filesDir.toUtf8();
    char err[256];

    // 1) Java: every known location, with a verdict + the raw filesystem reason
    {
        sxcl_java_report report;
        const size_t n = sxcl_java_probe_android(files.constData(), NULL, &report);
        SXCL_LOGI("java-probe: %d candidate(s), %d usable", (int)n, (int)report.usable);
        for (size_t i = 0; i < report.count; ++i) {
            const sxcl_java_probe *p = &report.items[i];
            SXCL_LOGI("java-probe[%d] owner=%s verdict=%s src=%s major=%d path=%s", (int)i,
                      p->owner, sxcl_java_verdict_key(p->verdict), p->source, p->major, p->path);
            SXCL_LOGI("java-probe[%d] reason=%s", (int)i, p->reason);
            SXCL_LOGI("java-probe[%d] hint=%s", (int)i, sxcl_java_verdict_hint(p->verdict));
        }

        // what the ordinary discovery (used to pick a Java for launch) actually finds
        sxcl_java_env_store store;
        sxcl_java_info found[8];
        sxcl_java_env_capture(&store);
        const size_t fn = sxcl_java_discover(&store.env, sxcl_java_current_os(), found, 8);
        SXCL_LOGI("java-discover: %d usable installation(s)", (int)fn);
        for (size_t i = 0; i < fn; ++i) {
            SXCL_LOGI("java-discover[%d] major=%d version=%s src=%s path=%s", (int)i, found[i].major,
                      found[i].version, found[i].source, found[i].path);
        }
        SXCL_LOGI("java-discover: android_files=%s",
                  store.env.android_files ? store.env.android_files : "(unset)");
    }

    // 2) Game directories: same idea, every candidate gets a verdict
    {
        sxcl_game_probes probes;
        const size_t n = sxcl_paths_probe_android(files.constData(), NULL, NULL, &probes);
        SXCL_LOGI("gamedir-probe: %d candidate(s), %d usable", (int)n, (int)probes.usable);
        for (size_t i = 0; i < probes.count; ++i) {
            const sxcl_game_probe *g = &probes.items[i];
            SXCL_LOGI("gamedir-probe[%d] owner=%s access=%s versions=%d path=%s", (int)i, g->owner,
                      sxcl_android_access_key(g->access), g->versions, g->path);
            SXCL_LOGI("gamedir-probe[%d] reason=%s", (int)i, g->reason);
        }
        // and what the normal auto-scan collects (exists-only, score-sorted)
        sxcl_game_folders folders;
        err[0] = 0;
        (void)sxcl_paths_detect_android(files.constData(), NULL, &folders, NULL, err, sizeof(err));
        SXCL_LOGI("gamedir-detect: %d folder(s)", (int)folders.count);
        for (size_t i = 0; i < folders.count; ++i) {
            SXCL_LOGI("gamedir-detect[%d] owner=%s versions=%d label=%s path=%s", (int)i,
                      folders.items[i].owner, folders.items[i].versions, folders.items[i].label,
                      folders.items[i].path);
        }
        if (folders.count == 0)
            SXCL_LOGI("gamedir-detect: none; err=%s", err);
    }
}
#else
void sxclAndroidProbeLog(const QString &) {}
#endif

int main(int argc, char **argv) {
    const QString filesDir = androidContextPath("getFilesDir");
    SXCL_LOGI("android entry, filesDir=%s", filesDir.toUtf8().constData());

    if (!filesDir.isEmpty()) {
        const Boot boot = readBootFile(filesDir);
        SXCL_LOGI("boot: assets=%s route=%s offscreen=%d shot=%d scale=%s",
                  boot.assets.toUtf8().constData(), boot.route.toUtf8().constData(),
                  int(boot.offscreen), int(boot.shot), boot.scale.toUtf8().constData());

        if (!boot.assets.isEmpty()) {
            qputenv("SXCL_THEME_DIR", (boot.assets + QStringLiteral("/theme")).toUtf8());
            qputenv("SXCL_BLOCK_DIR", (boot.assets + QStringLiteral("/icons/blocks")).toUtf8());
            qputenv("SXCL_ICON_DIR", (boot.assets + QStringLiteral("/icons/pcl")).toUtf8());
        }
        // the launcher keeps its own data inside the app sandbox
        qputenv("HOME", filesDir.toUtf8());
        qputenv("TMPDIR", filesDir.toUtf8());
        // core libsxcl: sxcl_paths_default_game_dir() reads this on Android
        // (app-private dir -> no storage permission needed, wiped on uninstall)
        qputenv("SXCL_ANDROID_FILES", filesDir.toUtf8());
        // shared storage root: public storage where HMCL/FCL/PojavLauncher keep data.
        // It is a fuse noexec mount, so nothing executable can live there; the core
        // only uses it to LOOK (and to tell the user why a hit is not usable).
        if (!qEnvironmentVariableIsSet("SXCL_ANDROID_SHARED"))
            qputenv("SXCL_ANDROID_SHARED", QByteArray("/storage/emulated/0"));
        // log what the core resolves, so a fresh device can be checked from logcat
        {
            char dirBuf[4096];
            char dirErr[256];
            dirErr[0] = '\0';
            if (sxcl_paths_default_game_dir(dirBuf, sizeof(dirBuf), dirErr, sizeof(dirErr)) == 0)
                SXCL_LOGI("default game dir = %s", dirBuf);
            else
                SXCL_LOGI("default game dir FAILED: %s", dirErr);
        }

        // evidence: log every Java / game-directory candidate and WHY it was
        // (or was not) accepted -- answers "why does it not detect my Java"
        sxclAndroidProbeLog(filesDir);

        if (!boot.accent.isEmpty())
            qputenv("SXCL_UI_ACCENT", boot.accent.toUtf8());
        if (!boot.theme.isEmpty())
            qputenv("SXCL_UI_THEME", boot.theme.toUtf8());

        // Headless 1:1 render mode: the offscreen QPA plugin keeps the window at
        // the size MainWindow asks for (1100x750) instead of the device screen,
        // which is what makes a pixel comparison against build/ref/py_*.png
        // possible on a phone. Painting (and therefore the pixels) is the same
        // raster engine either way.
        if (boot.offscreen) {
            qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
            if (!boot.scale.isEmpty())
                qputenv("QT_SCALE_FACTOR", boot.scale.toUtf8());
        }

        if (!boot.route.isEmpty())
            qputenv("SXCL_UI_ROUTE", boot.route.toUtf8());
        // acceptance hook: open the Nth ComboBox popup on the current page
        if (!boot.popup.isEmpty())
            qputenv("SXCL_UI_POPUP", boot.popup.toUtf8());
        // acceptance fixture: pad a ComboBox so a tall menu can be scroll-tested
        if (!boot.tallmenu.isEmpty())
            qputenv("SXCL_UI_TALLMENU", boot.tallmenu.toUtf8());
        // acceptance switch: child-widget click-through. OFF by default - it broke taps.
        if (!boot.passthrough.isEmpty())
            qputenv("SXCL_TOUCH_PASSTHROUGH", boot.passthrough.toUtf8());
        // acceptance hook: animation parameter re-check (150ms/250ms OutQuad, setMask).
        // The shared entry samples the widget's real geometry per frame; on Android the
        // samples go to logcat under the "sxcl-ui" tag.
        if (!boot.animtrace.isEmpty())
            qputenv("SXCL_ANIM_TRACE", boot.animtrace.toUtf8());
        if (boot.shot && !boot.route.isEmpty() && !boot.shots.isEmpty()) {
            QDir().mkpath(boot.shots);
            qputenv("SXCL_UI_SHOT",
                    (boot.shots + QLatin1Char('/') + boot.route + QStringLiteral(".png")).toUtf8());
        }
    }

    const int rc = sxcl_ui_desktop_main(argc, argv);
    SXCL_LOGI("event loop finished rc=%d", rc);

    if (!filesDir.isEmpty()) {
        QFile stamp(filesDir + QStringLiteral("/sxcl_last_run.txt"));
        if (stamp.open(QIODevice::WriteOnly | QIODevice::Text))
            stamp.write(QByteArray::number(rc));
    }
    return rc;
}
