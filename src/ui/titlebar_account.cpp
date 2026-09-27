/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "titlebar_account.h"

#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QRectF>

#include "dialogs/account.h" // loadAccountSnapshot / defaultTokenPath
#include "skin_image.h"
#include "skin_store.h"
#include "theme_bridge.h"

namespace sxcl::ui {
namespace {

constexpr int kPadH = 8;  // 左右内边距
constexpr int kGap = 6;   // 头像与名字之间
constexpr int kNamePixelSize = 13; // 与 qf 的 #titleLabel 同字号

QColor textColor() {
    return ThemeBridge::instance().isDark() ? QColor(Qt::white) : QColor(Qt::black);
}

} // namespace

TitlebarAccount::TitlebarAccount(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("titlebarAccountChip"));
    setFixedHeight(kHeight);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setToolTip(QStringLiteral("账户"));
    QObject::connect(&SkinStore::instance(), &SkinStore::changed, this, [this] { syncAvatar(); });
    syncAvatar();
}

QSize TitlebarAccount::sizeHint() const {
    QFont font = const_cast<TitlebarAccount *>(this)->font();
    font.setPixelSize(kNamePixelSize);
    const QFontMetrics metrics(font);
    const int nameWidth = m_playerName.isEmpty() ? 0 : (kGap + metrics.horizontalAdvance(m_playerName));
    return QSize(2 * kPadH + kAvatarSide + nameWidth, kHeight);
}

void TitlebarAccount::setPlayerName(const QString &name) {
    if (m_playerName == name)
        return;
    m_playerName = name;
    updateGeometry();
    update();
}

void TitlebarAccount::syncAvatar() {
    const QImage skin = SkinStore::instance().skin();
    m_hasSkin = !skin.isNull();
    update();
}

void TitlebarAccount::refresh() {
    const QString path = defaultTokenPath();
    if (path != m_tokenPath) {
        m_tokenPath = path;
        m_tokenStamp = -1;
    }
    qint64 stamp = 0;
    if (!path.isEmpty()) {
        const QFileInfo info(path);
        stamp = info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0;
    }
    if (stamp == m_tokenStamp)
        return; // 令牌文件没动过:不重复解密
    m_tokenStamp = stamp;

    const AccountSnapshot account = loadAccountSnapshot();
    /* 「正版账户登录完的玩家」判据 = **登录过 + 有 Java 版玩家名**:
     *   * 没登录 / 没有 Java 版档案(玩家名空)-> 名字空,只留一个占位头像(不写解释文字);
     *   * 令牌过期也算"登录过"(玩家名还在):名字照显示,皮肤先吃磁盘缓存那一份 ——
     *     要重新登录就点退出登录(用户口径:"哪有那么多事儿")。 */
    const bool haveProfile = account.loggedIn && !account.playerName.isEmpty();
    m_uuid = haveProfile ? account.uuid : QString();
    SkinStore::instance().setAccount(m_uuid);
    setPlayerName(haveProfile ? account.playerName : QString());
    syncAvatar();
}

void TitlebarAccount::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const bool dark = ThemeBridge::instance().isDark();
    const int c = dark ? 255 : 0;
    if (m_hover) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(c, c, c, 26)); // 与三键悬停同一档(qf: rgba(255,255,255,26))
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    }

    const qreal dpr = devicePixelRatioF();
    const QRectF avatarBox(kPadH, (height() - kAvatarSide) / 2.0, kAvatarSide, kAvatarSide);
    const QImage skin = SkinStore::instance().skin();
    QPixmap head = skin.isNull() ? QPixmap() : skinHeadPixmap(skin, int(qRound(kAvatarSide * dpr)));
    if (head.isNull())
        head = skinHeadPlaceholder(int(qRound(kAvatarSide * dpr)));
    head.setDevicePixelRatio(dpr);
    p.drawPixmap(avatarBox.topLeft(), head);

    if (!m_playerName.isEmpty()) {
        QFont font = this->font();
        font.setPixelSize(kNamePixelSize);
        p.setFont(font);
        p.setPen(textColor());
        const QRectF textBox(avatarBox.right() + kGap, 0,
                             width() - avatarBox.right() - kGap - kPadH, height());
        p.drawText(textBox, Qt::AlignVCenter | Qt::AlignLeft, m_playerName);
    }
}

void TitlebarAccount::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        emit clicked();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TitlebarAccount::enterEvent(QEnterEvent *event) {
    m_hover = true;
    update();
    QWidget::enterEvent(event);
}

void TitlebarAccount::leaveEvent(QEvent *event) {
    m_hover = false;
    update();
    QWidget::leaveEvent(event);
}

} // namespace sxcl::ui
