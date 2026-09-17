// sxcl-ui — Silent X Craft Launcher 图形前端入口(阶段 6 地基)
//
// 这里只做三件事:起 Qt 应用、把图标/主题准备好、开主窗口。
// 页面内容由后续批次补;命令行前端(sxcl-dl)与核心库保持独立,不受这里影响。
#include <QApplication>

#include <cstdio>

#include "icon_registry.h"
#include "main_window.h"
#include "theme_bridge.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
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

    // 主题:默认跟随 libqf 单例当前的主题(不在这里定义 themeMode/themeColor)
    sxcl::ui::ThemeBridge::instance().refreshAll();

    sxcl::ui::MainWindow window;
    window.show();
    return app.exec();
}
