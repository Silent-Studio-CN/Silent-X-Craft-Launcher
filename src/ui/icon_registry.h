/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include <QByteArray>
#include <QColor>
#include <QIcon>
#include <QString>
#include <QVector>

namespace sxcl::ui {

class IconRegistry {
public:
    // 界面层只用语义名,不直接写 svg 文件名
    enum Semantic {
        Home = 0, // 主页
        Download, // 下载
        Tasks,    // 任务
        Multiplayer, // 联机
        More,        // 更多
        Settings,    // 设置(导航底部)
        Help,        // 帮助
        Personalize, // 个性化
        Launch,      // 启动
        Refresh,     // 刷新
        Search,      // 搜索
        Mod,         // 模组(下载页左栏第二格,见 docs/25 §4)
        Shader,      // 光影(同栏第三格)
        Count
    };

    struct Entry {
        QString name;    // 语义名(中文),如 "主页"
        QString key;     // 语义键(英文),如 "home"
        QString file;    // assets/icons/pcl/<file>
        QString pclName; // index.tsv 第 2 列:原名称(可能是 "(无名)" 或中文)
        QString source;  // index.tsv 第 3 列:来源 XAML
        QString style;   // index.tsv 第 4 列:fill / stroke
        bool found = false;    // 文件是否读到
        bool inIndex = false;  // index.tsv 里是否有这一行
    };

    static IconRegistry &instance();

    // 图标目录:默认取编译期写死的源码目录(CMake 注入 SXCL_UI_ICON_DIR),
    // 也认环境变量 SXCL_ICON_DIR / 可执行文件旁的 assets/icons/pcl。
    void setIconDir(const QString &dir);
    QString iconDir() const;
    QString indexFile() const;
    bool resolveIconDir(); // 按上述顺序找一个存在的目录

    bool load(); // 读 index.tsv + 全部 svg 字节
    bool loaded() const { return m_loaded; }
    int count() const { return int(Count); }

    const Entry &entry(Semantic s) const;
    const QByteArray &svg(Semantic s) const; // 原始字节(currentColor 未替换)
    bool has(Semantic s) const;

    // 目标色 -> 着色后的 svg 文本(currentColor / #000000 / #ffffff 一并替换)
    QByteArray tinted(Semantic s, const QColor &c) const;
    // 当前主题下的图标色(取自 libqf 令牌)
    QColor themeIconColor() const;
    // 当前主题色(取自 libqf FluentStyle::themeColor)
    QColor accentColor() const;

    // QIcon 工厂:颜色在**每次绘制时**从 libqf 主题单例现取,所以主题一换图标就换色
    QIcon themedIcon(Semantic s) const;
    QIcon accentIcon(Semantic s) const;

    static QString key(Semantic s);
    static QString title(Semantic s);

private:
    IconRegistry() = default;
    void parseIndex();

    QString m_dir;
    QVector<Entry> m_entries;
    QVector<QByteArray> m_svg;
    bool m_loaded = false;
};

} // namespace sxcl::ui
