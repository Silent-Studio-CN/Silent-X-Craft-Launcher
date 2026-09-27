/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "icon_registry.h"

#include "theme_bridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QStringList>
#include <QSvgRenderer>

#include <functional>
#include <memory>
#include <utility>

namespace sxcl::ui {

namespace {

// 语义名 -> svg 文件。每一行的选取依据见 index.tsv(原名称/来源),报告里逐条列出。
struct MapRow {
    IconRegistry::Semantic semantic;
    const char *name;
    const char *key;
    const char *file;
    const char *why;
};

const MapRow kMap[] = {
    {IconRegistry::Home, "主页", "home", "icon_09f00658.svg",
     "index.tsv: 原名称「概览」(PageInstanceLeft.xaml)—— PCL 的首页/概览位"},
    {IconRegistry::Download, "下载", "download", "pathdownload.svg",
     "index.tsv: 原名称「PathDownload」(MyResourceItem.xaml)—— 下载箭头,语义直给"},
    {IconRegistry::Tasks, "任务", "tasks", "pathtime.svg",
     "index.tsv: 原名称「PathTime」(MyResourceItem.xaml)—— 时钟,任务/进度位"},
    {IconRegistry::Multiplayer, "联机", "multiplayer", "icon_b4bd7715.svg",
     "index.tsv: 原名称「联机」(PageSetupLeft.xaml)—— 语义直给"},
    {IconRegistry::More, "更多", "more", "icon_a209b3cb.svg",
     "index.tsv: 原名称「其他」(PageSetupLeft.xaml)—— 四宫格,即「更多」位"},
    {IconRegistry::Settings, "设置", "settings", "settings.svg",
     "index.tsv: 原名称「设置」(PageInstanceLeft.xaml)—— 语义直给"},
    {IconRegistry::Help, "帮助", "help", "help.svg",
     "index.tsv: 原名称「帮助」(PageOtherLeft.xaml)—— 语义直给"},
    {IconRegistry::Personalize, "个性化", "personalize", "icon_ab44a2ce.svg",
     "index.tsv: 原名称「个性化」(PageSetupLeft.xaml)—— 语义直给"},
    {IconRegistry::Launch, "启动", "launch", "launch.svg",
     "index.tsv: 原名称「启动」(PageSetupLeft.xaml)—— 语义直给"},
    {IconRegistry::Refresh, "刷新", "refresh", "refresh.svg",
     "index.tsv: 原名称「刷新」(PageDownloadLeft.xaml)—— 语义直给"},
    {IconRegistry::Search, "搜索", "search", "icon_01389e71.svg",
     "index.tsv: 来源 MySearchBox.xaml(搜索框里的放大镜),原表未给名"},
    /* Mod / Shader 这两枚**不用 PCL 那两张**(拼图块 / 光影包):用户 2026-09-27 点名
     * 「Mod 和光影的图标赶紧找东西替代上,别就麦块版本一个草方块」。
     * file 前面的 "ui/" 是相对 assets/icons 的子目录(见 assetPath);出处写在本目录的
     * index.tsv 与 NOTICE.md 里 —— 索引那一条断言(测试里查 inIndex)照旧成立。 */
    {IconRegistry::Mod, "模组", "mod", "ui/mod.svg",
     "assets/icons/ui/index.tsv —— 自绘「扩展模块」芯片,出处见 assets/icons/ui/NOTICE.md"},
    {IconRegistry::Shader, "光影", "shader", "ui/shader.svg",
     "assets/icons/ui/index.tsv —— 自绘「对比圆」,出处见 assets/icons/ui/NOTICE.md"},
};

const MapRow &row(IconRegistry::Semantic s) { return kMap[int(s)]; }

// 主题实时着色图标引擎:每次绘制按当前主题现取颜色,再替换 svg 里的墨迹色。
class ThemedSvgIconEngine : public QIconEngine {
public:
    ThemedSvgIconEngine(QByteArray raw, std::function<QColor()> colorFn)
        : m_raw(std::move(raw)), m_color(std::move(colorFn)) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
               QIcon::State state) override {
        Q_UNUSED(state)
        QSvgRenderer *r = renderer();
        if (!r)
            return;
        const qreal opacity = mode == QIcon::Disabled
                                  ? 0.4
                                  : (mode == QIcon::Selected ? 0.75 : 1.0);
        painter->save();
        painter->setOpacity(painter->opacity() * opacity);
        r->render(painter, QRectF(rect));
        painter->restore();
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap pm(size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paint(&p, QRect(QPoint(0, 0), size), mode, state);
        return pm;
    }

    QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        QPixmap pm = pixmap(size * scale, mode, state);
        pm.setDevicePixelRatio(scale);
        return pm;
    }

    QIconEngine *clone() const override {
        return new ThemedSvgIconEngine(m_raw, m_color);
    }

private:
    QSvgRenderer *renderer() {
        const QColor c = m_color();
        const QByteArray key = c.name(QColor::HexRgb).toLatin1();
        if (m_renderer && key == m_rendererKey)
            return m_renderer.get();
        QByteArray data = m_raw;
        data.replace("currentColor", key);
        data.replace("#000000", key);
        data.replace("#ffffff", key);
        data.replace("#FFFFFF", key);
        std::unique_ptr<QSvgRenderer> r(new QSvgRenderer(data));
        if (!r->isValid())
            return nullptr;
        m_rendererKey = key;
        m_renderer = std::move(r);
        return m_renderer.get();
    }

    QByteArray m_raw;
    std::function<QColor()> m_color;
    QByteArray m_rendererKey;
    std::unique_ptr<QSvgRenderer> m_renderer;
};

// 从 base 起逐级向上找 assets/icons/pcl(开发期:build/Release → 工程根)
void appendUpward(QStringList &out, const QString &start) {
    QDir d(start);
    for (int i = 0; i < 6; ++i) {
        out << d.absolutePath() + QStringLiteral("/assets/icons/pcl");
        if (!d.cdUp())
            break;
    }
}

QStringList candidateDirs() {
    QStringList out;
    // 1) 显式覆盖:环境变量
    const QByteArray env = qgetenv("SXCL_ICON_DIR");
    if (!env.isEmpty())
        out << QString::fromLocal8Bit(env);
    // 2) 可执行文件目录往上(假定构建目录在工程里)
    appendUpward(out, QCoreApplication::applicationDirPath());
    // 3) 当前工作目录往上(从别处调用时也能找到)
    appendUpward(out, QDir::currentPath());
    return out;
}

} // namespace

IconRegistry &IconRegistry::instance() {
    static IconRegistry inst;
    // 首次取用时自动加载:窗口可能先于显式 load() 被构造(测试里就是先建窗再看图标),
    // 没有这一步会拿空表越界。
    if (!inst.m_loaded)
        inst.load();
    return inst;
}

QString IconRegistry::key(Semantic s) { return QString::fromUtf8(row(s).key); }
QString IconRegistry::title(Semantic s) { return QString::fromUtf8(row(s).name); }

void IconRegistry::setIconDir(const QString &dir) { m_dir = dir; }
QString IconRegistry::iconDir() const { return m_dir; }
QString IconRegistry::indexFile() const {
    return m_dir.isEmpty() ? QString() : m_dir + QStringLiteral("/index.tsv");
}

bool IconRegistry::resolveIconDir() {
    if (!m_dir.isEmpty() && QFileInfo::exists(indexFile()))
        return true;
    for (const QString &d : candidateDirs()) {
        if (QFileInfo::exists(d + QStringLiteral("/index.tsv"))) {
            m_dir = d;
            return true;
        }
    }
    return false;
}

/* 索引文件(与 assets/icons/pcl/index.tsv 同格式):文件	名称	来源	风格[	尺寸]。
 * 两份都要读:
 *   * assets/icons/pcl/index.tsv —— PCL 那套素材的原文出处;
 *   * assets/icons/ui/index.tsv  —— **我们自己画**的那两枚(mod / shader)的出处。
 * 为什么自绘的也要有一份索引:界面侧那条"每个语义图标都要在索引里有出处"的断言是按
 * 索引文件查的(tests/ui_smoke_test.cpp 的 inIndex 那一条)—— 换图标不能靠绕开检查。 */
void IconRegistry::parseIndexFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const QStringList cols = line.split(QLatin1Char('\t'));
        if (cols.size() < 4)
            continue;
        for (Entry &e : m_entries) {
            if (e.file == cols[0]) {
                e.pclName = cols[1];
                e.source = cols[2];
                e.style = cols[3];
                e.inIndex = true;
            }
        }
    }
}

void IconRegistry::parseIndex() {
    parseIndexFile(indexFile());
    parseIndexFile(assetPath(QStringLiteral("ui/index.tsv")));
}

/* 语义图标文件的实际路径。file 带 "ui/" 前缀 = 隔壁那一套(assets/icons/ui/*,自绘),
 * 其余 = PCL 那套(m_dir = assets/icons/pcl)。 */
QString IconRegistry::assetPath(const QString &file) const {
    if (m_dir.isEmpty())
        return QString();
    if (file.startsWith(QLatin1String("ui/"))) {
        QDir icons(m_dir); // .../assets/icons/pcl
        if (icons.cdUp())  // .../assets/icons
            return icons.filePath(file);
        return QString();
    }
    return m_dir + QLatin1Char('/') + file;
}

bool IconRegistry::load() {
    m_entries.clear();
    m_svg.clear();
    m_loaded = false;
    resolveIconDir();
    for (const MapRow &r : kMap) {
        Entry e;
        e.name = QString::fromUtf8(r.name);
        e.key = QString::fromUtf8(r.key);
        e.file = QString::fromUtf8(r.file);
        m_entries.push_back(e);
    }
    parseIndex();
    for (int i = 0; i < m_entries.size(); ++i) {
        QFile f(assetPath(m_entries[i].file));
        QByteArray data;
        if (f.open(QIODevice::ReadOnly))
            data = f.readAll();
        m_entries[i].found = !data.isEmpty();
        m_svg.push_back(data);
    }
    m_loaded = true;
    return true;
}

const IconRegistry::Entry &IconRegistry::entry(Semantic s) const {
    static const Entry kEmpty;
    const int i = int(s);
    return (i >= 0 && i < m_entries.size()) ? m_entries[i] : kEmpty;
}

const QByteArray &IconRegistry::svg(Semantic s) const {
    static const QByteArray kEmpty;
    const int i = int(s);
    return (i >= 0 && i < m_svg.size()) ? m_svg[i] : kEmpty;
}

bool IconRegistry::has(Semantic s) const { return entry(s).found; }

QByteArray IconRegistry::tinted(Semantic s, const QColor &c) const {
    QByteArray data = m_svg[int(s)];
    if (!c.isValid())
        return data;
    const QByteArray hex = c.name(QColor::HexRgb).toLatin1();
    data.replace("currentColor", hex);
    data.replace("#000000", hex);
    data.replace("#ffffff", hex);
    data.replace("#FFFFFF", hex);
    return data;
}

QColor IconRegistry::themeIconColor() const { return ThemeBridge::instance().iconColor(); }
QColor IconRegistry::accentColor() const { return ThemeBridge::instance().accent(); }

QIcon IconRegistry::themedIcon(Semantic s) const {
    return QIcon(new ThemedSvgIconEngine(m_svg[int(s)],
                                         [] { return ThemeBridge::instance().iconColor(); }));
}

QIcon IconRegistry::accentIcon(Semantic s) const {
    return QIcon(new ThemedSvgIconEngine(m_svg[int(s)],
                                         [] { return ThemeBridge::instance().accent(); }));
}

} // namespace sxcl::ui
