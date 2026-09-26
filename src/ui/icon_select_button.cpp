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
#include <QImageReader>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QRectF>
#include <QScreen>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QSvgRenderer>

namespace sxcl::ui {

namespace {

// 图标上/左右留的边距(悬停底色不至于贴着图标)
constexpr int kPad = 2;
// 圆角:docs/27 §1「圆角只有三档:12 卡片 / 10 按钮 / 6 小标签」—— 这是小标签那一档
constexpr int kRadius = 6;
/* 选中态 = 图标**下方一条指示条**(用户 2026-09-26 最终口径:「你不会在他的 logo 下面画条线吗?
 * 你整那种死老丑的那个蓝色框给它圈起来是啥意思啊?」):
 *   * 颜色走主题令牌 accent(不写死);
 *   * 厚度 2 逻辑像素、长度 = 图标自身宽度(不超出图标左右缘);
 *   * 紧贴图标下缘留 4 逻辑像素间距;未选中不画(hover 也不预显 —— 那样验收里"未选中那颗
 *     没有 accent 像素"就不是确定性的了,而这条是我们要拿像素证明的)。 */
constexpr int kIndicatorGap = 4;
constexpr int kIndicatorH = 2;
// 指示条下面再留一点,免得它贴着控件边缘
constexpr int kPadBottom = 2;

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

QString assetPath(const QString &file) {
    const QString dir = editionDir();
    return dir.isEmpty() ? QString() : dir + QLatin1Char('/') + file;
}

// 素材的原始像素尺寸:svg 取 viewBox 尺寸,png 只读文件头(不解码整图)
QSize assetNaturalSize(const QString &file) {
    static QHash<QString, QSize> cache;
    if (file.isEmpty())
        return QSize();
    if (cache.contains(file))
        return cache.value(file);
    const QString path = assetPath(file);
    QSize size;
    if (file.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)) {
        QSvgRenderer renderer(path);
        if (renderer.isValid())
            size = renderer.defaultSize();
    } else {
        QImageReader reader(path);
        size = reader.size();
    }
    cache.insert(file, size);
    return size;
}

// 图标位图缓存:同一份文件 + 同一个物理盒尺寸只解码/渲染一次(悬停会反复重绘)
QPixmap iconPixmap(const QString &file, const QSize &box) {
    static QHash<QString, QPixmap> cache;
    if (file.isEmpty() || box.isEmpty())
        return QPixmap();
    const QString path = assetPath(file);
    if (path.isEmpty())
        return QPixmap();
    const qreal dpr = screenDpr();
    const int pw = qMax(1, int(qRound(box.width() * dpr)));
    const int ph = qMax(1, int(qRound(box.height() * dpr)));
    const QString key = file + QLatin1Char('#') + QString::number(pw) + QLatin1Char('x') +
                        QString::number(ph);
    if (cache.contains(key))
        return cache.value(key);

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
        pm = QPixmap(pw, ph);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        // 等比放进盒子(viewBox 不是正方形时也不变形),居中
        const QSizeF def = renderer.defaultSize();
        QRectF target(0, 0, pw, ph);
        if (def.width() > 0 && def.height() > 0) {
            const qreal scale =
                qMin(pw / double(def.width()), ph / double(def.height()));
            const QSizeF fitted(def.width() * scale, def.height() * scale);
            target = QRectF(0, 0, fitted.width(), fitted.height());
            target.moveCenter(QPointF(pw / 2.0, ph / 2.0));
        }
        renderer.render(&p, target);
    } else {
        QPixmap src(path);
        if (src.isNull()) {
            cache.insert(key, QPixmap());
            return QPixmap();
        }
        if (src.width() == pw && src.height() == ph) {
            pm = src; // 原样(整数倍放大时也走这条)
        } else if (src.width() < pw && pw % src.width() == 0 && ph % src.height() == 0) {
            // 像素画放大:整数倍 -> 最近邻,不发糊
            pm = src.scaled(pw, ph, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        } else if (src.width() > pw * 3) {
            // 大图缩小(官方 LOGO 1937px 宽 -> 200px 上下):先两段缩,避免细笔画被采样漏掉
            const QPixmap mid = src.scaled(pw * 3, ph * 3, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
            pm = mid.scaled(pw, ph, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        } else {
            pm = src.scaled(pw, ph, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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

void IconSelectButton::setIconHeight(int height) {
    m_iconHeight = qMax(8, height);
    updateGeometry();
    update();
}

QSize IconSelectButton::iconBoxSize() const {
    // 宽度由素材横纵比算出来:宽幅 LOGO 得到宽盒,近正方形的咖啡杯得到方盒
    const QSize natural = assetNaturalSize(m_iconFile);
    if (natural.isEmpty() || natural.height() <= 0)
        return QSize(m_iconHeight, m_iconHeight);
    const int w = qMax(1, int(qRound(double(m_iconHeight) * natural.width() / natural.height())));
    return QSize(w, m_iconHeight);
}

QSize IconSelectButton::sizeHint() const {
    // 高度 = 上边距 + 图标 + (4 间距 + 2 指示条) + 下边距 —— 指示条画在控件内部,
    // 所以两种状态、两颗图标的高度都一样,选中/取消选中时布局不跳。
    const QSize box = iconBoxSize();
    return QSize(box.width() + 2 * kPad,
                 box.height() + kPad + kIndicatorGap + kIndicatorH + kPadBottom);
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

    // ---- 悬停反馈:一档提亮(与导航条目同一档 10% 对比色);选中态不再是底色/方框 ----
    const bool dark = ThemeBridge::instance().isDark();
    const QColor contrast(dark ? 255 : 0, dark ? 255 : 0, dark ? 255 : 0); // 深色叠白/浅色叠黑
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (m_hover && isEnabled()) {
        QColor hover = contrast;
        hover.setAlpha(10);
        p.setPen(Qt::NoPen);
        p.setBrush(hover);
        p.drawRoundedRect(box, kRadius, kRadius);
    }

    if (isDown())
        p.setOpacity(0.7); // 按下反馈与导航条目一致
    if (!isEnabled())
        p.setOpacity(0.4);

    // ---- 图标:水平居中、贴上边距,整数逻辑矩形(亚像素会让像素画发糊)----
    const QSize iconBox = iconBoxSize();
    const QRect target(QPoint((width() - iconBox.width()) / 2, kPad), iconBox);
    const QPixmap pm = iconPixmap(m_iconFile, iconBox);
    if (!pm.isNull())
        p.drawPixmap(target, pm);

    // ---- 选中态:图标**下方**那条指示条(2 逻辑像素厚,长度 = 图标宽度)----
    if (isChecked()) {
        const QRectF bar(target.left(), target.bottom() + 1 + kIndicatorGap, target.width(),
                         kIndicatorH);
        p.setPen(Qt::NoPen);
        p.setBrush(ThemeBridge::instance().accent());
        p.drawRoundedRect(bar, kIndicatorH / 2.0, kIndicatorH / 2.0);
    }
}

} // namespace sxcl::ui
