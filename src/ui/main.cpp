/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QCursor>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLocale>
#include <QPainter>
#include <QPixmap>
#include <QAbstractButton> // 验收钩子:点模组页的搜索按钮(可能是 PrimaryPushButton)
#include "crash_handler.h"   // 启动器自己的崩溃取证(未处理异常 -> logs/crashes/)
#include "ui2/ui2.h"         // 新界面(0.2.0 起的重写;SXCL_UI2=1 切换)
#include "sxcl/console.h"    // 控制台切 UTF-8(否则中文 stderr 在 936 下全是乱码)
#include <QColorDialog>  // 验收钩子:SXCL_UI_ACCENT_APPLY 要在真对话框里"挑一个颜色"
#include <QMouseEvent>   // 验收钩子:取色块靠 mouseReleaseEvent 开对话框,得真发一对鼠标事件
#include <QLineEdit>       // 验收钩子:SXCL_UI_MODS_QUERY 往搜索框里写字
#include <QPushButton> // 验收钩子:SXCL_UI_JRE_HOSTED 要找并点设置页上的「开始下载」按钮
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
#include "nav.h" // 验收钩子:SXCL_UI_NAV 要点侧二栏上那一条(NavPanel::button)
#include "fluent_theme.h"
#include "theme_bridge.h"

// 账户(正版登录;Python 版无此功能,新增):主程序这边只需要两件事 ——
// 设置文件路径(uiSettingsPath)与取证用的登录对话框。
#include "dialogs/account.h"
#include "dialogs/auth_dialog.h"
#include "workers/ui_paths.h" // uiSettingsFilePath / uiGameDirectory / uiGameDirectoryReason(启动清单)
#include "sxcl/lang.h"     // 语言表(键 -> 文案;.lang 与 Python 版逐字段兼容)
#include "sxcl/settings.h" // 启动恢复:ui.theme / ui.accent / ui.language(环境变量优先)

// ComboBox:弹出层取证要用它的 showPopup()
#include "fluent/fluent_segmented.h"   // Pivot:验收钩子要拨到「离线启动」那一档
#include "fluent/fluent_setting_cards.h"

// ── 运行日志(核心库 include/sxcl/log.h)──────────────────────────────────
// 进程一启动就开:文件 <配置目录>/logs/sxcl-YYYYMMDD-HHMMSS.log + stderr(桌面)/logcat(安卓)。
// 记什么/不记什么见 docs/17-运行日志.md 的清单 —— 只留下"能用来定位问题"的那些行。
#include "sxcl/launch.h" // sxcl_java_discover(启动时的 Java 清单;只读文件系统,不执行 java)
#include "sxcl/log.h"
#include "sxcl/sysinfo.h"
#include "sxcl/version.h"

namespace {

// 启动时的 Java 清单:用户报"我明明装了 Java"时,日志里必须有我们**看到过哪些**、
// 每个的版本/来源/路径。sxcl_java_discover 全程只读 <home>/release(launch.h:113-120),
// **不执行** java,所以这一步只是扫目录,不会把启动拖慢到需要异步。
void logStartupJavaList() {
    QElapsedTimer timer;
    timer.start();
    sxcl_java_env_store store;
    sxcl_java_info found[8];
    sxcl_java_env_capture(&store);
    const size_t count = sxcl_java_discover(&store.env, sxcl_java_current_os(), found, 8);
    SXCL_LOG_I("startup", "Java 检测:%llu 个可用安装(只读扫描,不执行 java;耗时 %lldms)",
               (unsigned long long)count, static_cast<long long>(timer.elapsed()));
    for (size_t i = 0; i < count; ++i) {
        SXCL_LOG_I("startup", "Java[%llu] major=%d version=%s 64bit=%d 来源=%s 路径=%s",
                   (unsigned long long)i, found[i].major,
                   found[i].version[0] != '\0' ? found[i].version : "(未知)",
                   found[i].is_64bit, found[i].source[0] != '\0' ? found[i].source : "(未知)",
                   found[i].path);
    }
    if (count == 0)
        SXCL_LOG_W("startup", "Java 检测:一个都没扫到 —— 下次启动游戏时会要求先装/指定 Java");
}

} // namespace

// 取证通路:把控件树按文本打出来(SXCL_UI_DUMP=1)。
//
// 为什么需要它:截图只能"看",而验收要求每条结论都给可核对的证据。
// 这份 dump 给的是**可以逐行读**的事实:账户组/各张卡片的类名与文字、按钮文案、
// 几何位置与可见性 —— 与 build/ref/TREE_py_*.txt 是同一类产物(只读,不改任何状态)。
// ── 文字度量(SXCL_UI_DUMP 的每一行都带上它)────────────────────────────────
//
// 「文字被挤压」在控件树里是**两个可量的数**:控件自己声明需要多大(sizeHint),
// 和布局实际给了多大(width/height)。只报"看起来挤了"没有依据,所以每个带文字的
// 控件都打三组数:
//
//   text=<w>x<h>  当前字体下这份文本的**自然**尺寸(单行宽 / 不换行高度)
//   need=<w>x<h>  控件按该文字 + 自己的内边距算出的首选尺寸(QWidget::sizeHint())
//   got =<w>x<h>  布局这次真的给了多少(QWidget::width()/height())
//
// 判据(两个布尔,直接印在行尾):
//   CUT-W  文字自然宽装不进"控件宽 - 自身内边距" -> 这一行在屏幕上会被切掉
//   CUT-H  文字自然高装不进"控件高 - 自身内边距" -> 行高不够(多行/换行文本)
// 内边距 = need - text(字体换了它不变,所以拿它当常数是安全的)。
//
// 依据:docs/05-UI-1to1规格.md §3 的字号表 + §7 的逐页结构;规格里那些数字都是
// **桌面 1100x750 + 桌面字体**下量出来的,安卓逻辑宽只有 800,同一份固定尺寸就会挤。
static QString textMetrics(QWidget *widget, const QString &full) {
    if (full.isEmpty())
        return QString();
    const QFontMetrics fm(widget->font());
    // 换行的标签:形态上要按最长的一行算(单行宽),高度按行数算
    const QStringList lines = full.split(QLatin1Char('\n'));
    int textW = 0;
    for (const QString &line : lines)
        textW = qMax(textW, fm.horizontalAdvance(line));
    QWidget *w = widget;
    const int textH = fm.height() * lines.size();
    const QSize hint = w->sizeHint();
    const int padW = qMax(0, hint.width() - textW);
    const int padH = qMax(0, hint.height() - textH);
    const bool cutW = w->width() > 0 && textW + padW > w->width();
    const bool cutH = w->height() > 0 && textH + padH > w->height();
    QString out = QStringLiteral(" [text=%1x%2 need=%3x%4 got=%5x%6]")
                      .arg(textW)
                      .arg(textH)
                      .arg(hint.width())
                      .arg(hint.height())
                      .arg(w->width())
                      .arg(w->height());
    if (cutW)
        out += QStringLiteral(" CUT-W");
    if (cutH)
        out += QStringLiteral(" CUT-H");
    return out;
}

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
            else if (auto *combo = qobject_cast<QComboBox *>(widget))
                text = combo->currentText();
            // 度量必须在**原样文本**上做(截断只影响打印,不影响需要多宽)
            const QString metrics = textMetrics(widget, text);
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
            if (!metrics.isEmpty())
                std::fprintf(stderr, "%s", metrics.toUtf8().constData());
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
    /* 新界面(docs/27)开关:**第一件事**就分流 —— 它自带 QApplication,
     * 所以不能等老界面把日志/QApplication 都建好再切(SXCL_UI2=1 时老界面一行不跑)。 */
    if (qEnvironmentVariableIntValue("SXCL_UI2") == 1) {
        return sxcl::ui2::run(argc, argv);
    }

    // 控制台按 UTF-8 解释我们的字节(不设的话中文在 936 代码页下糊成"鐣岄潰灏辩华")
    const int consoleCpBefore = sxcl_console_set_utf8();

    // ── 运行日志:进程一启动就开 —— "SXCL 启动开始"的每一条都留档 ──
    // 写不进去(目录/文件打不开)只影响日志本身:核心库静默降级,绝不拦住启动。
    // 安卓打包层已经先开过一次(它更早,能看到 boot 文件解析),这里是幂等的第二道。
    char logError[SXCL_LOG_ERROR_MAX];
    logError[0] = '\0';
    const int logRc = sxcl_log_init(nullptr, logError, sizeof(logError));
    SXCL_LOG_I("startup", "===== SXCL 启动 版本=%s 构建=%s %s =====", sxcl_version_string(),
               __DATE__, __TIME__);
    SXCL_LOG_I("startup", "平台=%s 内核=%s/%s 内存=%lluMB 逻辑核=%d pid=%llu 命令行参数=%d",
               QSysInfo::prettyProductName().toUtf8().constData(),
               QSysInfo::kernelType().toUtf8().constData(),
               QSysInfo::currentCpuArchitecture().toUtf8().constData(),
               (unsigned long long)sxcl_sysinfo_total_mb(), sxcl_sysinfo_cpu_logical(),
               (unsigned long long)QCoreApplication::applicationPid(), argc - 1);
    SXCL_LOG_I("startup", "日志文件=%s 上限=%llu 字节 保留=%d 份%s%s", sxcl_log_file_path(),
               (unsigned long long)sxcl_log_max_bytes(), sxcl_log_keep_files(),
               logRc != SXCL_LOG_OK ? " 打不开:" : "", logRc != SXCL_LOG_OK ? logError : "");
    /* 崩溃取证:**日志一开就装** —— 晚装一步,这个窗口期里崩掉就什么都没留下
     * (用户报"切主题色直接崩"时我们正是手上什么都没有)。 */
    sxcl::ui::installCrashHandler();
    sxcl::ui::installHangWatchdog(3000); // 界面卡住也要留现场(用户报过"启动游戏未响应")
    SXCL_LOG_I("startup", "控制台代码页: 切之前=%d 现在=%s(中文乱码就是这个 936 惹的)",
               consoleCpBefore, "UTF-8(65001)");

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

    // ── 启动清单进运行日志(见 docs/17-运行日志.md 的"选择性记录"清单)──
    // 记这几样就够回答"这台机器上启动器看到了什么":设置文件、游戏目录(**含为什么选它**)、
    // 主题/语言、Java 清单。不记 Qt 内部噪声、不记每秒刷屏的进度。
    SXCL_LOG_I("startup", "Qt=%s 样式=Fusion 缩放=%s 语言表=%s(%d 条)",
               qVersion(), qEnvironmentVariable("QT_SCALE_FACTOR", "1").toUtf8().constData(),
               sxcl_lang_code(sxcl_lang_default()),
               static_cast<int>(sxcl_lang_count(sxcl_lang_default())));
    SXCL_LOG_I("startup", "设置文件=%s", sxcl::ui::uiSettingsFilePath().toUtf8().constData());
    {
        // uiGameDirectory() 可能触发安卓的自动探测与落盘(与设置页同一条路),顺便拿到依据
        const QString gameDir = sxcl::ui::uiGameDirectory();
        SXCL_LOG_I("startup", "游戏目录=%s(选择依据:%s)", gameDir.toUtf8().constData(),
                   sxcl::ui::uiGameDirectoryReason().toUtf8().constData());
    }
    SXCL_LOG_I("startup", "主题=%s(mode=%s) 强调色=%s 语言=%s QSS=%d 个文件",
               theme.isDark() ? "深色" : "浅色",
               themeMode.isEmpty() ? "(未设置)" : themeMode.toUtf8().constData(),
               theme.accent().name().toUtf8().constData(),
               language.isEmpty() ? "(未设置)" : language.toUtf8().constData(),
               theme.qssFileCount());
    logStartupJavaList();

    sxcl::ui::ThemeBridge::instance().refreshAll();

        /* 语义图标表(assets/icons/pcl/*.svg -> IconRegistry)必须在**建界面之前**加载:
     * 下载页那层"双层侧边栏"里的 MOD / 光影 用的就是它(NavItem.semantic)。
     * 以前只有版本选择页(文件夹图标)顺手加载过一次 —— 用户不进那一页,那两格就是**空白图标**
     * (用户 2026-09-22 晚点名:「MOD 和光影都没显示图标」)。 */
    if (sxcl::ui::IconRegistry::instance().resolveIconDir()) {
        (void)sxcl::ui::IconRegistry::instance().load();
    }
    std::fprintf(stderr, "[sxcl-ui] 语义图标: 目录=%s mod=%d shader=%d 条目=%d\n",
                 sxcl::ui::IconRegistry::instance().iconDir().toUtf8().constData(),
                 (int)sxcl::ui::IconRegistry::instance().has(sxcl::ui::IconRegistry::Mod),
                 (int)sxcl::ui::IconRegistry::instance().has(sxcl::ui::IconRegistry::Shader),
                 (int)sxcl::ui::IconRegistry::instance().count());

sxcl::ui::MainWindow window;

    // ── 取证通路:强制窗口**逻辑**尺寸(SXCL_UI_WINDOW=<宽>x<高>)──────────────
    // 为什么需要它:桌面 1:1 参考图的口径是 1100x750(内容区 1052x702),而安卓手机给
    // 全屏窗口的**逻辑**尺寸完全不同 —— 实测 G6012BS(1600x2400 物理 @density 320,
    // dpr=2.0)就是 800x1200(内容区 752x1152):**宽度少 300px、高度多 450px**。
    // "文字被挤压"必须在这个尺寸下量,桌面尺寸下量不出来。
    // 只改窗口大小,其余一切照产品路径走(还是那个 MainWindow、那些页面),不造假状态。
    // 最小尺寸也要放开:安卓上窗口尺寸由系统给(setMinimumSize 只是建议、平台不执行),
    // 不放开会停在 900x600 上,量到的就不是手机的真实可用区。
    const QString windowSpec = qEnvironmentVariable("SXCL_UI_WINDOW");
    if (!windowSpec.isEmpty()) {
        const QStringList parts = windowSpec.split(QLatin1Char('x'), Qt::SkipEmptyParts);
        bool okW = false, okH = false;
        const int w = parts.size() > 0 ? parts.at(0).toInt(&okW) : 0;
        const int h = parts.size() > 1 ? parts.at(1).toInt(&okH) : 0;
        if (okW && okH && w > 0 && h > 0) {
            window.setMinimumSize(0, 0);
            window.resize(w, h);
            std::fprintf(stderr, "[sxcl-ui] 强制窗口尺寸 %dx%d(SXCL_UI_WINDOW)\n", w, h);
        } else {
            std::fprintf(stderr,
                         "[sxcl-ui] SXCL_UI_WINDOW 要写成 <宽>x<高>(收到 \"%s\")\n",
                         windowSpec.toUtf8().constData());
        }
    }

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

    // 验收通路:自托管 JRE(设置页「游戏设置 → Java」组里的「内置 JRE（自托管）」卡片)
    //   SXCL_UI_JRE_HOSTED=<主版本>(如 17):在设置页里选中该主版本,再**点真正的「开始下载」按钮** ——
    //   与上面 SXCL_UI_AUTH_DIALOG 同一个口径:走产品路径(点界面),不在这里自己调核心库
    //   (那样验的就只是核心库,验不到"设置页到底接上没有")。
    //   来源与落点**本钩子一概不碰**,由环境的设置文件/环境变量决定:
    //     SXCL_UI_SETTINGS      设置文件(里面放 java.jre_index_url = index.json 地址)
    //     SXCL_JAVA_JRE_INDEX_URL 环境变量来源(优先级高于设置项)
    //     SXCL_RUNTIME_DIR      核心库的运行时根覆盖(验收必须指到临时目录)
    //   核心库回调与界面读数走 stderr(SXCL_UI_TRACE=1);截图用 SXCL_UI_SHOT/SXCL_UI_SHOT_DELAY。
    const QString hostedMajor = qEnvironmentVariable("SXCL_UI_JRE_HOSTED");
    if (!hostedMajor.isEmpty()) {
        QTimer::singleShot(600, &app, [&window, hostedMajor] {
            QWidget *page = window.sessionPage(QStringLiteral("settings"));
            if (page == nullptr) {
                std::fprintf(stderr,
                             "[sxcl-ui] SXCL_UI_JRE_HOSTED 需要设置页:请加 SXCL_UI_ROUTE=settings\n");
                return;
            }
            const int want = hostedMajor.toInt();
            if (auto *combo = page->findChild<QComboBox *>(QStringLiteral("hostedJreComponent"))) {
                for (int i = 0; i < combo->count(); ++i) {
                    if (combo->itemData(i).toInt() == want) {
                        combo->setCurrentIndex(i);
                        break;
                    }
                }
                std::fprintf(stderr, "[sxcl-ui] 内置 JRE:选定 Java %d(下拉第 %d 项)\n", want,
                             combo->currentIndex());
            } else {
                std::fprintf(stderr, "[sxcl-ui] 设置页里没找到「内置 JRE」的组件下拉\n");
            }
            if (auto *button = page->findChild<QPushButton *>(QStringLiteral("hostedJreDownloadButton"))) {
                button->click();
                std::fprintf(stderr,
                             "[sxcl-ui] 已点击设置页「Java → 内置 JRE（自托管）→ 开始下载」\n");
            } else {
                std::fprintf(stderr, "[sxcl-ui] 设置页里没找到内置 JRE 的「开始下载」按钮\n");
            }
        });
    }

    // 验收通路:直接打开**下载配置页**(SXCL_UI_CONFIG=<版本号>,如 1.20.1)。
    //   加载器列表(Forge/NeoForge/Fabric/Quilt/OptiFine)就在这一页 —— 没有它只能靠手点版本行进去。
    const QString configVersion = qEnvironmentVariable("SXCL_UI_CONFIG");
    if (!configVersion.isEmpty()) {
        QTimer::singleShot(400, &app, [&window, configVersion]() {
            if (qEnvironmentVariableIsSet("SXCL_UI_TRACE")) {
                std::fprintf(stderr, "[sxcl-ui] 打开下载配置页:%s(SXCL_UI_CONFIG)\n",
                             configVersion.toUtf8().constData());
            }
            QMetaObject::invokeMethod(&window, "switchToDownloadConfig", Qt::DirectConnection,
                                      Q_ARG(QString, configVersion));
        });
    }

    // 验收通路:滚动条淡出压力测试(SXCL_UI_SCROLLBAR_STRESS=N)。
    //   为什么要有它:真机崩了两次(07:41 QAbstractAnimation::stop、08:07 QObject::deleteLater),
    //   符号栈都指向 libqf 的 ScrollBar::fadeTo —— 那里动画跑完会 deleteLater 自杀,
    //   而 m_fadeAni 没清空,下一次 fadeTo 就在野指针上 stop()/deleteLater()。
    //   这里**不碰鼠标**,直接给滚动条发 Enter/Leave 事件(leave 就会 fadeTo),
    //   每 200ms 一次,足够让上一个动画跑完并被 deleteLater 掉 —— 老代码必崩,
    //   修好后必须活下来。这就是"崩溃可复现"的那把尺子。
    const int scrollStress = qEnvironmentVariableIntValue("SXCL_UI_SCROLLBAR_STRESS");
    if (scrollStress > 0) {
        QTimer::singleShot(1500, &app, [&app, scrollStress]() {
            QWidget *bar = nullptr;
            for (QWidget *w : QApplication::allWidgets()) {
                if (w->isVisible() &&
                    QString::fromLatin1(w->metaObject()->className()).endsWith(QLatin1String("ScrollBar"))) {
                    bar = w;
                    break;
                }
            }
            if (bar == nullptr) {
                std::fprintf(stderr, "[sxcl-ui] STRESS: 没找到可见的滚动条\n");
                return;
            }
            auto *state = new int(0);
            auto *timer = new QTimer(&app);
            QObject::connect(timer, &QTimer::timeout, &app, [bar, state, scrollStress]() {
                *state += 1;
                if (*state > scrollStress) {
                    std::fprintf(stderr, "[sxcl-ui] STRESS: %d 次 Enter/Leave 之后**还活着**\n", scrollStress);
                    QCoreApplication::quit();
                    return;
                }
                QEvent enter(QEvent::Enter);
                QApplication::sendEvent(bar, &enter);
                QEvent leave(QEvent::Leave);
                QApplication::sendEvent(bar, &leave);
                std::fprintf(stderr, "[sxcl-ui] STRESS: 第 %d 次\n", *state);
            });
            timer->start(200);
        });
    }

    // 验收通路:故意崩一次(SXCL_UI_CRASH_TEST=1)。
    //   证明"启动器自己崩了会留下东西":logs/crashes/ 下应当出现
    //   sxcl-ui-crash-<时间>.txt(异常码 + 出错模块 + 符号化调用栈 + 日志路径)与同名 .dmp。
    if (qEnvironmentVariableIntValue("SXCL_UI_CRASH_TEST") == 1) {
        QTimer::singleShot(1500, &app, []() {
            std::fprintf(stderr, "[sxcl-ui] CRASH-TEST: 故意制造一个访问违例\n");
            std::fflush(stderr);
            volatile int *boom = reinterpret_cast<volatile int *>(0);
            *boom = 1; // 让编译器别把这段优化掉
        });
    }

    // 验收通路:在**设置页**里改强调色(SXCL_UI_ACCENT_APPLY='#ff8800')。
    //   为什么要有它:用户报「切主题色直接把窗口搞崩」—— 崩溃只能靠**真路径**复现,
    //   而这条路的每一步都不在"调核心库"上:切设置页 -> 点真取色块(ColorPickerButton,
    //   它靠 mouseReleaseEvent 开 QColorDialog)-> 对话框里选色确定 -> 设置页的
    //   colorChanged -> FluentTheme::setAccent + onThemeChanged(重套 QSS/调色板/广播)。
    //   所以这里发**一对真鼠标事件**,再在对话框的嵌套事件循环里把颜色定下来。
    const QString accentApply = qEnvironmentVariable("SXCL_UI_ACCENT_APPLY");
    if (!accentApply.isEmpty()) {
        QTimer::singleShot(700, &app, [&app, &window, accentApply]() {
            QMetaObject::invokeMethod(&window, "switchToRoute", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("settings")));
            std::fprintf(stderr, "[sxcl-ui] ACCENT: 已切到设置页,准备点取色块 -> %s\n",
                         accentApply.toUtf8().constData());
            /* 对话框是**模态**(QColorDialog::getColor 自己跑嵌套事件循环),所以这个定时器
             * 是在对话框开着的时候才到点的 —— 正好用来"替用户挑一个颜色再确定"。 */
            QTimer::singleShot(1200, &app, [&app, accentApply]() {
                QWidget *modal = QApplication::activeModalWidget();
                std::fprintf(stderr, "[sxcl-ui] ACCENT: 模态窗口=%s\n",
                             modal != nullptr ? modal->metaObject()->className() : "(没有)");
                if (auto *cd = qobject_cast<QColorDialog *>(modal)) {
                    cd->setCurrentColor(QColor(accentApply));
                    std::fprintf(stderr, "[sxcl-ui] ACCENT: 在对话框里选色 %s 并确定\n",
                                 accentApply.toUtf8().constData());
                    cd->accept();
                }
            });
            QWidget *picker = nullptr;
            const QList<QWidget *> all = QApplication::allWidgets();
            for (QWidget *w : all) {
                if (w->isVisible() &&
                    QString::fromLatin1(w->metaObject()->className()) ==
                        QLatin1String("ColorPickerButton")) {
                    picker = w;
                    break;
                }
            }
            if (picker == nullptr) {
                std::fprintf(stderr, "[sxcl-ui] ACCENT: 设置页里没找到取色块\n");
                return;
            }
            const QPointF pos(8.0, 8.0);
            QMouseEvent press(QEvent::MouseButtonPress, pos, picker->mapToGlobal(pos.toPoint()),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease, pos,
                                picker->mapToGlobal(pos.toPoint()), Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            std::fprintf(stderr, "[sxcl-ui] ACCENT: 点取色块 %s -> 等对话框\n",
                         picker->metaObject()->className());
            QApplication::sendEvent(picker, &press);
            QApplication::sendEvent(picker, &release);   // 这里会阻塞到对话框关闭
            std::fprintf(stderr, "[sxcl-ui] ACCENT: 取色块已经返回(对话框关掉了)\n");
        });
    }

    // 验收通路:在**设置页**里切换主题模式(SXCL_UI_THEME_SWITCH=light|dark|auto)。
    //   与上一条同一个理由:用户报"切主题把窗口搞崩",而这条路要真的走
    //   设置页那张卡(下拉 -> indexChanged -> onThemeChanged -> FluentTheme::apply ->
    //   libqf setTheme/setThemeColor -> 广播 -> ThemeBridge 重刷所有登记窗口)。
    //   这里**不点下拉**(ComboBox 的弹层是另一套代码),直接把**真下拉**的当前项改掉 ——
    //   这是产品路径上"用户选了另一项"那一步,信号链一模一样。
    const QString themeSwitch = qEnvironmentVariable("SXCL_UI_THEME_SWITCH");
    if (!themeSwitch.isEmpty()) {
        QTimer::singleShot(700, &app, [&app, &window, themeSwitch]() {
            QMetaObject::invokeMethod(&window, "switchToRoute", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("settings")));
            const QString want = themeSwitch == QLatin1String("light") ? QStringLiteral("浅色")
                                 : themeSwitch == QLatin1String("dark") ? QStringLiteral("深色")
                                                                        : QStringLiteral("跟随系统");
            QTimer::singleShot(500, &app, [&window, want]() {
                const QList<QComboBox *> boxes = window.findChildren<QComboBox *>();
                for (QComboBox *box : boxes) {
                    int hit = -1;
                    for (int i = 0; i < box->count(); ++i) {
                        if (box->itemText(i).contains(want))
                            hit = i;
                    }
                    if (hit < 0 || !box->isVisible())
                        continue;
                    std::fprintf(stderr, "[sxcl-ui] THEME: 下拉里选「%s」(第 %d 项)\n",
                                 want.toUtf8().constData(), hit);
                    box->setCurrentIndex(hit);
                    std::fprintf(stderr, "[sxcl-ui] THEME: setCurrentIndex 返回(没崩)\n");
                    return;
                }
                std::fprintf(stderr, "[sxcl-ui] THEME: 设置页里没找到主题下拉\n");
            });
        });
    }

    // 验收通路:直接**走一遍下载/安装**(SXCL_UI_DOWNLOAD="<MC>|<实例名>|<加载器>|<加载器版本>")。
    //   调的就是配置页那个「开始下载」按钮调的方法(MainWindow::switchToDownloadProgress),
    //   于是 InstallWorker 整条链(原版 + 加载器)都会真的跑起来 —— 用来验收
    //   "GUI 里装 Quilt"(它不走安装器 jar,走 meta 直装,见 install_worker 的 quiltFromMeta)。
    const QString downloadSpec = qEnvironmentVariable("SXCL_UI_DOWNLOAD");
    if (!downloadSpec.isEmpty()) {
        QTimer::singleShot(700, &app, [&window, downloadSpec]() {
            const QStringList parts = downloadSpec.split(QLatin1Char('|'));
            const QString mc = parts.value(0);
            const QString instance = parts.value(1, mc);
            const QString loader = parts.value(2);
            const QString loaderVersion = parts.value(3);
            std::fprintf(stderr, "[sxcl-ui] 开始安装:MC=%s 实例=%s 加载器=%s %s(SXCL_UI_DOWNLOAD)\n",
                         mc.toUtf8().constData(), instance.toUtf8().constData(),
                         loader.isEmpty() ? "(无)" : loader.toUtf8().constData(),
                         loaderVersion.toUtf8().constData());
            QMetaObject::invokeMethod(&window, "switchToDownloadProgress", Qt::DirectConnection,
                                      Q_ARG(QString, mc), Q_ARG(QString, instance),
                                      Q_ARG(QString, loader), Q_ARG(QString, loaderVersion));
        });
    }

    // 验收通路:进**子栏**(下载页左侧那三个:Minecraft 版本 / MOD / 光影;版本选择页的文件夹也是)。
    //   SXCL_UI_NAV=<routeKey>(如 download_mod):窗口起来后**点那一条** ——
    //   走产品路径(按钮 clicked -> NavPanel::setCurrent -> routeChanged -> 切 stack),
    //   不在这里自己去 setCurrentIndex(那样验不到接线本身,与 SXCL_UI_JRE_HOSTED 同一个口径)。
    const QString navKey = qEnvironmentVariable("SXCL_UI_NAV");
    if (!navKey.isEmpty()) {
        QTimer::singleShot(400, &app, [&window, navKey]() {
            const QList<sxcl::ui::NavPanel *> panels = window.findChildren<sxcl::ui::NavPanel *>();
            for (sxcl::ui::NavPanel *panel : panels) {
                if (NavigationPushButton *btn = panel->button(navKey)) {
                    btn->click();
                    std::fprintf(stderr, "[sxcl-ui] 子栏 %s 已点(SXCL_UI_NAV)\n",
                                 navKey.toUtf8().constData());
                    return;
                }
            }
            std::fprintf(stderr, "[sxcl-ui] 找不到子栏条目 %s(SXCL_UI_NAV)\n",
                         navKey.toUtf8().constData());
        });
    }

    // 验收通路:模组页真的搜一次(SXCL_UI_MODS_QUERY=<关键词>)。
    //   往**真控件**里写字、点**真的搜索按钮** —— 走产品路径(工作线程取 JSON -> 解析 -> 出卡片),
    //   与 SXCL_UI_JRE_HOSTED 同一个口径:验的是接线,不是"main.cpp 里自己调核心库"。
    const QString modsQuery = qEnvironmentVariable("SXCL_UI_MODS_QUERY");
    if (!modsQuery.isEmpty()) {
        QTimer::singleShot(600, &app, [&window, modsQuery]() {
            /* 模组页在下载页的 stack 里有**两份**(MOD 那一栏与光影那一栏),
             * 两份的搜索框同名 —— 必须挑**看得见的那一份**(用户眼前那个),
             * 否则会往隐藏的那一页里打字(实测踩到:dump 里中招的是隐藏页,可见页毫无反应)。 */
            QLineEdit *box = nullptr;
            const QList<QLineEdit *> boxes =
                window.findChildren<QLineEdit *>(QStringLiteral("modsSearchBox"));
            for (QLineEdit *candidate : boxes) {
                if (candidate->isVisible()) {
                    box = candidate;
                    break;
                }
            }
            QAbstractButton *btn = nullptr;
            if (box != nullptr && box->parentWidget() != nullptr) {
                btn = box->parentWidget()->findChild<QAbstractButton *>(
                    QStringLiteral("modsSearchButton"));
            }
            if (box == nullptr || btn == nullptr) {
                std::fprintf(stderr,
                             "[sxcl-ui] 找不到模组页的搜索框/按钮(SXCL_UI_MODS_QUERY 需要 "
                             "SXCL_UI_ROUTE=download + SXCL_UI_NAV=download_mod|download_shader)\n");
                return;
            }
            box->setText(modsQuery);
            btn->click();
            std::fprintf(stderr, "[sxcl-ui] 模组页已点搜索(可见页):%s\n",
                         modsQuery.toUtf8().constData());
        });
    }

    // 验收通路:点模组页第 N 个「装」(SXCL_UI_MODS_INSTALL=N,1 起;0/不设 = 不点)。
    //   搜索结果是异步回来的,所以这一步**排在搜索之后**(默认等 7s,可用
    //   SXCL_UI_MODS_INSTALL_DELAY 调);点的是卡片上真的那个按钮。
    const int installIndex = qEnvironmentVariableIntValue("SXCL_UI_MODS_INSTALL");
    if (installIndex > 0) {
        const int installDelay = qEnvironmentVariableIntValue("SXCL_UI_MODS_INSTALL_DELAY");
        QTimer::singleShot(installDelay > 0 ? installDelay : 7000, &app, [&window, installIndex]() {
            QList<QAbstractButton *> visible;
            const QList<QAbstractButton *> all =
                window.findChildren<QAbstractButton *>(QStringLiteral("modsInstallButton"));
            for (QAbstractButton *candidate : all) {
                if (candidate->isVisible()) {
                    visible.append(candidate);
                }
            }
            if (visible.size() < installIndex) {
                std::fprintf(stderr, "[sxcl-ui] 第 %d 个「装」不存在(当前可见 %d 个)\n", installIndex,
                             static_cast<int>(visible.size()));
                return;
            }
            visible.at(installIndex - 1)->click();
            std::fprintf(stderr, "[sxcl-ui] 模组页已点第 %d 个「装」\n", installIndex);
        });
    }

    // 验收通路:从**主页点「启动」**(SXCL_UI_LAUNCH=1)。
    //   走产品路径:点真按钮 -> HomePage::launchOffline -> 切到启动页 -> LaunchWorker 跑
    //   (补全文件 / 选 Java / 起进程)。这样"界面这条启动链"在真机上也能被验到。
    if (qEnvironmentVariableIntValue("SXCL_UI_LAUNCH") == 1) {
        const int launchDelay = qEnvironmentVariableIntValue("SXCL_UI_LAUNCH_DELAY");
        QTimer::singleShot(launchDelay > 0 ? launchDelay : 800, &app, [&window]() {
            /* 先把登录方式滑块拨到「离线启动」:有已登录账户时主页会自动停在「正版登录」那一档,
             * 直接按正版启动会拿**真账户**去跑游戏 —— 验收不能这么干。 */
            const QList<Pivot *> pivots = window.findChildren<Pivot *>();
            for (Pivot *pivot : pivots) {
                if (pivot->item(QStringLiteral("offline")) != nullptr) {
                    pivot->setCurrentItem(QStringLiteral("offline"));
                    break;
                }
            }
            /* 按 objectName 找那个按钮。**不要求可见**:登录滑块可能还停在「正版登录」那一档
             * (有已登录账户时主页会自动停在那里),离线卡片就是 hidden 的 —— 但点它的
             * clicked 照样走 launchOffline()。可见的那一个优先。 */
            QAbstractButton *button = nullptr;
            const QList<QAbstractButton *> found =
                window.findChildren<QAbstractButton *>(QStringLiteral("homeOfflineLaunchButton"));
            std::fprintf(stderr, "[sxcl-ui] 主页启动按钮候选 %d 个\n", (int)found.size());
            for (QAbstractButton *candidate : found) {
                if (candidate->isVisible()) {
                    button = candidate;
                    break;
                }
            }
            if (button == nullptr && !found.isEmpty()) {
                button = found.first();
            }
            if (button == nullptr) {
                std::fprintf(stderr,
                             "[sxcl-ui] 找不到主页的「启动」按钮(SXCL_UI_LAUNCH 需要 "
                             "SXCL_UI_ROUTE=home,并且设置里有 game.selected_version)\n");
                return;
            }
            button->click();
            std::fprintf(stderr, "[sxcl-ui] 主页已点「启动」(SXCL_UI_LAUNCH)\n");
        });
    }

    // 验收通路:把当前页面里那条"侧2"收起来(SXCL_UI_COLLAPSE=1)。
    //   折叠态的几何(面板 48 宽、按钮 40x36)只能靠图看 —— 用户点名"缩回来不是侧1 的方形",
    //   所以给一张能复现的取证图。用 NavPanel::setCollapsed(true)(面板自己的公开 API)。
    if (qEnvironmentVariableIntValue("SXCL_UI_COLLAPSE") == 1) {
        const int collapseDelay = qEnvironmentVariableIntValue("SXCL_UI_COLLAPSE_DELAY");
        QTimer::singleShot(collapseDelay > 0 ? collapseDelay : 1200, &app, [&window]() {
            QWidget *page = window.pageStack() != nullptr ? window.pageStack()->currentWidget()
                                                          : nullptr;
            if (page == nullptr) {
                return;
            }
            const QList<sxcl::ui::NavPanel *> panels = page->findChildren<sxcl::ui::NavPanel *>();
            for (sxcl::ui::NavPanel *panel : panels) {
                panel->setCollapsed(true);
                std::fprintf(stderr, "[sxcl-ui] 侧2 已折叠(SXCL_UI_COLLAPSE)\n");
            }
        });
    }

    // 验收通路:打开版本选择页里**文件夹那一行的齿轮**(自定义图标弹窗),抓它本身。
    //   SXCL_UI_ICON_POPUP=1 + SXCL_UI_SHOT=... :点第一行的动作按钮(NavPanel::actionButton),
    //   然后抓活动弹层 —— 那个弹窗是 Qt::Popup 顶层窗,window.grab() 抓不到它。
    if (qEnvironmentVariableIntValue("SXCL_UI_ICON_POPUP") == 1 && !shot.isEmpty()) {
        QTimer::singleShot(1500, &app, [&window, &app, shot]() {
            QWidget *page = window.pageStack() != nullptr ? window.pageStack()->currentWidget()
                                                          : nullptr;
            sxcl::ui::NavPanel *panel =
                page != nullptr ? page->findChild<sxcl::ui::NavPanel *>() : nullptr;
            /* 直接触发 NavPanel::itemAction(那个齿轮被点时发的就是它)——
             * 齿轮本身是 qf 的 NavToolButton,不是 QAbstractButton,没有 click() 可调。 */
            QString route;
            if (panel != nullptr) {
                for (const sxcl::ui::NavItem &item : panel->items()) {
                    if (!item.actionIcon.isEmpty()) {
                        route = item.routeKey;
                        break;
                    }
                }
            }
            if (route.isEmpty()) {
                std::fprintf(stderr, "[sxcl-ui] 找不到带齿轮的文件夹行(需要 SXCL_UI_ROUTE=select)\n");
                QCoreApplication::quit();
                return;
            }
            QMetaObject::invokeMethod(panel, "itemAction", Qt::DirectConnection,
                                      Q_ARG(QString, route));
            std::fprintf(stderr, "[sxcl-ui] 已点文件夹行的齿轮(自定义图标弹窗)\n");
            QTimer::singleShot(700, &app, [shot]() {
                QWidget *pop = QApplication::activePopupWidget();
                if (pop == nullptr) {
                    std::fprintf(stderr, "[sxcl-ui] 图标弹窗没起来\n");
                    QCoreApplication::quit();
                    return;
                }
                if (qEnvironmentVariableIntValue("SXCL_UI_DUMP") == 1) {
                    std::fprintf(stderr, "[sxcl-ui] 控件树 dump(图标弹窗):\n");
                    dumpWidgetTree(pop, 5);
                }
                const QPixmap pm = pop->grab();
                const bool ok = pm.save(shot);
                std::fprintf(stderr, "[sxcl-ui] 图标弹窗截图 %s %dx%d %s\n",
                             shot.toUtf8().constData(), pm.width(), pm.height(), ok ? "OK" : "FAILED");
                QCoreApplication::quit();
            });
        });
    }

    if (!shot.isEmpty() && popup.isEmpty() && qEnvironmentVariableIntValue("SXCL_UI_ICON_POPUP") != 1) {
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

    SXCL_LOG_I("startup", "界面就绪:进入事件循环(路由=%s)",
               route.isEmpty() ? "(默认首页)" : route.toUtf8().constData());
    const int rc = app.exec();

    // 收尾:把"这次运行记了多少"写进日志再关文件 —— 验收要的行数直接看这一行。
    {
        sxcl_log_stats stats;
        sxcl_log_get_stats(&stats);
        // 统计取自**本行之前**:所以文件里的总行数 = 这个数 + 1(就是这一行本身)。
        SXCL_LOG_I("startup", "===== SXCL 退出 rc=%d 退出前累计行数=%llu 字节=%llu 轮转=%u 丢弃=%llu "
                              "文件=%s =====",
                   rc, stats.lines, stats.bytes, stats.rotations, stats.dropped,
                   sxcl_log_file_path());
    }
    sxcl_log_shutdown();
    return rc;
}