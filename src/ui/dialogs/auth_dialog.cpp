// auth_dialog.cpp —— 设备码登录对话框的实现(声明/纪律见 auth_dialog.h)。
//
// Python 版**没有**这个对话框(它只有离线启动):这是新增界面,设计上延续 qf 语言 ——
//   * 壳 = libqf 的 MessageBoxBase(python MessageBoxBase:居中卡片 + 遮罩 + 底部按钮组),
//     与 fluent_dialog.cpp:158-191 的度量一致(按钮组定高 81、布局边距 24/12);
//   * 文字层级 = docs/05-UI-1to1规格.md §3(SubtitleLabel 20/600、BodyLabel 14/400、
//     CaptionLabel 12/400、TitleLabel 28/600 —— 8 位码用 TitleLabel,一眼能读);
//   * 颜色 = §2 的令牌(成功/失败用 success/danger,**没有任何硬编码色值**);
//   * 按钮 = §4(PushButton/PrimaryPushButton 高 32、字号 14)。
#include "auth_dialog.h"

#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include "account.h"
#include "fluent_theme.h"
#include "sxcl/auth.h" // 返回码(SXCL_AUTH_ERR_CANCELLED …)

namespace sxcl::ui {
namespace {

// qf 的 PushButton 构造里有一句 setFont(self)(button.py:36 → 14px);libqf 只套了 QSS,
// 字号得按 docs/05 §4「PushButton / PrimaryPushButton 高 32、字体 14」钉回来。
// (与 launch_page.cpp 的同名小工具逐字一致 —— 各页各留一份,免得为一个函数动公共头。)
void applyButtonFont(QPushButton *button) {
    QFont font = button->font();
    font.setPixelSize(14);
    font.setWeight(QFont::Normal);
    button->setFont(font);
    button->setFixedHeight(32);
}

QString countdownText(int seconds) {
    const int safe = seconds > 0 ? seconds : 0;
    return QStringLiteral("剩余时间 %1:%2")
        .arg(safe / 60, 2, 10, QLatin1Char('0'))
        .arg(safe % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

AuthLoginDialog::AuthLoginDialog(QWidget *parent) : MessageBoxBase(parent) {
    buildUi();

    connect(yesButton(), &QPushButton::clicked, this, [this] { onYesClicked(); });
    connect(cancelButton(), &QPushButton::clicked, this, [this] { onCancelClicked(); });
    connect(m_copyButton, &QPushButton::clicked, this, [this] { copyCode(); });
    connect(m_openButton, &QPushButton::clicked, this, [this] { openLink(); });

    m_countdown = new QTimer(this);
    m_countdown->setInterval(1000);
    connect(m_countdown, &QTimer::timeout, this, [this] { tickCountdown(); });

    startLogin();
}

AuthLoginDialog::~AuthLoginDialog() {
    // 析构时若有任务在跑:取消 + 脱开(不 wait —— 绝不让界面等在网络上;任务自己收尾后自删)
    if (m_task != nullptr) {
        m_task->cancel();
        m_task->disconnect(this);
        m_task->setParent(nullptr);
        if (m_task->running())
            connect(m_task, &AccountTask::finished, m_task, &QObject::deleteLater);
        else
            delete m_task;
        m_task = nullptr;
    }
}

AuthLoginDialog *AuthLoginDialog::open(QWidget *parent) {
    auto *dialog = new AuthLoginDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    return dialog;
}

void AuthLoginDialog::buildUi() {
    const ThemeTokens &tokens = FluentTheme::instance().tokens();

    m_heading = new SubtitleLabel(QStringLiteral("登录 Microsoft 账户"), widget());
    m_instructions = new BodyLabel(
        QStringLiteral("用浏览器打开下面的网址，输入这 8 位代码即可完成授权。\n"
                       "本窗口会自动等待，期间界面不会卡住；随时可以取消。"),
        widget());
    m_instructions->setWordWrap(true);

    m_linkLabel = new BodyLabel(QStringLiteral("（正在向微软申请设备码…）"), widget());
    m_linkLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_openButton = new PushButton(QStringLiteral("打开链接"), widget());
    applyButtonFont(m_openButton);
    m_openButton->setEnabled(false);

    m_codeLabel = new TitleLabel(QStringLiteral("········"), widget());
    m_codeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_copyButton = new PushButton(QStringLiteral("复制代码"), widget());
    applyButtonFont(m_copyButton);
    m_copyButton->setEnabled(false);

    m_countdownLabel = new CaptionLabel(QString(), widget());
    m_countdownLabel->setTextColor(tokens.textTertiary, tokens.textTertiary);
    m_statusLabel = new CaptionLabel(QString(), widget());
    m_statusLabel->setTextColor(tokens.textTertiary, tokens.textTertiary);

    m_errorLabel = new BodyLabel(QString(), widget());
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_errorLabel->setTextColor(tokens.danger, tokens.danger); // §2 令牌:danger
    m_errorLabel->hide();

    auto *linkRow = new QHBoxLayout();
    linkRow->setSpacing(8);
    linkRow->addWidget(m_linkLabel, 1);
    linkRow->addWidget(m_openButton);

    auto *codeRow = new QHBoxLayout();
    codeRow->setSpacing(12);
    codeRow->addWidget(m_codeLabel);
    codeRow->addWidget(m_copyButton);
    codeRow->addStretch(1);

    QVBoxLayout *view = viewLayout();
    view->addWidget(m_heading);
    view->addWidget(m_instructions);
    view->addLayout(linkRow);
    view->addLayout(codeRow);
    view->addWidget(m_countdownLabel);
    view->addWidget(m_statusLabel);
    view->addWidget(m_errorLabel);

    // 底部按钮(MessageBoxBase 自带的那两个):
    //   等待中 = [复制代码][取消];失败后 = [重试][关闭]。
    yesButton()->setText(QStringLiteral("复制代码"));
    yesButton()->setEnabled(false);
    cancelButton()->setText(QStringLiteral("取消"));

    // 网址与"核心库原文"都可能很长:不设上限时卡片会被拽到屏幕那么宽。
    widget()->setMaximumWidth(620);
}

void AuthLoginDialog::startLogin() {
    m_failed = false;
    m_succeeded = false;
    m_code.clear();
    m_verificationUri.clear();
    m_secondsLeft = 0;

    m_errorLabel->clear();
    m_errorLabel->hide();
    m_codeLabel->setText(QStringLiteral("········"));
    m_countdownLabel->clear();
    m_linkLabel->setText(QStringLiteral("（正在向微软申请设备码…）"));
    m_openButton->setEnabled(false);
    m_copyButton->setEnabled(false);
    yesButton()->setText(QStringLiteral("复制代码"));
    yesButton()->setEnabled(false);
    cancelButton()->setText(QStringLiteral("取消"));
    cancelButton()->setEnabled(true);
    setStatusText(QStringLiteral("正在向微软申请设备码…"));

    // 整条登录链交给 work 线程(AccountTask);UI 只收信号 —— 主线程不阻塞。
    m_task = new AccountTask(AccountTask::Operation::Login, this);
    connect(m_task, &AccountTask::userCode, this, &AuthLoginDialog::onUserCode);
    connect(m_task, &AccountTask::deviceCodeInfo, this, &AuthLoginDialog::onDeviceCodeInfo);
    connect(m_task, &AccountTask::statusText, this, &AuthLoginDialog::onStatus);
    connect(m_task, &AccountTask::finished, this, &AuthLoginDialog::onFinished);
    m_task->start();
}

void AuthLoginDialog::retry() {
    if (m_task != nullptr && m_task->running()) {
        setStatusText(QStringLiteral("上一次登录还在收尾，请稍候…"));
        return;
    }
    startLogin();
}

void AuthLoginDialog::onUserCode(const QString &code, const QString &verificationUri) {
    m_code = code;
    m_verificationUri = verificationUri;
    m_codeLabel->setText(code.isEmpty() ? QStringLiteral("(未给出)") : code);
    m_linkLabel->setText(verificationUri.isEmpty() ? QStringLiteral("(未给出验证网址)")
                                                   : verificationUri);
    m_openButton->setEnabled(!verificationUri.isEmpty());
    m_copyButton->setEnabled(!code.isEmpty());
    yesButton()->setEnabled(!code.isEmpty());
    setStatusText(QStringLiteral("等待你在浏览器里输入这串代码并同意授权…"));
}

void AuthLoginDialog::onDeviceCodeInfo(int intervalSeconds, int expiresInSeconds) {
    // 服务端要求:每 intervalSeconds 轮询一次;设备码在 expiresInSeconds 之后作废。
    m_secondsLeft = expiresInSeconds > 0 ? expiresInSeconds : 0;
    m_countdownLabel->setText(countdownText(m_secondsLeft));
    if (intervalSeconds > 0) {
        setStatusText(QStringLiteral("正在按服务端要求轮询（每 %1 秒一次）…").arg(intervalSeconds));
    }
    if (m_secondsLeft > 0 && m_countdown == nullptr) {
        // 不会发生(m_countdown 在构造函数里建好);写出来只是让时序一目了然。
    }
    if (m_countdown != nullptr && !m_countdown->isActive())
        m_countdown->start();
}

void AuthLoginDialog::tickCountdown() {
    if (m_secondsLeft > 0)
        --m_secondsLeft;
    if (m_secondsLeft <= 0) {
        m_countdownLabel->setText(countdownText(0));
        if (m_countdown != nullptr)
            m_countdown->stop();
        // 过期由核心库判定并回报(设备码有效期以微软给的 expires_in 为准),
        // 这里只把倒计时停在 00:00,不自己下结论。
        return;
    }
    m_countdownLabel->setText(countdownText(m_secondsLeft));
}

void AuthLoginDialog::onStatus(const QString &text) {
    if (m_failed)
        return; // 失败信息优先,不被后续进度覆盖
    setStatusText(text);
}

void AuthLoginDialog::onFinished(bool ok, const QString &message, const QString &rawError,
                                 int code) {
    if (m_countdown != nullptr)
        m_countdown->stop();

    if (ok) {
        m_succeeded = true;
        setStatusText(QStringLiteral("登录成功，账户状态已更新。"));
        emit accountChanged();
        accept(); // 成功后对话框自动关闭
        return;
    }
    if (code == SXCL_AUTH_ERR_CANCELLED) {
        // 用户自己点的取消 —— 安静收尾,不再弹错误。
        if (m_task != nullptr && !m_task->running())
            reject();
        return;
    }
    showFailure(message, rawError, code);
}

void AuthLoginDialog::showFailure(const QString &message, const QString &rawError, int code) {
    m_failed = true;
    yesButton()->setText(QStringLiteral("重试"));
    yesButton()->setEnabled(true);
    cancelButton()->setText(QStringLiteral("关闭"));
    cancelButton()->setEnabled(true);
    setStatusText(QStringLiteral("登录失败（返回码 %1）").arg(code), true);

    // **原样**展示核心库的 err:第 6 跳被 Mojang 应用资质挡住时,这里会一字不改地出现
    //   "用 Xbox 凭据换 Minecraft 令牌失败：…：Invalid app registration,
    //    see https://aka.ms/AppRegInfo for more information"
    // (docs/09 §9.2)。界面**不翻译、不改写、不吞掉**这条,用户才能拿着它去查。
    QString text = message;
    if (!rawError.isEmpty()) {
        if (!text.isEmpty())
            text += QStringLiteral("\n");
        text += QStringLiteral("核心库原文（原样）：") + rawError;
    }
    m_errorLabel->setText(text);
    m_errorLabel->show();
    m_copyButton->setEnabled(!m_code.isEmpty());
    m_openButton->setEnabled(!m_verificationUri.isEmpty());
}

void AuthLoginDialog::setStatusText(const QString &text, bool danger) {
    const ThemeTokens &tokens = FluentTheme::instance().tokens();
    m_statusLabel->setText(text);
    if (danger)
        m_statusLabel->setTextColor(tokens.danger, tokens.danger);
    else
        m_statusLabel->setTextColor(tokens.textTertiary, tokens.textTertiary);
}

void AuthLoginDialog::onYesClicked() {
    if (m_failed) {
        retry();
        return;
    }
    copyCode();
}

void AuthLoginDialog::onCancelClicked() {
    // 取消 = 告诉核心库别再轮询(should_cancel + transport cancel_all),然后**立刻**关窗:
    // 关窗不等网络(任务被脱开,自己收尾后自删)。
    if (m_task != nullptr)
        m_task->cancel();
    reject();
}

void AuthLoginDialog::copyCode() {
    if (m_code.isEmpty())
        return;
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(m_code);
    setStatusText(QStringLiteral("已复制代码 %1，去浏览器里粘贴即可。").arg(m_code));
}

void AuthLoginDialog::openLink() {
    if (m_verificationUri.isEmpty())
        return;
    QDesktopServices::openUrl(QUrl(m_verificationUri));
    setStatusText(QStringLiteral("已交给系统打开：%1").arg(m_verificationUri));
}

void AuthLoginDialog::done(int code) {
    // 关闭路径(accept/reject/窗口关闭)统一在这里:先取消并脱开后台任务,再做淡出。
    if (m_task != nullptr) {
        m_task->cancel();
        m_task->disconnect(this);
        m_task->setParent(nullptr);
        if (m_task->running())
            connect(m_task, &AccountTask::finished, m_task, &QObject::deleteLater);
        else
            delete m_task;
        m_task = nullptr;
    }
    MessageBoxBase::done(code);
}

} // namespace sxcl::ui
