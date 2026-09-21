#pragma once
// tray —— 任务栏托盘图标(「最小化 = 隐藏窗口」的唯一恢复入口)
//
// 为什么是**新增设计**:Python 版 src/app/main_window.py 的窗口部分没有这套语义 ——
// 它最小化就是最小化到任务栏、关闭即退出、没有托盘。用户对 C 版的要求是
//   「最小化就退出(界面)但不杀进程,后台任务继续跑」
// 界面既然退走了,就必须留一个回来的入口 —— 这个类就是那个入口。
// 所以本文件不是对 Python 版的移植,与 qf 的视觉规格无关:托盘菜单是 Windows
// 自己的菜单,颜色/字体走系统,不存在"要和参考图逐像素对齐"的对象。
//
// 职责边界:**只管托盘,不碰窗口**。三个动作各发一个信号,由 MainWindow 决定怎么做
// (显示窗口要 raise + 重新置顶,退出要先结束 worker 再落盘)。托盘自己不 new 窗口、
// 不 close 窗口,免得窗口状态有三个地方改。
#include <QObject>
#include <QString>

class QAction;
class QMenu;
class QSystemTrayIcon;

namespace sxcl::ui {

class SxclTray : public QObject {
    Q_OBJECT
public:
    // 当前会话有没有托盘(offscreen 平台、没有 shell 的会话 = false)。
    // **这条很重要**:没有托盘时 MainWindow 既不创建它,也**不打开**
    // "关掉最后一个窗口不退出进程"那条规则 —— 否则最小化后用户没有任何办法把窗口拿回来,
    // 进程会留在后台且不可见。宁可不隐藏,也不能让窗口失踪。
    static bool available();

    explicit SxclTray(QObject *parent = nullptr);
    ~SxclTray() override;

    bool isVisible() const;

    void show();
    void hide();

    // 菜单里的只读信息行:「当前任务:<进度>」。空串 = 没有在跑的任务。
    void setTaskSummary(const QString &text);
    // 菜单项在「缩成小窗口 / 恢复原尺寸」之间切(小窗口态下点它 = 恢复)
    void setMiniMode(bool mini);
    void setToolTip(const QString &text);

signals:
    void showWindowRequested();  // 双击 / 左键单击 / 菜单「显示主窗口」
    void toggleMiniRequested();  // 菜单「缩成小窗口」/「恢复原尺寸」
    void quitRequested();        // 菜单「退出」:主窗口必须先结束 worker 并落盘状态

private:
    QSystemTrayIcon *m_icon = nullptr;
    QMenu *m_menu = nullptr; // 没有父控件(托盘菜单是顶层弹层),析构里显式 delete
    QAction *m_taskAct = nullptr;
    QAction *m_showAct = nullptr;
    QAction *m_miniAct = nullptr;
    QAction *m_quitAct = nullptr;
    bool m_mini = false;
};

} // namespace sxcl::ui
