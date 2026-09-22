/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#include <QFrame>
#include <QHash>
#include <QString>
#include <QVector>

#include "libqf.h"

#include "icon_registry.h"
class QVBoxLayout;
class QPropertyAnimation;

namespace sxcl::ui {

// 一个导航项的声明(路由键 / 图标 / 标题 / 是否在底部)
// 图标两种来源,对应 Python 版 main_window.py:105-119 的写法:
//   * qfIcon 非空   -> qf 原生图标(FIF.HOME / FIF.UPDATE / FIF.LAYOUT / FIF.GLOBE / FIF.SETTING)
//   * blockKind 非空 -> 方块图(grass_block_icon(24),来自 src/app/icons.py)
struct NavItem {
    QString routeKey;
    QString qfIcon;    // qf 图标名(与 Python FluentIcon 枚举同名,如 "Home")
    QString blockKind; // 方块种类(BLOCK_FILES 的键,如 "vanilla")
    QString title;
    bool bottom = false;
    // 第三套图标来源(2026-09-22 加):IconRegistry 的语义名。
    // 为什么需要:下载页那层"双层侧边栏"要用的 PCL 图标(mod.svg / shader.svg)既不是方块 PNG、
    // 也不是 qf 内置图标 —— 它们走 IconRegistry(语义名 -> assets/icons/pcl/*.svg)。
    // -1 = 不用它(qfIcon / blockKind 优先)。
    int semantic = -1;

    // ↓ 2026-09-22 晚新增的两个字段(**必须放在最后**:别处还有 {key, qfIcon, blockKind, title,
    //   bottom, semantic} 这样的聚合初始化,插在中间会把 int 塞给 QString/QPixmap)。

    /** 非空 = 这一行**悬停时**右侧出现一个动作按钮(用 qf 图标名,如 "Setting")。
     *  用户点名:"当 2 栏被拉出,鼠标悬停的文件夹出现设置按钮"。点它不会选中这一行,
     *  而是发 NavPanel::itemAction(routeKey)。 */
    QString actionIcon;
    /** 第二行小字（文件夹列表就是它显示**路径**：用户 2026-09-22 晚点名
     *  「文件夹列表都是 .minecraft 就算了。怎么区分啊？做成上方名字下方小字路径」）。
     *  非空时这一行会变高（两行文字），字号更小、颜色取次要文字色。 */
    QString subtitle;
};

// 纯图标工具按钮(qf python NavigationToolButton):
//   * 尺寸恒为 40x36(setCompacted 被覆写,不随面板展开变化)
//   * 不可选中(isSelectable=False -> 不画选中态/指示条)
//   * 只画图标,不画文字(isCompacted 恒为 True)
class NavToolButton : public NavigationWidget {
    Q_OBJECT
public:
    // svgName:qf 图标名,如 "Menu" / "Return"
    explicit NavToolButton(const QString &svgName, QWidget *parent = nullptr);

    void setSvgName(const QString &svgName);
    QString svgName() const { return m_svgName; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString m_svgName;
};

// 导航条目:libqf 的 NavigationPushButton,绘制按 qf navigation_widget.py:176-214 逐句复刻
class NavButton : public NavigationPushButton {
    Q_OBJECT
public:
    NavButton(const QIcon &icon, const QString &qfIconName, const QString &blockKind,
              const QString &text, QWidget *parent = nullptr);

    /** 页面给的现成图标(文件夹自定义图标);空 = 回到 qfIcon / blockKind。 */
    void setPixmapIcon(const QPixmap &pm);
    /** 第二行小字(路径之类);空 = 单行。 */
    void setSubtitle(const QString &text);
    /** 右侧是否留出动作按钮(齿轮)的位置,免得文字压到它下面。 */
    void setActionReserve(bool reserve) { m_actionReserve = reserve; }

protected:
    void paintEvent(QPaintEvent *) override;
    QRect indicatorRect() const override; // 选中指示条:QRectF(0,10,3,16)

private:
    QString m_qfIconName;
    QString m_blockKind;
    QPixmap m_pixmap;
    QString m_subtitle;
    bool m_actionReserve = false;
};

// 导航面板:顶部组(返回 + 菜单 + 导航项)+ 弹性 + 底部组(设置)
class NavPanel : public QFrame {
    Q_OBJECT
public:
    // qf navigation_panel.py:96-97,108,121-122 —— 全部照抄,不许改
    static constexpr int kExpandedWidth = 322;  // expandWidth
    static constexpr int kCollapsedWidth = 48;  // resize(48, height) / setMinimumWidth(48)
    static constexpr int kExpandDurationMs = 150; // expandAni.setDuration(150)
    static constexpr int kMinimumExpandWidth = 1008; // 窗口更宽才是 EXPAND 模式(否则 MENU 覆盖层)

    explicit NavPanel(QWidget *parent = nullptr);

    NavigationPushButton *addItem(const NavItem &item);
    /** 给一条已经加进来的条目换上一张"现成的图"(版本选择页的文件夹自定义图标)。
     *  为什么不做成 NavItem 的字段:NavItem 有**文件作用域**的数组(kNavSpec / kDownloadNav),
     *  而 QPixmap 必须在 QGuiApplication 之后构造 —— 放进结构体里会让那些静态数组
     *  在 main() 之前就构造 QPixmap,直接报"Must construct a QGuiApplication before a QPixmap"。
     *  所以由页面在建好条目之后单独设(那时应用早就起来了)。 */
    void setItemIconPixmap(const QString &routeKey, const QPixmap &pm);
    void setCurrent(const QString &routeKey);
    QString currentRouteKey() const { return m_current; }
    const QVector<NavItem> &items() const { return m_items; }
    NavigationPushButton *button(const QString &routeKey) const;
    /** 这一行的悬停动作按钮（齿轮）；没有就返回 nullptr。弹窗类 UI 拿它当锚点。 */
    QWidget *actionButton(const QString &routeKey) const;

    // qf navigation_panel.py:83 menuButton 对应的汉堡按钮(与条目的间距、位置同 qf)
    NavToolButton *menuButton() const { return m_menuBtn; }
    NavToolButton *returnButton() const { return m_returnBtn; }

    bool collapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed); // 汉堡按钮切换;折叠后仅图标(库的 compacted 态)

signals:
    void routeChanged(const QString &routeKey);
    /** 某一行右侧的"动作按钮"(NavItem.actionIcon)被点了。参数是那一行的 routeKey。 */
    void itemAction(const QString &routeKey);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setWidgetsCompacted(bool compacted);
    /** 给一条导航项挂上悬停动作按钮(见 NavItem.actionIcon)。 */
    void attachActionButton(NavigationPushButton *host, const QString &routeKey,
                            const QString &svgName);
    void layoutActionButton(NavigationPushButton *host);

    QVBoxLayout *m_top = nullptr;
    QVBoxLayout *m_bottom = nullptr;
    NavToolButton *m_returnBtn = nullptr;
    NavToolButton *m_menuBtn = nullptr;
    QPropertyAnimation *m_widthAni = nullptr; // 展开/折叠:150ms OutQuad(qf navigation_panel.py:121-122)
    QVector<NavItem> m_items;
    QHash<QString, NavigationPushButton *> m_buttons;
    QHash<QString, NavToolButton *> m_actionButtons;   /**< routeKey -> 悬停动作按钮 */
    QString m_current;
    bool m_collapsed = true; // qf 默认 displayMode = COMPACT(折叠,48 宽)
};

} // namespace sxcl::ui
