/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

// 图标点选钮的实现(理由与素材口径见头文件)。

#include "icon_select_button.h"

#include "theme_bridge.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QSvgRenderer>

namespace sxcl::ui {

namespace {

// 图标外留的边距:悬停/选中那层圆角底色才不至于贴着图标(24x24 钮 + 20 图标 = 各边 2)
constexpr int kPad = 2;
// 圆角:docs/27 §1「圆角只有三档:12 卡片 / 10 按钮 / 6 小标签」—— 这是小标签那一档
constexpr int kRadius = 6;

qreal screenDpr() {
    if (const QScreen *s = QGuiApplication::primaryScreen())
        return s->devicePixelRatio();
    return 1.0;
}

/* 原版图标目录 assets/icons/edition 的解析顺序(与 SxclIcons / IconRegistry 同一套口径):
 *   1) 编译期钉死的 SXCL_UI_EDITION_DIR(CMake 注入,开发期就是源码树里的那份);
 *   2) 环境变量 SXCL_EDITION_DIR(取证时可以指到别处);
 *   3) 可执行文件旁的 assets/icons/edition(装机后按 exe 相对路径找);
 *   4) SXCL_UI_BLOCK_DIR(assets/icons/blocks)的**兄弟目录** edition —— 与 ui_icons.cpp
 *      从 blocks 推 assets/icons/ui 是同一个办法,少一个编译期开关也不会错位。 */
QString editionDir() {
    static QString cached;
    static bool resolved = false;
    if (resolved)
        return cached;
    resolved = true;
    QStringList candidates;
#ifdef SXCL_UI_EDITION_DIR
    candidates << QString::fromLatin1(SXCL_UI_EDITION_DIR);
#endif
    const QString env = qEnvironmentVariable("SXCL_EDITION_DIR");
    if (!env.isEmpty())
        candidates << env;
    candidates << QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("assets/icons/edition"));
#ifdef SXCL_UI_BLOCK_DIR
    {
        QString blocks = QString::fromLatin1(SXCL_UI_BLOCK_DIR);
        const int cut = blocks.lastIndexOf(QLatin1Char('/'));
        candidates << (cut > 0 ? blocks.left(cut) : blocks) + QStringLiteral("/edition");
    }
#endif
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c)) {
            cached = c;
            return cached;
        }
    }
    cached.clear();
    return cached;
}

// 图标位图缓存:同一份文件 + 同一个物理尺寸只解码/渲染一次(悬停会反复重绘)
QPixmap iconPixmap(const QString &file, int size) {
    static QHash<QString, QPixmap> cache;
    if (file.isEmpty() || size <= 0)
        return QPixmap();
    const QString dir = editionDir();
    if (dir.isEmpty())
        return QPixmap();
    const qreal dpr = screenDpr();
    const int physical = qMax(1, int(qRound(size * dpr)));
    const QString key = file + QLatin1Char('#') + QString::number(physical);
    if (cache.contains(key))
        return cache.value(key);

    const QString path = dir + QLatin1Char('/') + file;
    QPixmap pm;
    if (file.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            cache.insert(key, QPixmap());
            return QPixmap();
        }
        QSvgRenderer renderer(f.readAll());
        if (!renderer.isValid()) {
            cache.insert(key, QPixmap());
            return QPixmap();
        }
        pm = QPixmap(physical, physical);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        // 等比放进正方形(原图 viewBox 不是正方形时也不变形),居中
        const QSizeF def = renderer.defaultSize();
        QRectF box(0, 0, physical, physical);
        if (def.width() > 0 && def.height() > 0 && !qFuzzyCompare(def.width(), def.height())) {
            if (def.width() > def.height())
                box.setHeight(physical * def.height() / def.width());
            else
                box.setWidth(physical * def.width() / def.height());
            box.moveCenter(QPointF(physical / 2.0, physical / 2.0));
        }
        renderer.render(&p, box);
    } else {
        QPixmap src(path);
        if (src.isNull()) {
            cache.insert(key, QPixmap());
            return QPixmap();
        }
        pm = src;
        if (src.width() != physical) {
            const bool exact = physical > 0 && src.width() % physical == 0;
            pm = src.scaled(physical, physical, Qt::KeepAspectRatio,
                            exact ? Qt::FastTransformation : Qt::SmoothTransformation);
        }
    }
    pm.setDevicePixelRatio(dpr);
    cache.insert(key, pm);
    return pm;
}

} // namespace

IconSelectButton::IconSelectButton(QWidget *parent) : QAbstractButton(parent) {
    setCheckable(true);                // 选中态 = "当前版本形态",点选切换
    setCursor(Qt::PointingHandCursor); // 能被猜到是可点的
    setFocusPolicy(Qt::StrongFocus);   // 键盘可达(docs/27 §4)
    setAttribute(Qt::WA_Hover, true);
}

void IconSelectButton::setIconFile(const QString &file) {
    m_iconFile = file;
    updateGeometry();
    update();
}

void IconSelectButton::setIconSide(int side) {
    m_iconSide = qMax(8, side);
    updateGeometry();
    update();
}

QSize IconSelectButton::sizeHint() const {
    return QSize(m_iconSide + 2 * kPad, m_iconSide + 2 * kPad);
}

void IconSelectButton::enterEvent(QEnterEvent *event) {
    m_hover = true;
    update();
    QAbstractButton::enterEvent(event);
}

void IconSelectButton::leaveEvent(QEvent *event) {
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void IconSelectButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                     QPainter::SmoothPixmapTransform);
    p.setPen(Qt::NoPen);

    // ---- 底色:全部现取主题令牌(换主题只重画,不重建控件)----
    const bool dark = ThemeBridge::instance().isDark();
    const QColor contrast(dark ? 255 : 0, dark ? 255 : 0, dark ? 255 : 0); // 深色叠白/浅色叠黑
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (isChecked()) {
        // 选中态 = 强调色描边 + 强调色淡底(令牌 accent,不写死颜色;与导航指示条同一个色源)
        const QColor accent = ThemeBridge::instance().accent();
        QColor fill = accent;
        fill.setAlpha(dark ? 64 : 44);
        p.setPen(QPen(accent, 1));
        p.setBrush(fill);
        p.drawRoundedRect(box, kRadius, kRadius);
        p.setPen(Qt::NoPen);
    } else if (m_hover && isEnabled()) {
        // 悬停一档提亮:与导航条目同一档(10% 对比色)
        QColor hover = contrast;
        hover.setAlpha(10);
        p.setBrush(hover);
        p.drawRoundedRect(box, kRadius, kRadius);
    }

    if (isDown())
        p.setOpacity(0.7); // 按下反馈与导航条目一致
    if (!isEnabled())
        p.setOpacity(0.4);

    // ---- 图标:居中,整数逻辑矩形(亚像素会让像素画发糊)----
    const QRect iconBox(QPoint((width() - m_iconSide) / 2, (height() - m_iconSide) / 2),
                        QSize(m_iconSide, m_iconSide));
    const QPixmap pm = iconPixmap(m_iconFile, m_iconSide);
    if (!pm.isNull())
        p.drawPixmap(iconBox, pm);
}

} // namespace sxcl::ui
