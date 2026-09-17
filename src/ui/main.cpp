// sxcl-ui — Silent X Craft Launcher 图形前端入口(阶段 6 地基)
//
// 这里只做三件事:起 Qt 应用、把图标/主题准备好、开主窗口。
// 页面内容由后续批次补;命令行前端(sxcl-dl)与核心库保持独立,不受这里影响。
#include <QApplication>
#include <QColor>
#include <QPainter>
#include <QPixmap>
#include <QTimer>

#include <cstdio>

#include "icon_registry.h"
#include "main_window.h"
#include "fluent_theme.h"
#include "theme_bridge.h"

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
    if (!shot.isEmpty()) {
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