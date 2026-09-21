/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

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
