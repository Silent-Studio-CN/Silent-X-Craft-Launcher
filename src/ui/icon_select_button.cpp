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

/* 图标目录 assets/icons/<sub> 的解析顺序(与 SxclIcons / IconRegistry / ui_icons 同一套口径):
 *   1) 编译期钉死的 SXCL_UI_EDITION_DIR(CMake 注入,**只对 edition 这一个子目录有意义**);
 *   2) 环境变量 SXCL_EDITION_DIR(取证时可以指到别处,同样只管 edition);
 *   3) 可执行文件旁的 assets/icons/<sub>(装机后按 exe 相对路径找);
 *   4) SXCL_UI_BLOCK_DIR(assets/icons/blocks)的**兄弟目录** <sub> —— 与 ui_icons.cpp
 *      从 blocks 推 assets/icons/ui 是同一个办法,少一个编译期开关也不会错位。
 * sub = "edition"(下载页版本形态)/ "ui"(主页正版·离线两枚,见 assets/icons/ui/NOTICE.md)。 */
QString iconAssetDir(const QString &sub) {
    static QHash<QString, QString> cache;
    const QHash<QString, QString>::const_iterator hit = cache.constFind(sub);
    if (hit != cache.constEnd())
        return hit.value();

    QStringList candidates;
    if (sub == QLatin1String("edition")) {
#ifdef SXCL_UI_EDITION_DIR
        candidates << QString::fromLatin1(SXCL_UI_EDITION_DIR);
#endif
        const QString env = qEnvironmentVariable("SXCL_EDITION_DIR");
        if (!env.isEmpty())
            candidates << env;
    }
    candidates << QDir(QCoreApplication::applicationDirPath())
                      .filePath(QStringLiteral("assets/icons/") + sub);
#ifdef SXCL_UI_BLOCK_DIR
    {
        QString blocks = QString::fromLatin1(SXCL_UI_BLOCK_DIR);
        const int cut = blocks.lastIndexOf(QLatin1Char('/'));
        candidates << (cut > 0 ? blocks.left(cut) : blocks) + QLatin1Char('/') + sub;
    }
#endif
    QString found;
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c)) {
            found = c;
            break;
        }
    }
    cache.insert(sub, found);
    return found;
}

QString assetPath(const QString &sub, const QString &file) {
    const QString dir = iconAssetDir(sub);
    return dir.isEmpty() ? QString() : dir + QLatin1Char('/') + file;
}

// 素材的原始像素尺寸:svg 取 viewBox 尺寸,png 只读文件头(不解码整图)
QSize assetNaturalSize(const QString &sub, const QString &file) {
    static QHash<QString, QSize> cache;
    if (file.isEmpty())
        return QSize();
    const QString key0 = sub + QLatin1Char('#') + file;
    if (cache.contains(key0))
        return cache.value(key0);
    const QString path = assetPath(sub, file);
    QSize size;
    if (file.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)) {
        QSvgRenderer renderer(path);
        if (renderer.isValid())
            size = renderer.defaultSize();
    } else {
        QImageReader reader(path);
        size = reader.size();
    }
    cache.insert(key0, size);
    return size;
}

// 图标位图缓存:同一份文件 + 同一个物理盒尺寸只解码/渲染一次(悬停会反复重绘)
QPixmap iconPixmap(const QString &sub, const QString &file, const QSize &box) {
    static QHash<QString, QPixmap> cache;
    if (file.isEmpty() || box.isEmpty())
        return QPixmap();
    const QString path = assetPath(sub, file);
    if (path.isEmpty())
        return QPixmap();
    const qreal dpr = screenDpr();
    const int pw = qMax(1, int(qRound(box.width() * dpr)));
    const int ph = qMax(1, int(qRound(box.height() * dpr)));
    const QString key = sub + QLatin1Char('#') + file + QLatin1Char('#') + QString::number(pw) +
                        QLatin1Char('x') + QString::number(ph);
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

/* 单色线稿的着色:SourceIn 保留 alpha、把颜色换成调用时给的**令牌色**(现取,切主题跟着变)。
 * 缓存键带上颜色 —— 主题一换,同一份(文件,盒子)要取到另一份。 */
QPixmap tintedPixmap(const QString &sub, const QString &file, const QSize &box,
                     const QColor &color) {
    static QHash<QString, QPixmap> cache;
    if (file.isEmpty() || box.isEmpty() || !color.isValid())
        return QPixmap();
    const QString key = sub + QLatin1Char('#') + file + QLatin1Char('@') +
                        color.name(QColor::HexArgb) + QLatin1Char('#') + QString::number(box.width()) +
                        QLatin1Char('x') + QString::number(box.height());
    const QHash<QString, QPixmap>::const_iterator hit = cache.constFind(key);
    if (hit != cache.constEnd())
        return hit.value();
    const QPixmap src = iconPixmap(sub, file, box);
    if (src.isNull()) {
        cache.insert(key, QPixmap());
        return QPixmap();
    }
    QPixmap out = src; // 浅拷贝;下面 begin 时自动 detach
    QPainter p(&out);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(out.rect(), color);
    p.end();
    cache.insert(key, out);
    return out;
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

void IconSelectButton::setIconDir(const QString &dir) {
    const QString next = dir.trimmed().isEmpty() ? QStringLiteral("edition") : dir.trimmed();
    if (next == m_iconDir)
        return;
    m_iconDir = next;
    updateGeometry(); // 素材尺寸变了 -> 图标盒宽度跟着变
    update();
}

void IconSelectButton::setIconBoxSize(const QSize &size) {
    if (m_boxOverride == size)
        return;
    m_boxOverride = size;
    updateGeometry();
    update();
}

void IconSelectButton::setVertical(bool on) {
    if (m_vertical == on)
        return;
    m_vertical = on;
    updateGeometry(); // 长宽对调 -> 按钮自己的尺寸跟着变
    update();
}

void IconSelectButton::setMonochrome(bool on) {
    if (on == m_monochrome)
        return;
    m_monochrome = on;
    update();
}

void IconSelectButton::setIconHeight(int height) {
    m_iconHeight = qMax(8, height);
    updateGeometry();
    update();
}

QSize IconSelectButton::rawIconBoxSize() const {
    // 宽度由素材横纵比算出来:宽幅 LOGO 得到宽盒,近正方形的咖啡杯得到方盒;
    // 调用方指定过绘制盒(setIconBoxSize)就按它来 —— 两枚横纵比不同的图标要摆在一起时,
    // 尺寸一致这件事只能由调用方钉。
    if (m_boxOverride.isValid() && !m_boxOverride.isEmpty())
        return m_boxOverride;
    const QSize natural = assetNaturalSize(m_iconDir, m_iconFile);
    if (natural.isEmpty() || natural.height() <= 0)
        return QSize(m_iconHeight, m_iconHeight);
    const int w = qMax(1, int(qRound(double(m_iconHeight) * natural.width() / natural.height())));
    return QSize(w, m_iconHeight);
}

QSize IconSelectButton::iconBoxSize() const {
    // 屏幕上的那一份:竖起来画时,未旋转盒的长宽对调
    const QSize raw = rawIconBoxSize();
    return m_vertical ? QSize(raw.height(), raw.width()) : raw;
}

QSize IconSelectButton::sizeHint() const {
    /* 指示条画在控件内部,所以两种状态、两颗图标的高度都一样,选中/取消选中时布局不跳。
     *   * 横着画:高 = 上边距 + 图标 + (4 间距 + 2 指示条) + 下边距;
     *   * 竖着画:指示条跑到图标**右边**(它就是"图标下方那条线"跟着图标转了 90°),
     *     所以改成宽 = 左边距 + 图标 + (4 间距 + 2 指示条) + 右边距,高 = 图标 + 上下边距。 */
    const QSize box = iconBoxSize();
    if (m_vertical)
        return QSize(box.width() + kPad + kIndicatorGap + kIndicatorH + kPadBottom,
                     box.height() + 2 * kPad);
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

    // ---- 图标:居中、整数逻辑矩形(亚像素会让像素画发糊)----
    //   * 横着画:水平居中、贴上边距;
    //   * 竖着画:垂直居中、贴左边距(整枚图标逆时针转 90°,像书脊那样从下往上读)。
    const QSize raw = rawIconBoxSize();     // 渲染成这一份(未旋转)
    const QSize screenBox = iconBoxSize();  // 屏幕上占这一块(竖着画时是 raw 的长宽对调)
    const QRect target = m_vertical
                             ? QRect(QPoint(kPad, (height() - screenBox.height()) / 2), screenBox)
                             : QRect(QPoint((width() - screenBox.width()) / 2, kPad), screenBox);
    /* 图标:彩色素材原样画(微软四色 LOGO 的识别度就在这里);单色线稿按令牌现染 ——
     * 未选中的断线用次要文字色,选中的用 accent(与下面那条指示条同一个令牌)。 */
    const QPixmap pm =
        m_monochrome ? tintedPixmap(m_iconDir, m_iconFile, raw,
                                    isChecked() ? ThemeBridge::instance().accent()
                                                : ThemeBridge::instance().token(
                                                      QStringLiteral("textSecondary")))
                     : iconPixmap(m_iconDir, m_iconFile, raw);
    if (!pm.isNull()) {
        if (!m_vertical) {
            p.drawPixmap(target.topLeft(), pm);
        } else {
            p.save();
            p.translate(target.center());
            p.rotate(-90);
            p.drawPixmap(QPoint(-raw.width() / 2, -raw.height() / 2), pm);
            p.restore();
        }
    }

    // ---- 选中态:那条 2 逻辑像素的指示条(长度 = 图标自己的长度)----
    // 竖着画时它**跟着图标一起转** —— 用户 2026-09-27:「它底色的那条蓝线也变成竖的」。
    if (isChecked()) {
        p.setPen(Qt::NoPen);
        p.setBrush(ThemeBridge::instance().accent());
        if (!m_vertical) {
            const QRectF bar(target.left(), target.bottom() + 1 + kIndicatorGap, target.width(),
                             kIndicatorH);
            p.drawRoundedRect(bar, kIndicatorH / 2.0, kIndicatorH / 2.0);
        } else {
            p.save();
            p.translate(target.center());
            p.rotate(-90);
            const QRectF bar(-raw.width() / 2.0, raw.height() / 2.0 + 1 + kIndicatorGap,
                             raw.width(), kIndicatorH);
            p.drawRoundedRect(bar, kIndicatorH / 2.0, kIndicatorH / 2.0);
            p.restore();
        }
    }
}

} // namespace sxcl::ui
