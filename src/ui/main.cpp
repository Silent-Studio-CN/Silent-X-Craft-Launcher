// sxcl-ui — Silent X Craft Launcher 图形前端入口(阶段 6 地基)
//
// 这里只做三件事:起 Qt 应用、把图标/主题准备好、开主窗口。
// 页面内容由后续批次补;命令行前端(sxcl-dl)与核心库保持独立,不受这里影响。
#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QPainter>
#include <QPixmap>
#include <QTimer>

#include <cstdio>

#include "icon_registry.h"
#include "main_window.h"
#include "fluent_theme.h"
#include "theme_bridge.h"

// ComboBox:弹出层取证要用它的 showPopup()
#include "fluent/fluent_setting_cards.h"

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

    // 主题:1:1 主题层(令牌 = Python theme.py;样式 = qf 原版 QSS)
    // 默认深色 + Fluent 默认蓝;验收脚本用 SXCL_UI_ACCENT / SXCL_UI_THEME 覆盖成参考图的条件
    sxcl::ui::FluentTheme &theme = sxcl::ui::FluentTheme::instance();
    const QString themeEnv = qEnvironmentVariable("SXCL_UI_THEME");
    theme.setDark(themeEnv.compare(QStringLiteral("light"), Qt::CaseInsensitive) != 0);
    const QString accentEnv = qEnvironmentVariable("SXCL_UI_ACCENT");
    if (!accentEnv.isEmpty())
        theme.setAccent(QColor::fromString(accentEnv));
    theme.apply(&app);
    std::fprintf(stderr, "[sxcl-ui] 主题: %s, 强调色 %s, QSS %d 个文件(%s)\n",
                 theme.isDark() ? "深色" : "浅色", theme.accent().name().toUtf8().constData(),
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

    if (!shot.isEmpty() && popup.isEmpty()) {
        QTimer::singleShot(1500, &app, [&window, shot]() {
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
            }
            const bool ok = pm.save(shot);
            std::fprintf(stderr, "[sxcl-ui] 截图 %s %dx%d %s\n", shot.toUtf8().constData(),
                         pm.width(), pm.height(), ok ? "OK" : "FAILED");
            QCoreApplication::quit();
        });
    }
    return app.exec();
}