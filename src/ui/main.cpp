// sxcl-ui — Silent X Craft Launcher 图形前端入口(阶段 6 地基)
//
// 这里只做三件事:起 Qt 应用、把图标/主题准备好、开主窗口。
// 页面内容由后续批次补;命令行前端(sxcl-dl)与核心库保持独立,不受这里影响。
#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QLocale>
#include <QPainter>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyleHints>
#include <QTimer>

#include <climits>
#include <cstdio>

// 取证通路:动画参数复核(SXCL_ANIM_TRACE)的输出通道。
// Android 上 fprintf(stderr) 不进 logcat(实测:整份 logcat 里一条 [sxcl-ui] 都没有),
// 所以这里显式走 android log;桌面/其它平台仍旧走 stderr —— 两个平台是同一份采样代码。
#if defined(__ANDROID__)
#include <android/log.h>
#define SXCL_UI_TRACE(...) __android_log_print(ANDROID_LOG_INFO, "sxcl-ui", __VA_ARGS__)
#else
#define SXCL_UI_TRACE(...)                             \
    do {                                               \
        std::fprintf(stderr, "[sxcl-ui] " __VA_ARGS__); \
        std::fprintf(stderr, "\n");                    \
    } while (0)
#endif

#include "icon_registry.h"
#include "main_window.h"
#include "fluent_theme.h"
#include "theme_bridge.h"

// 账户(正版登录;Python 版无此功能,新增):主程序这边只需要两件事 ——
// 设置文件路径(uiSettingsPath)与取证用的登录对话框。
#include "dialogs/account.h"
#include "dialogs/auth_dialog.h"
#include "sxcl/lang.h"     // 语言表(键 -> 文案;.lang 与 Python 版逐字段兼容)
#include "sxcl/settings.h" // 启动恢复:ui.theme / ui.accent / ui.language(环境变量优先)

// ComboBox:弹出层取证要用它的 showPopup()
#include "fluent/fluent_setting_cards.h"

// 取证通路:把控件树按文本打出来(SXCL_UI_DUMP=1)。
//
// 为什么需要它:截图只能"看",而验收要求每条结论都给可核对的证据。
// 这份 dump 给的是**可以逐行读**的事实:账户组/各张卡片的类名与文字、按钮文案、
// 几何位置与可见性 —— 与 build/ref/TREE_py_*.txt 是同一类产物(只读,不改任何状态)。
static void dumpWidgetTree(QWidget *root, int maxDepth) {
    struct Walker {
        static void walk(QWidget *widget, int depth, int maxDepth) {
            if (widget == nullptr || depth > maxDepth)
                return;
            const QPoint topLeft = widget->mapTo(widget->window(), QPoint(0, 0));
            QString text;
            if (auto *label = qobject_cast<QLabel *>(widget))
                text = label->text();
            else if (auto *button = qobject_cast<QAbstractButton *>(widget))
                text = button->text();
            text.replace(QLatin1Char('\n'), QLatin1Char(' '));
            if (text.size() > 80)
                text = text.left(80) + QStringLiteral("…");
            std::fprintf(stderr, "%*s%s", depth * 2, "",
                         widget->metaObject()->className());
            if (!widget->objectName().isEmpty())
                std::fprintf(stderr, " #%s", widget->objectName().toUtf8().constData());
            std::fprintf(stderr, " (%d,%d %dx%d)%s%s", topLeft.x(), topLeft.y(), widget->width(),
                         widget->height(), widget->isVisible() ? "" : " hidden",
                         widget->isEnabled() ? "" : " disabled");
            if (!text.isEmpty())
                std::fprintf(stderr, " \"%s\"", text.toUtf8().constData());
            std::fprintf(stderr, "\n");
            const QList<QWidget *> children = widget->findChildren<QWidget *>(
                QString(), Qt::FindDirectChildrenOnly);
            for (QWidget *child : children)
                walk(child, depth + 1, maxDepth);
        }
    };
    Walker::walk(root, 0, maxDepth);
}
// ── 取证通路:动画参数复核(SXCL_ANIM_TRACE)──────────────────────────────
// 只读:不创建、不修改任何动画对象,只按帧记录"动画跑起来之后控件真正呈现出来的
// 位置/尺寸/透明度/遮罩"。桌面与 Android 共用这一份采样代码,所以两边打印的轨迹
// 可以直接对比 —— docs/08 的触屏一节引用的就是这里的输出。
//   SXCL_ANIM_TRACE=1:弹出层(Qt::Popup 顶层窗)出现时开始采样(位置/透明度/遮罩)
//   SXCL_ANIM_TRACE=2:另外走产品路径点一次导航汉堡按钮,同时采样面板宽度
namespace {

struct SxclTraceState {
    QElapsedTimer clock;
    QList<QString> rows;
    int last = INT_MIN;
    int stable = 0;
};

// metric: 0 = y(弹出层下落/pull-up),1 = width(导航面板展开/折叠)
void sxclTraceWidgetMotion(const QString &tag, QWidget *w, int timeoutMs, int metric) {
    auto *st = new SxclTraceState();
    st->clock.start();
    auto *timer = new QTimer(w);
    timer->setInterval(8); // 60fps 相邻两帧 ~16ms,8ms 采样不会漏掉关键帧
    QObject::connect(timer, &QTimer::timeout, w, [tag, w, timeoutMs, metric, st, timer]() {
        const qint64 t = st->clock.elapsed();
        const QRect maskRect = w->mask().boundingRect();
        const int value = metric == 1 ? w->width() : w->y();
        st->rows.append(QStringLiteral("%1 t=%2ms %3=%4 opacity=%5 mask=(%6,%7 %8x%9)")
                            .arg(tag)
                            .arg(t)
                            .arg(metric == 1 ? QStringLiteral("width") : QStringLiteral("y"))
                            .arg(value)
                            .arg(w->windowOpacity(), 0, 'f', 3)
                            .arg(maskRect.x())
                            .arg(maskRect.y())
                            .arg(maskRect.width())
                            .arg(maskRect.height()));
        st->stable = (value == st->last) ? st->stable + 1 : 0;
        st->last = value;
        if (t >= timeoutMs || st->stable >= 10) {
            timer->stop();
            for (const QString &row : st->rows)
                SXCL_UI_TRACE("%s", row.toUtf8().constData());
            SXCL_UI_TRACE("%s SUMMARY samples=%d settle=%lldms first=[%s] last=[%s]",
                          tag.toUtf8().constData(), int(st->rows.size()),
                          static_cast<long long>(t),
                          st->rows.isEmpty() ? "" : st->rows.first().toUtf8().constData(),
                          st->rows.isEmpty() ? "" : st->rows.last().toUtf8().constData());
            delete st;
        }
    });
    timer->start();
}

// 弹出层是独立顶层窗(Qt::Popup):Show 一到就开始采样,不需要知道它的类名。
class SxclAnimTraceFilter : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (ev->type() == QEvent::Show) {
            auto *w = qobject_cast<QWidget *>(obj);
            if (w != nullptr && w->isWindow() && (w->windowFlags() & Qt::Popup)
                && !w->property("sxclTraced").toBool()) {
                w->setProperty("sxclTraced", true);
                SXCL_UI_TRACE("popup SHOW class=%s rect=(%d,%d %dx%d) dpr=%.2f",
                              w->metaObject()->className(), w->x(), w->y(), w->width(),
                              w->height(), w->devicePixelRatioF());
                sxclTraceWidgetMotion(QStringLiteral("popup"), w, 600, 0);
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};

} // namespace

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    // Python main.py:96 —— app.setStyle("Fusion")。不设的话 Windows 默认样式的控件度量
    // 与 Python 端不一致(实测 QListWidget 行高 18 vs 16,列表逐行累积错位)。
    QApplication::setStyle(QStringLiteral("Fusion"));
    QApplication::setApplicationName(QStringLiteral("Silent X Craft Launcher"));
    QApplication::setOrganizationName(QStringLiteral("SilentStudio"));

    // 图标:PCL 抽出的矢量图,语义映射见 icon_registry.cpp 的表(依据 index.tsv)
    sxcl::ui::IconRegistry &icons = sxcl::ui::IconRegistry::instance();
    icons.load();
    int missing = 0;
    for (int i = 0; i < sxcl::ui::IconRegistry::Count; ++i) {
        if (!icons.has(sxcl::ui::IconRegistry::Semantic(i))) {
            std::fprintf(stderr, "[sxcl-ui] 图标缺失: %s (%s)\n",
                         icons.entry(sxcl::ui::IconRegistry::Semantic(i))
                             .file.toUtf8()
                             .constData(),
                         icons.iconDir().toUtf8().constData());
            ++missing;
        }
    }
    if (missing)
        std::fprintf(stderr, "[sxcl-ui] 共 %d 个图标没读到,界面会用空白图标继续跑\n",
                     missing);

    // ── 主题:1:1 主题层(令牌 = Python theme.py;样式 = qf 原版 QSS)──
    //
    // 启动恢复(新增):读我们自己的设置 sxcl_settings 里的 ui.theme / ui.accent / ui.language
    // 并**套用**,这样用户上次在设置页选的主题/强调色/语言重启后还在。
    // 优先级与核心库其它配置同一口径:**环境变量 > sxcl_settings > 内置默认**。
    // 环境变量优先是验收的硬要求:SXCL_UI_THEME / SXCL_UI_ACCENT 是抓参考图/取证时钉条件的
    // 开关(见 docs/05-UI-1to1规格.md §11),不能被本机配置悄悄改掉,否则取证不可复现。
    QString savedTheme;
    QString savedAccent;
    QString savedLanguage;
    {
        const QByteArray settingsPath = sxcl::ui::uiSettingsPath().toUtf8();
        if (sxcl_settings *settings = sxcl_settings_open(settingsPath.constData())) {
            // 取值一律走核心库的"启动期解析"(settings.h:环境变量优先,再设置文件,最后默认值):
            //   SXCL_UI_THEME / SXCL_UI_ACCENT / SXCL_UI_LANG(旧名 SXCL_UI_LANGUAGE 也认)
            // 这样"环境变量优先"这条规则只写在核心库一处,界面层不再自己判一遍。
            // 顺带:核心库会校验取值(主题只认 auto/light/dark,强调色只认 #rrggbb),
            // 写错的值一律回默认,不会把怪颜色套到界面上。
            if (const char *value = sxcl_settings_resolved_theme(settings))
                savedTheme = QString::fromUtf8(value);
            if (const char *value = sxcl_settings_resolved_accent(settings))
                savedAccent = QString::fromUtf8(value);
            if (const char *value = sxcl_settings_resolved_language(settings))
                savedLanguage = QString::fromUtf8(value);
            sxcl_settings_free(settings);
        }
    }

    sxcl::ui::FluentTheme &theme = sxcl::ui::FluentTheme::instance();
    const QString themeEnv = qEnvironmentVariable("SXCL_UI_THEME");
    const QString themeMode =
        (!themeEnv.isEmpty() ? themeEnv : savedTheme).trimmed().toLower();
    bool dark = true; // 内置默认:深色(C 版此前一直是这个默认值,不变)
    if (themeMode == QLatin1String("light")) {
        dark = false;
    } else if (themeMode == QLatin1String("auto")) {
        // 跟随系统:与设置页 _on_theme_changed 同一条判据(QStyleHints::colorScheme)。
        dark = QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light;
    }
    theme.setDark(dark);

    // 强调色:环境变量是**配置里的原始色**(docs/05 §11.1),setAccent 收的就是原始色,
    // 推导(qf ThemeColor)由主题层自己做 —— 这里不要预推一次。
    const QString accentEnv = qEnvironmentVariable("SXCL_UI_ACCENT");
    const QString accentText = !accentEnv.isEmpty() ? accentEnv : savedAccent;
    if (!accentText.isEmpty())
        theme.setAccent(QColor::fromString(accentText));
    theme.apply(&app);

    // 语言:设置页写 ui.language(zh-CN / en-US)。
    //   1) 语言表(核心库 include/sxcl/lang.h):内置中英两份,磁盘 <配置目录>/lang/<code>.lang
    //      或 SXCL_LANG_DIR 下的同名文件会逐键覆盖;查不到的键回落中文。
    //      **必须在这里设**:窗口与各页面随后才构造,页面取文案时表已就位(设置页再设一次是幂等)。
    //      界面层不用 Qt 的 .qm —— 文案表就在核心库里,离网/首次运行也有完整两份。
    //   2) Qt 区域设置:影响日期/数字格式与文件对话框。
    const QString languageEnv = qEnvironmentVariable("SXCL_UI_LANGUAGE");
    const QString language = !languageEnv.isEmpty() ? languageEnv : savedLanguage;
    if (!language.isEmpty())
        QLocale::setDefault(QLocale(language));

    char langErr[SXCL_LANG_ERR_MAX];
    langErr[0] = '\0';
    {
        const QByteArray code =
            (language.isEmpty() ? QStringLiteral("zh-CN") : language).toUtf8();
        const int rc = sxcl_lang_set_default(code.constData(), nullptr, langErr, sizeof(langErr));
        if (rc == SXCL_LANG_OK) {
            const sxcl_lang *lang = sxcl_lang_default();
            std::fprintf(stderr, "[sxcl-ui] 语言表: %s(%d 条)%s%s\n",
                         sxcl_lang_code(lang), (int)sxcl_lang_count(lang),
                         langErr[0] ? "; " : "", langErr);
        } else {
            std::fprintf(stderr, "[sxcl-ui] 语言表初始化失败(%d): %s\n", rc, langErr);
        }
    }

    std::fprintf(stderr, "[sxcl-ui] 主题: %s(mode=%s), 强调色 %s, 语言 %s, QSS %d 个文件(%s)\n",
                 theme.isDark() ? "深色" : "浅色",
                 themeMode.isEmpty() ? "(未设置)" : themeMode.toUtf8().constData(),
                 theme.accent().name().toUtf8().constData(),
                 language.isEmpty() ? "(未设置)" : language.toUtf8().constData(),
                 theme.qssFileCount(), theme.themeDir().toUtf8().constData());

    sxcl::ui::ThemeBridge::instance().refreshAll();

    sxcl::ui::MainWindow window;
    // 验收通路:用 SXCL_UI_ROUTE 指定起始路由(对应 build/ref/py_<route>.png)
    const QString route = qEnvironmentVariable("SXCL_UI_ROUTE");
    if (!route.isEmpty())
        window.switchToRoute(route);
    window.show();

    // 验收通路:自渲染截图(对应 Python 的 widget.grab(),不受其它窗口遮挡)
    //   SXCL_UI_SHOT=<png 路径> 时,窗口显示后抓图并退出
    const QString shot = qEnvironmentVariable("SXCL_UI_SHOT");

    // 取证通路:下拉弹出层截图(对应 Python tools/popup_capture_py.py)
    //   SXCL_UI_POPUP=<序号> + SXCL_UI_SHOT=<png 路径>:
    //   打开当前路由页里第 N 个 ComboBox 的弹出层(序号 = 可见 ComboBox 的 findChildren
    //   顺序,与 Python 端 page.findChildren(ComboBox) 同序),等 qf 动画跑完
    //   (250ms OutQuad)再 grab QApplication::activePopupWidget() 存图退出。
    //   这是成品的取证功能(见 docs/05-UI-1to1规格.md §11),不是临时诊断。
    const QString popup = qEnvironmentVariable("SXCL_UI_POPUP");
    if (!popup.isEmpty() && !shot.isEmpty()) {
        bool numeric = false;
        const int index = popup.toInt(&numeric);
        // (&app 要捕获:内层 singleShot 用它当 context 对象)
        QTimer::singleShot(1500, &app, [&app, &window, index, numeric, shot]() {
            if (!numeric || index < 0) {
                std::fprintf(stderr, "[sxcl-ui] SXCL_UI_POPUP 需要非负序号,收到 %d\n", index);
                QCoreApplication::quit();
                return;
            }
            // 光标停到固定点:qf 的 getCurrentScreenGeometry() 取【光标所在屏】的可用区域,
            // 弹层的 hover 高亮也取决于光标位置 —— 参考图脚本(popup_capture_py.py)同口径,
            // 否则同一份代码两次抓图的悬停行会不一样。
            QCursor::setPos(5, 5);

            QList<ComboBox *> combos;
            const QList<ComboBox *> all = window.findChildren<ComboBox *>();
            for (ComboBox *c : all) {
                if (c->isVisible())
                    combos.append(c);
            }
            if (index >= combos.size()) {
                std::fprintf(stderr, "[sxcl-ui] 弹出层 #%d 不存在(当前页可见下拉 %d 个)\n",
                             index, static_cast<int>(combos.size()));
                QCoreApplication::quit();
                return;
            }
            ComboBox *combo = combos.at(index);
            std::fprintf(stderr, "[sxcl-ui] 弹出层 #%d:下拉框 %dx%d 当前项 %d\n", index,
                         combo->width(), combo->height(), combo->currentIndex());
            combo->showPopup();
            QTimer::singleShot(700, &app, [shot]() {
                QWidget *pop = QApplication::activePopupWidget();
                if (!pop) {
                    std::fprintf(stderr, "[sxcl-ui] 弹出层截图失败:没有活动弹层\n");
                    QCoreApplication::quit();
                    return;
                }
                const QPixmap pm = pop->grab();
                const bool ok = pm.save(shot);
                std::fprintf(stderr, "[sxcl-ui] 弹层截图 %s %dx%d dpr=%.2f %s\n",
                             shot.toUtf8().constData(), pm.width(), pm.height(),
                             pm.devicePixelRatio(), ok ? "OK" : "FAILED");
                QCoreApplication::quit();
            });
        });
    }

    // 取证通路:登录对话框(SXCL_UI_AUTH_DIALOG=1)—— 用于"没有真实登录也要能演示"的验收。
    //   * 默认走真网络:能拿到真实的 user_code(用户手动完成那一步);
    //   * 配 SXCL_UI_AUTH_REPLAY=<tests/fixtures/auth> 走夹具回放(实测响应脱敏副本):
    //     即使不联网也能把整条链跑完;再加 SXCL_UI_AUTH_REPLAY_HOP6=1,
    //     第 6 跳会返回实测的 403 正文,**原样**显示在对话框里(见 docs/09 §9.2)。
    //
    // **走产品路径**:这里不自己 new 对话框,而是**点设置页上的「登录」按钮** ——
    // 这样"对话框由设置页打开 → 登录成功后设置页收到 accountChanged 去刷账户卡"
    // 也一起被验到(需配 SXCL_UI_ROUTE=settings)。
    if (qEnvironmentVariableIntValue("SXCL_UI_AUTH_DIALOG") == 1) {
        QTimer::singleShot(400, &app, [&window] {
            QWidget *page = window.sessionPage(QStringLiteral("settings"));
            if (page == nullptr) {
                std::fprintf(stderr,
                             "[sxcl-ui] SXCL_UI_AUTH_DIALOG 需要设置页:请加 SXCL_UI_ROUTE=settings\n");
                return;
            }
            const QList<PushSettingCard *> cards = page->findChildren<PushSettingCard *>();
            for (PushSettingCard *card : cards) {
                if (card->button() != nullptr &&
                    card->button()->text() == QStringLiteral("登录")) {
                    card->button()->click();
                    std::fprintf(stderr, "[sxcl-ui] 已点击设置页「账户 → 登录」\n");
                    return;
                }
            }
            std::fprintf(stderr, "[sxcl-ui] 设置页里没找到「登录」按钮\n");
        });
    }

    if (!shot.isEmpty() && popup.isEmpty()) {
        // SXCL_UI_SHOT_DELAY=<ms>:等对话框拿到 user_code / 走到第 6 跳再抓图(默认 1500)。
        const int delayMs = qEnvironmentVariableIntValue("SXCL_UI_SHOT_DELAY");
        const int waitMs = delayMs > 0 ? delayMs : 1500;
        QTimer::singleShot(waitMs, &app, [&window, shot]() {
            // 取证通路:SXCL_UI_SCROLL=bottom|<像素> —— 抓图前把当前页面的滚动区滚过去。
            // 账户组在设置页靠下的位置,不滚动的话默认视口里看不到(等价 Python 端
            // page.verticalScrollBar().setValue(...) 之后再 widget.grab())。
            const QString scrollSpec = qEnvironmentVariable("SXCL_UI_SCROLL");
            if (!scrollSpec.isEmpty() && window.pageStack() != nullptr) {
                QWidget *page = window.pageStack()->currentWidget();
                // 页面自己就是 ScrollArea(各页都继承 libqf 的 ScrollArea)→ 先按自己判,
                // 再退回找子控件(临时页那种外壳里嵌滚动区的结构)。
                QScrollArea *area = qobject_cast<QScrollArea *>(page);
                if (area == nullptr && page != nullptr)
                    area = page->findChild<QScrollArea *>();
                if (area != nullptr && area->verticalScrollBar() != nullptr) {
                    QScrollBar *bar = area->verticalScrollBar();
                    const bool toBottom =
                        scrollSpec.compare(QStringLiteral("bottom"), Qt::CaseInsensitive) == 0;
                    bar->setValue(toBottom ? bar->maximum() : scrollSpec.toInt());
                    std::fprintf(stderr, "[sxcl-ui] 滚动到 %d/%d\n", bar->value(), bar->maximum());
                }
            }
            if (qEnvironmentVariableIntValue("SXCL_UI_DUMP") == 1) {
                // 深度 6:页面 → 视口 → view → 分组 → 卡片 → 卡片里的标签/按钮
                // (少了这一层就只能看到卡片本身,看不到卡片上的文字 —— 实测踩过)
                std::fprintf(stderr, "[sxcl-ui] 控件树 dump(当前页面):\n");
                dumpWidgetTree(window.pageStack()->currentWidget(), 6);
                if (auto *dialog = window.findChild<sxcl::ui::AuthLoginDialog *>()) {
                    if (dialog->isVisible()) {
                        std::fprintf(stderr, "[sxcl-ui] 控件树 dump(登录对话框):\n");
                        dumpWidgetTree(dialog, 5);
                    }
                }
            }
            // libqf 的窗口开着 WA_TranslucentBackground(亚克力/Mica 区域在屏幕上是系统画的),
            // 直接 grab() 会得到 alpha=0 的洞。验收图必须与参考图同样不透明,
            // 所以先铺一层令牌底色(等价于"关掉系统合成"时用户看到的画面)再叠上去。
            const QPixmap raw = window.grab();
            QPixmap pm(raw.size());
            pm.setDevicePixelRatio(raw.devicePixelRatio());
            pm.fill(sxcl::ui::FluentTheme::instance().tokens().bg);
            {
                QPainter p(&pm);
                p.drawPixmap(0, 0, raw);
                // 遮罩对话框是独立的顶层窗口(QDialog),不在 window.grab() 里:
                // 它自己带整窗大小的遮罩,叠在 (0,0) 就是用户看到的画面。
                // 登录成功后对话框会自己关闭 —— 那时这里抓到的就是"账户卡已刷新"的设置页。
                auto *dialog = window.findChild<sxcl::ui::AuthLoginDialog *>();
                if (dialog != nullptr && dialog->isVisible()) {
                    p.drawPixmap(0, 0, dialog->grab());
                    std::fprintf(stderr, "[sxcl-ui] 登录对话框在画面上(已叠进截图)\n");
                }
            }
            const bool ok = pm.save(shot);
            std::fprintf(stderr, "[sxcl-ui] 截图 %s %dx%d %s\n", shot.toUtf8().constData(),
                         pm.width(), pm.height(), ok ? "OK" : "FAILED");
            QCoreApplication::quit();
        });
    }
    // 取证通路:动画参数复核(见本文件上方 SXCL_ANIM_TRACE 的说明)
    const int animTrace = qEnvironmentVariableIntValue("SXCL_ANIM_TRACE");
    if (animTrace >= 1) {
        qApp->installEventFilter(new SxclAnimTraceFilter());
        SXCL_UI_TRACE("anim trace on (level=%d)", animTrace);
        if (animTrace >= 2) {
            QTimer::singleShot(1200, &window, [&window] {
                sxcl::ui::NavPanel *nav = window.navPanel();
                if (nav == nullptr) {
                    SXCL_UI_TRACE("nav trace: no NavPanel");
                    return;
                }
                SXCL_UI_TRACE("nav toggle: click menu button (collapsed=%d width=%d; spec %dms "
                              "OutQuad %d<->%d)",
                              int(nav->collapsed()), nav->width(),
                              sxcl::ui::NavPanel::kExpandDurationMs,
                              sxcl::ui::NavPanel::kCollapsedWidth, sxcl::ui::NavPanel::kExpandedWidth);
                sxclTraceWidgetMotion(QStringLiteral("nav"), nav, 600, 1);
                nav->menuButton()->click();
            });
        }
    }

    return app.exec();
}