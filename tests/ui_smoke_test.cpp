// tests/ui_smoke_test.cpp —— 阶段 6(UI 地基)的程序化验收
//
// 全程 -platform offscreen,不需要显示器,也不弹窗。断言分五组:
//   ① 主窗存在、标题正确、尺寸合理(初始 1100x750,最小 900x600)
//   ② 主导航 = 主页/下载/任务/联机/更多 五项 + 底部「设置」一项(逐项断言标题文本)
//   ③ 五个语义图标都加载成功,且 QSvgRenderer 渲染到 QImage 后墨迹覆盖率 > 3%
//   ④ 主题令牌浅色/深色各取一次,断言两种模式下窗口背景色不同(主题桥接真的生效)
//   ⑤ 截图 <build>/ui_shot.png,整图墨迹覆盖率 > 5%(不是白屏)
//
// 失败不 early-return:所有断言跑完再给退出码,一次性看到全部问题。
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHash>
#include <QSet>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QStackedWidget>
#include <QSvgRenderer>
#include <QStringList>
#include <QWidget>

#include <cstdio>

#include "libqf.h" // libqf(导航断言用的是真控件,不是我们的数据结构)
#include "icon_registry.h"
#include "main_window.h"
#include "nav.h"
#include "theme_bridge.h"

using namespace sxcl::ui;

namespace {

int g_checks = 0;
int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString()) {
    ++g_checks;
    if (!ok)
        ++g_fail;
    const QByteArray w = what.toUtf8();
    const QByteArray d = detail.toUtf8();
    std::printf("%s %s%s%s\n", ok ? "[ ok ]" : "[FAIL]", w.constData(),
                d.isEmpty() ? "" : "  ->  ", d.constData());
    std::fflush(stdout);
}

void section(const QString &title) {
    std::printf("\n---- %s ----\n", title.toUtf8().constData());
    std::fflush(stdout);
}

// 墨迹覆盖率:把 svg 渲染到 n x n 透明图,数 alpha > 16 的像素占比
double svgInkCoverage(const QByteArray &svg, int n) {
    QSvgRenderer r(svg);
    if (!r.isValid())
        return -1.0;
    QImage img(n, n, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        r.render(&p);
    }
    long ink = 0;
    for (int y = 0; y < n; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < n; ++x)
            if (qAlpha(line[x]) > 16)
                ++ink;
    }
    return double(ink) / double(n * n);
}

// 非透明像素的平均色(用来证明"按主题色着色"确实改了墨迹颜色)
QColor svgInkMeanColor(const QByteArray &svg, int n) {
    QSvgRenderer r(svg);
    if (!r.isValid())
        return QColor();
    QImage img(n, n, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        r.render(&p);
    }
    qint64 sr = 0, sg = 0, sb = 0;
    long cnt = 0;
    for (int y = 0; y < n; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < n; ++x) {
            if (qAlpha(line[x]) > 128) {
                sr += qRed(line[x]);
                sg += qGreen(line[x]);
                sb += qBlue(line[x]);
                ++cnt;
            }
        }
    }
    if (!cnt)
        return QColor();
    return QColor(int(sr / cnt), int(sg / cnt), int(sb / cnt));
}

int channelDistance(const QColor &a, const QColor &b) {
    return qMax(qAbs(a.red() - b.red()),
                qMax(qAbs(a.green() - b.green()), qAbs(a.blue() - b.blue())));
}

// 量化直方图找主色(每通道 >> 2 分箱:足够把 #ffffff / #f3f3f3 / #f0f4f9 分开)
struct Bucket {
    long n = 0;
    qint64 r = 0, g = 0, b = 0;
};

QColor dominantColor(const QImage &img) {
    QHash<QRgb, Bucket> hist;
    for (int y = 0; y < img.height(); y += 2) {
        for (int x = 0; x < img.width(); x += 2) {
            const QRgb c = img.pixel(x, y);
            const QRgb key = qRgb(qRed(c) >> 2, qGreen(c) >> 2, qBlue(c) >> 2);
            Bucket &bk = hist[key];
            bk.n += 1;
            bk.r += qRed(c);
            bk.g += qGreen(c);
            bk.b += qBlue(c);
        }
    }
    const Bucket *best = nullptr;
    for (auto it = hist.constBegin(); it != hist.constEnd(); ++it) {
        if (!best || it.value().n > best->n)
            best = &it.value();
    }
    if (!best || best->n == 0)
        return QColor();
    return QColor(int(best->r / best->n), int(best->g / best->n), int(best->b / best->n));
}

bool isNavSemantic(IconRegistry::Semantic s) {
    return s == IconRegistry::Home || s == IconRegistry::Download ||
           s == IconRegistry::Tasks || s == IconRegistry::Multiplayer ||
           s == IconRegistry::More;
}

struct Coverage {
    double structural = 0; // 与主色差异 > 8 的像素占比
    double ink = 0;        // 与主色差异 > 64 的像素占比(文字/图标/描边)
};

Coverage imageCoverage(const QImage &img, const QColor &ref) {
    Coverage c;
    long structural = 0, ink = 0, total = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QColor p = img.pixelColor(x, y);
            const int d = channelDistance(p, ref);
            if (d > 8)
                ++structural;
            if (d > 64)
                ++ink;
            ++total;
        }
    }
    if (total) {
        c.structural = double(structural) / double(total);
        c.ink = double(ink) / double(total);
    }
    return c;
}

} // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen"); // 直接跑 exe 也是离屏
    // offscreen 平台用的是 basic 字体库:不告诉它字体目录就一个字体都没有,
    // 窗口里的文字会整片画不出来(截图只剩图标)。这里兜底指到系统字体目录。
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        const QByteArray windir = qgetenv("WINDIR");
        const QString fonts = QString::fromLocal8Bit(windir) + QStringLiteral("/Fonts");
        if (!windir.isEmpty() && QFileInfo::exists(fonts))
            qputenv("QT_QPA_FONTDIR", fonts.toLocal8Bit());
    }
    QApplication app(argc, argv);
    std::printf("平台插件: %s   字体族: %d 个\n",
                QGuiApplication::platformName().toUtf8().constData(),
                int(QFontDatabase::families().size()));

    // =================================================================
    section(QStringLiteral("① 主窗口"));
    MainWindow window;
    window.show();
    QApplication::processEvents();
    if (window.layout())
        window.layout()->activate();
    QApplication::processEvents();

    const QString title = window.windowTitle();
    check(title == QStringLiteral("Silent X Craft Launcher"), QStringLiteral("窗口标题"),
          title);
    check(window.width() == MainWindow::kInitialWidth &&
              window.height() == MainWindow::kInitialHeight,
          QStringLiteral("初始尺寸 = 1100x750"),
          QStringLiteral("%1x%2").arg(window.width()).arg(window.height()));
    check(window.minimumWidth() == MainWindow::kMinimumWidth &&
              window.minimumHeight() == MainWindow::kMinimumHeight,
          QStringLiteral("最小尺寸 = 900x600"),
          QStringLiteral("%1x%2").arg(window.minimumWidth()).arg(window.minimumHeight()));
    check(window.isVisible(), QStringLiteral("窗口已 show()"));

    // =================================================================
    section(QStringLiteral("② 导航:5 项主导航 + 底部设置 1 项"));
    // 口径 = Python src/app/main_window.py:105-119(逐条对应,不是我们自创):
    //   Home / grass_block(版本) / Update(任务) / Layout(按键映射) / Globe(联机) / Setting(设置,底部)
    // 老的"下载/更多"是 C 版早期自创项,已按 Python 口径替换 —— 本测试就是这条口径的守卫。
    const QStringList wantKeys{QStringLiteral("home"), QStringLiteral("versions"),
                               QStringLiteral("tasks"), QStringLiteral("keymap"),
                               QStringLiteral("multiplayer"), QStringLiteral("settings")};
    const QStringList wantTitles{QStringLiteral("主页"), QStringLiteral("版本"),
                                 QStringLiteral("任务"), QStringLiteral("按键映射"),
                                 QStringLiteral("联机"), QStringLiteral("设置")};
    const QVector<NavItem> &items = window.navItems();
    check(items.size() == 6, QStringLiteral("导航项数 = 6"),
          QStringLiteral("实际 %1").arg(items.size()));
    for (int i = 0; i < items.size() && i < wantKeys.size(); ++i) {
        check(items[i].routeKey == wantKeys[i] && items[i].title == wantTitles[i],
              QStringLiteral("导航[%1] 路由/标题").arg(i),
              QStringLiteral("%1 / %2").arg(items[i].routeKey, items[i].title));
    }
    int topCount = 0, bottomCount = 0;
    for (const NavItem &it : items)
        (it.bottom ? bottomCount : topCount) += 1;
    check(topCount == 5 && bottomCount == 1,
          QStringLiteral("顶部 5 项 / 底部 1 项(设置)"),
          QStringLiteral("top=%1 bottom=%2").arg(topCount).arg(bottomCount));

    // 真控件断言:libqf 的 NavigationPushButton 必须真的有 6 个,文字对得上
    const QList<NavigationPushButton *> buttons =
        window.findChildren<NavigationPushButton *>();
    check(buttons.size() == 6, QStringLiteral("窗口内 libqf 导航按钮数 = 6"),
          QStringLiteral("实际 %1").arg(buttons.size()));
    for (int i = 0; i < wantKeys.size(); ++i) {
        NavigationPushButton *b = window.navPanel()->button(wantKeys[i]);
        check(b != nullptr && b->text() == wantTitles[i],
              QStringLiteral("导航按钮「%1」存在且文字正确").arg(wantTitles[i]),
              b ? b->text() : QStringLiteral("(null)"));
    }
    // 底部项在布局里必须真的更靠下
    NavigationPushButton *bottomBtn = window.navPanel()->button(QStringLiteral("settings"));
    int maxTopY = -1;
    for (int i = 0; i < 5; ++i) {
        NavigationPushButton *b = window.navPanel()->button(wantKeys[i]);
        if (b)
            maxTopY = qMax(maxTopY, b->mapTo(&window, QPoint(0, 0)).y());
    }
    if (bottomBtn)
        check(bottomBtn->mapTo(&window, QPoint(0, 0)).y() > maxTopY,
              QStringLiteral("「设置」在其余五项下方"),
              QStringLiteral("settings.y=%1 > topMax.y=%2")
                  .arg(bottomBtn->mapTo(&window, QPoint(0, 0)).y())
                  .arg(maxTopY));
    check(window.currentRouteKey() == QStringLiteral("home"),
          QStringLiteral("默认选中主页"), window.currentRouteKey());
    check(window.pageStack()->count() == 6, QStringLiteral("页面栈 6 页"),
          QStringLiteral("实际 %1").arg(window.pageStack()->count()));

    // =================================================================
    section(QStringLiteral("③ 语义图标加载 + 墨迹覆盖率(128x128,alpha>16)"));
    IconRegistry &icons = IconRegistry::instance();
    icons.load();
    std::printf("     图标目录: %s\n", icons.iconDir().toUtf8().constData());
    for (int i = 0; i < icons.count(); ++i) {
        const IconRegistry::Semantic s = IconRegistry::Semantic(i);
        const IconRegistry::Entry &e = icons.entry(s);
        const bool loaded = icons.has(s) && !icons.svg(s).isEmpty();
        const double cov = loaded ? svgInkCoverage(icons.svg(s), 128) : -1.0;
        const bool isNav = isNavSemantic(s);
        std::printf("     %-4s %-4s %-22s %-14s cov=%.2f%%%s\n",
                    isNav ? "导航" : "其余", e.name.toUtf8().constData(),
                    e.file.toUtf8().constData(),
                    e.pclName.toUtf8().constData(), cov < 0 ? 0.0 : cov * 100.0,
                    e.inIndex ? "" : "  <不在 index.tsv>");
        check(loaded, QStringLiteral("图标「%1」加载成功").arg(e.name), e.file);
        check(e.inIndex, QStringLiteral("图标「%1」在 index.tsv 里有出处").arg(e.name),
              e.source);
        check(cov > 0.03, QStringLiteral("图标「%1」墨迹覆盖率 > 3%").arg(e.name),
              QStringLiteral("%1%").arg(cov * 100.0, 0, 'f', 2));
        if (isNav)
            check(!icons.themedIcon(s).isNull(),
                  QStringLiteral("导航图标「%1」能生成 QIcon").arg(e.name));
    }

    // 着色:同一份 svg,浅色/深色主题下墨迹平均色必须不同(证明"按主题着色")
    check(!icons.themedIcon(IconRegistry::Home).isNull(),
          QStringLiteral("主页图标 QIcon 非空"));

    // =================================================================
    section(QStringLiteral("④ 主题令牌(浅/深各取一次)"));
    ThemeBridge &tb = ThemeBridge::instance();
    tb.setMode(fluent::Theme::Light);
    QApplication::processEvents();
    const QColor lightBg = tb.token(QStringLiteral("bg"));
    const QColor lightAccent = tb.accent();
    const QImage lightImg = window.grab().toImage();
    const QColor lightWinPx = lightImg.pixelColor(lightImg.width() / 2, 20);
    const QColor lightInk = svgInkMeanColor(icons.tinted(IconRegistry::Home, tb.iconColor()), 64);

    tb.setMode(fluent::Theme::Dark);
    QApplication::processEvents();
    const QColor darkBg = tb.token(QStringLiteral("bg"));
    const QColor darkAccent = tb.accent();
    const QImage darkImg = window.grab().toImage();
    const QColor darkWinPx = darkImg.pixelColor(darkImg.width() / 2, 20);
    const QColor darkInk = svgInkMeanColor(icons.tinted(IconRegistry::Home, tb.iconColor()), 64);

    std::printf("     浅色: bg=%s accent=%s 窗口背景像素=(%d,%d,%d) 图标墨迹=(%d,%d,%d)\n",
                lightBg.name().toUtf8().constData(), lightAccent.name().toUtf8().constData(),
                lightWinPx.red(), lightWinPx.green(), lightWinPx.blue(), lightInk.red(),
                lightInk.green(), lightInk.blue());
    std::printf("     深色: bg=%s accent=%s 窗口背景像素=(%d,%d,%d) 图标墨迹=(%d,%d,%d)\n",
                darkBg.name().toUtf8().constData(), darkAccent.name().toUtf8().constData(),
                darkWinPx.red(), darkWinPx.green(), darkWinPx.blue(), darkInk.red(),
                darkInk.green(), darkInk.blue());
    check(lightBg != darkBg, QStringLiteral("令牌 bg 浅/深不同"),
          QStringLiteral("%1 vs %2").arg(lightBg.name(), darkBg.name()));
    check(channelDistance(lightWinPx, darkWinPx) > 100,
          QStringLiteral("窗口背景像素浅/深不同(主题桥接生效)"),
          QStringLiteral("距离 %1").arg(channelDistance(lightWinPx, darkWinPx)));
    check(channelDistance(lightWinPx, lightBg) <= 2 &&
              channelDistance(darkWinPx, darkBg) <= 2,
          QStringLiteral("窗口背景像素 == 令牌 bg(两模式都对得上)"));
    check(lightWinPx.value() > 180 && darkWinPx.value() < 90,
          QStringLiteral("浅色更亮 / 深色更暗(方向正确)"),
          QStringLiteral("V=%1 / %2").arg(lightWinPx.value()).arg(darkWinPx.value()));
    check(channelDistance(lightInk, darkInk) > 100,
          QStringLiteral("同一图标在浅/深主题下墨迹色不同(按主题着色)"),
          QStringLiteral("距离 %1").arg(channelDistance(lightInk, darkInk)));
    check(channelDistance(svgInkMeanColor(icons.tinted(IconRegistry::Home, lightAccent), 64),
                          lightAccent) < 24,
          QStringLiteral("按 libqf 主题色着色:墨迹平均色 ≈ 主题色"),
          QStringLiteral("accent=%1").arg(lightAccent.name()));
    // 强调色语义:FluentTheme::accent() 返回 **推导后**的值(Python theme.py:135/168 的 themeColor()),
    // 它随深浅主题变化 —— 所以不能拿"浅色时抓到的那个值"去和"此刻(可能已切深色)"的值比。
    // 这里断言的是真正要守的东西:**accent 来自 libqf 单例,不是本层自己定的**。
    check(tb.accent() == fluent::FluentStyle::instance()->themeColor() && tb.accent().isValid(),
          QStringLiteral("主题色来自 libqf 单例(推导值,非本层定义)"),
          QStringLiteral("accent=%1 themeColor=%2").arg(tb.accent().name(),
                      fluent::FluentStyle::instance()->themeColor().name()));

    // 回到浅色出图
    tb.setMode(fluent::Theme::Light);
    QApplication::processEvents();

    // =================================================================
    section(QStringLiteral("⑤ 截图与整图覆盖率"));
    const QString shotPath = QDir::current().absoluteFilePath(QStringLiteral("ui_shot.png"));
    const QPixmap shot = window.grab();
    const bool saved = shot.save(shotPath, "PNG");
    check(saved, QStringLiteral("截图已写出"), shotPath);
    const QImage img = shot.toImage();
    check(img.width() == MainWindow::kInitialWidth &&
              img.height() == MainWindow::kInitialHeight,
          QStringLiteral("截图尺寸 = 1100x750"),
          QStringLiteral("%1x%2").arg(img.width()).arg(img.height()));
    const QColor dom = dominantColor(img);
    const Coverage cov = imageCoverage(img, dom);
    std::printf("     主色=#%02x%02x%02x  结构覆盖率(>8)=%.2f%%  墨迹覆盖率(>64)=%.2f%%\n",
                dom.red(), dom.green(), dom.blue(), cov.structural * 100.0, cov.ink * 100.0);
    check(cov.structural > 0.05, QStringLiteral("整图墨迹覆盖率 > 5%(不是白屏)"),
          QStringLiteral("%1%").arg(cov.structural * 100.0, 0, 'f', 2));
    check(cov.ink > 0.0025, QStringLiteral("文字/图标墨迹覆盖率(>64) > 0.25%"),
          QStringLiteral("%1%").arg(cov.ink * 100.0, 0, 'f', 2));
    // 导航面板区域必须真画了东西(图标就画在这里;与字体是否存在无关)
    const QWidget *nav = window.navPanel();
    const QRect navRect(nav->mapTo(&window, QPoint(0, 0)), nav->size());
    const QImage navImg = img.copy(navRect);
    const Coverage navCov = imageCoverage(navImg, dominantColor(navImg));
    std::printf("     导航面板 %dx%d 结构覆盖率=%.2f%% 墨迹=%.2f%%\n", navRect.width(),
                navRect.height(), navCov.structural * 100.0, navCov.ink * 100.0);
    check(navCov.ink > 0.003, QStringLiteral("导航面板里有墨迹(图标/文字画出来了)"),
          QStringLiteral("%1%").arg(navCov.ink * 100.0, 0, 'f', 2));
    QSet<QRgb> colors;
    for (int y = 0; y < img.height(); y += 3)
        for (int x = 0; x < img.width(); x += 3)
            colors.insert(img.pixel(x, y));
    check(colors.size() > 16, QStringLiteral("整图颜色数 > 16(不是纯色块)"),
          QStringLiteral("%1 色").arg(colors.size()));
    check(QFileInfo::exists(shotPath) && QFileInfo(shotPath).size() > 2000,
          QStringLiteral("截图文件非空"),
          QStringLiteral("%1 字节").arg(QFileInfo(shotPath).size()));

    // =================================================================
    std::printf("\n==== 断言 %d 条,失败 %d 条 ====\n", g_checks, g_fail);
    std::fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}
