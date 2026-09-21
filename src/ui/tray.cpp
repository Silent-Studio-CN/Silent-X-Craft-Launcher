#include "tray.h"

#include <QAction>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSystemTrayIcon>

#include "icon_registry.h"
#include "theme_bridge.h"

namespace sxcl::ui {
namespace {

// 托盘图标:优先用图标注册表里已经加载的真图(PCL 抽出的矢量图,按主题色着色);
// 图源一个都没读到(资源目录不在)时画一个圆角方块 + 白字 S —— 绝不返回空图标。
// 空图标在 Windows 上表现为托盘里"看不见但点得到"的一块,用户根本找不到恢复入口。
QIcon makeTrayIcon() {
    IconRegistry &reg = IconRegistry::instance();
    if (!reg.loaded())
        reg.load();
    // 先试"启动"(最贴近这个产品的语义),没有再退到"主页"
    const IconRegistry::Semantic candidates[] = {IconRegistry::Launch, IconRegistry::Home};
    for (IconRegistry::Semantic s : candidates) {
        if (!reg.has(s))
            continue;
        const QIcon icon = reg.accentIcon(s);
        if (!icon.isNull())
            return icon;
    }
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QColor accent = ThemeBridge::instance().accent();
    if (!accent.isValid())
        accent = QColor(0, 103, 192);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawRoundedRect(QRectF(1, 1, 30, 30), 7, 7);
    p.setPen(QColor(255, 255, 255));
    QFont f = p.font();
    f.setPixelSize(20);
    f.setBold(true);
    p.setFont(f);
    p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("S"));
    return QIcon(pm);
}

} // namespace

bool SxclTray::available() { return QSystemTrayIcon::isSystemTrayAvailable(); }

SxclTray::SxclTray(QObject *parent) : QObject(parent) {
    m_icon = new QSystemTrayIcon(this);
    m_icon->setIcon(makeTrayIcon());
    m_icon->setToolTip(QStringLiteral("Silent X Craft Launcher"));

    // 菜单自带父控件会被 Qt 挂到某个窗口下面(托盘菜单不该属于任何窗口),
    // 所以父为空,由本类析构负责 delete。
    m_menu = new QMenu();

    // 第一行是**只读信息行**:用户把界面收走之后,托盘菜单本身就是"任务还在不在跑"的
    // 唯一可见处(小窗口里那一行只在窗口显示时能看到)。
    m_taskAct = m_menu->addAction(QStringLiteral("当前任务:无"));
    m_taskAct->setEnabled(false);

    m_menu->addSeparator();
    m_showAct = m_menu->addAction(QStringLiteral("显示主窗口"));
    m_miniAct = m_menu->addAction(QStringLiteral("缩成小窗口"));
    m_menu->addSeparator();
    m_quitAct = m_menu->addAction(QStringLiteral("退出"));

    // 「退出」必须走主窗口:关进程前要结束 worker 并把任务状态落盘。
    // 这里只发信号,不自己 quit()。
    connect(m_showAct, &QAction::triggered, this, &SxclTray::showWindowRequested);
    connect(m_miniAct, &QAction::triggered, this, &SxclTray::toggleMiniRequested);
    connect(m_quitAct, &QAction::triggered, this, &SxclTray::quitRequested);

    m_icon->setContextMenu(m_menu);
    connect(m_icon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                // 双击是用户明说的恢复入口;左键单击同义(Windows 上这类常驻程序的习惯)。
                // 右键(Context)交给 Qt 弹上面那个菜单,不在这里抢。
                if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
                    emit showWindowRequested();
            });
}

SxclTray::~SxclTray() {
    // 托盘图标必须在 QSystemTrayIcon 析构前藏掉,否则任务栏上会留下一个"幽灵图标",
    // 要等用户把鼠标划过去才消失。
    if (m_icon != nullptr) {
        m_icon->hide();
        m_icon->setContextMenu(nullptr);
    }
    delete m_menu;
    m_menu = nullptr;
}

bool SxclTray::isVisible() const { return m_icon != nullptr && m_icon->isVisible(); }

void SxclTray::show() {
    if (m_icon != nullptr)
        m_icon->show();
}

void SxclTray::hide() {
    if (m_icon != nullptr)
        m_icon->hide();
}

void SxclTray::setTaskSummary(const QString &text) {
    if (m_taskAct == nullptr)
        return;
    const QString line =
        text.isEmpty() ? QStringLiteral("当前任务:无") : QStringLiteral("当前任务:%1").arg(text);
    m_taskAct->setText(line);
    if (m_icon != nullptr)
        m_icon->setToolTip(QStringLiteral("Silent X Craft Launcher\n%1").arg(line));
}

void SxclTray::setMiniMode(bool mini) {
    m_mini = mini;
    if (m_miniAct != nullptr)
        m_miniAct->setText(mini ? QStringLiteral("恢复原尺寸") : QStringLiteral("缩成小窗口"));
}

void SxclTray::setToolTip(const QString &text) {
    if (m_icon != nullptr)
        m_icon->setToolTip(text);
}

} // namespace sxcl::ui
