/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#pragma once

#if defined(_MSC_VER)
#pragma warning(push, 0) // libqf 是外部依赖,头文件在 /W4 下不干净(见 libqf.h 的说明)
#endif
#include "fluent/fluent_dialog.h" // MaskDialogBase / MessageBoxBase
#include "fluent/fluent_labels.h" // TitleLabel / BodyLabel / CaptionLabel / SubtitleLabel
#include "fluent/fluent_controls.h" // PushButton / PrimaryPushButton
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <QString>

class QTimer;

namespace sxcl::ui {

class AccountTask;

class AuthLoginDialog : public MessageBoxBase {
    Q_OBJECT
public:
    explicit AuthLoginDialog(QWidget *parent = nullptr);
    ~AuthLoginDialog() override;

    // 打开对话框(show();不用 exec() —— 主线程不允许被对话框的回环占住)
    static AuthLoginDialog *open(QWidget *parent);

signals:
    // 登录成功(账户状态变了)→ 设置页据此刷新「账户」卡
    void accountChanged();

protected:
    // 关闭(accept/reject/窗口关闭都走这里):先取消并**脱开**后台任务,
    // 再交给基类做淡出动画 —— 这样关窗永远不会被在飞的网络请求拖住。
    void done(int code) override;

private:
    void buildUi();
    void startLogin();
    void retry();

    void onUserCode(const QString &code, const QString &verificationUri);
    void onDeviceCodeInfo(int intervalSeconds, int expiresInSeconds);
    void onStatus(const QString &text);
    void onFinished(bool ok, const QString &message, const QString &rawError, int code);

    void onYesClicked();   // 复制代码 / 重试
    void onCancelClicked(); // 取消 / 关闭
    void copyCode();
    void openLink();
    void tickCountdown();
    void showFailure(const QString &message, const QString &rawError, int code);
    void setStatusText(const QString &text, bool danger = false);

    AccountTask *m_task = nullptr; // 每个对话框一个任务(重试时换新的);结束自删(destroyed 后置空)
    QTimer *m_countdown = nullptr;
    int m_secondsLeft = 0;
    QString m_code;
    QString m_verificationUri;
    bool m_succeeded = false;
    bool m_failed = false;

    // 控件(qf 件;颜色/字号见文件头与 docs/05)
    SubtitleLabel *m_heading = nullptr;
    BodyLabel *m_instructions = nullptr;
    BodyLabel *m_linkLabel = nullptr;
    PushButton *m_openButton = nullptr;
    TitleLabel *m_codeLabel = nullptr;
    PushButton *m_copyButton = nullptr;
    CaptionLabel *m_countdownLabel = nullptr;
    CaptionLabel *m_statusLabel = nullptr;
    BodyLabel *m_errorLabel = nullptr;
};

} // namespace sxcl::ui
