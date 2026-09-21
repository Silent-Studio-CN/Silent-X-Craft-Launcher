/*
 * (C) Silent X Craft Launcher
 * Copyright by SilentStudio.
 * All rights reserved.
 */

#include "sxcl_icons.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QScreen>

namespace sxcl::ui {
namespace {

// Python icons.py:101-116 BLOCK_FILES
struct FileRow {
    const char *kind;
    const char *file;
};
const FileRow kFiles[] = {
    {"vanilla", "Grass.png"},        {"snapshot", "CommandBlock.png"},
    {"old", "CobbleStone.png"},      {"forge", "Anvil.png"},
    {"neoforge", "NeoForge.png"},    {"fabric", "Fabric.png"},
    {"quilt", "Fabric.png"},         {"optifine", "GrassPath.png"},
    {"liteloader", "Egg.png"},       {"error", "RedstoneBlock.png"},
    {"gold", "GoldBlock.png"},       {"optifabric", "OptiFabric.png"},
    {"lamp_on", "RedstoneLampOn.png"}, {"lamp_off", "RedstoneLampOff.png"},
};

// Python icons.py:122-128 screen_ratio()
qreal screenRatio() {
    if (const QScreen *s = QGuiApplication::primaryScreen())
        return s->devicePixelRatio();
    return 1.0;
}

} // namespace

SxclIcons &SxclIcons::instance() {
    static SxclIcons inst;
    return inst;
}

QStringList SxclIcons::kinds() {
    QStringList out;
    for (const FileRow &r : kFiles)
        out << QString::fromLatin1(r.kind);
    return out;
}

QString SxclIcons::blockFile(const QString &kind) {
    for (const FileRow &r : kFiles) {
        if (kind == QLatin1String(r.kind))
            return QString::fromLatin1(r.file);
    }
    return QString();
}

QString SxclIcons::stateIconKind(const QString &versionType, const QStringList &loaders, bool broken) {
    // Python icons.py:78-97
    if (broken)
        return QStringLiteral("error");
    static const char *kOrder[] = {"forge", "neoforge", "fabric", "quilt", "optifine", "liteloader"};
    for (const char *k : kOrder) {
        if (loaders.contains(QString::fromLatin1(k)))
            return QString::fromLatin1(k);
    }
    const QString t = versionType.toLower();
    if (t == QLatin1String("snapshot"))
        return QStringLiteral("snapshot");
    if (t == QLatin1String("old_alpha") || t == QLatin1String("old_beta") || t == QLatin1String("old"))
        return QStringLiteral("old");
    return QStringLiteral("vanilla");
}

void SxclIcons::setBlockDir(const QString &dir) { m_dir = dir; }

QString SxclIcons::blockDir() const { return m_dir; }

bool SxclIcons::resolveBlockDir() {
    QStringList candidates;
#ifdef SXCL_UI_BLOCK_DIR
    candidates << QString::fromLatin1(SXCL_UI_BLOCK_DIR);
#endif
    const QString env = qEnvironmentVariable("SXCL_BLOCK_DIR");
    if (!env.isEmpty())
        candidates << env;
    candidates << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/icons/blocks"));
    for (const QString &c : candidates) {
        if (QFileInfo::exists(c + QStringLiteral("/Grass.png"))) {
            m_dir = c;
            return true;
        }
    }
    return false;
}

bool SxclIcons::hasAsset(const QString &kind) const {
    const QString file = blockFile(kind);
    if (file.isEmpty() || m_dir.isEmpty())
        return false;
    return QFileInfo::exists(m_dir + QLatin1Char('/') + file);
}

QPixmap SxclIcons::source(const QString &kind) {
    if (m_sourceCache.contains(kind))
        return m_sourceCache.value(kind);
    if (m_dir.isEmpty() && !resolveBlockDir()) {
        m_sourceCache.insert(kind, QPixmap());
        return QPixmap();
    }
    const QString file = blockFile(kind);
    QPixmap pm;
    if (!file.isEmpty())
        pm.load(m_dir + QLatin1Char('/') + file);
    m_sourceCache.insert(kind, pm);
    return pm;
}

QPixmap SxclIcons::blockPixmap(const QString &kind, int size) {
    // Python icons.py:187-201
    const qreal ratio = screenRatio();
    const int physical = qMax(1, int(qRound(size * ratio)));
    const QString key = kind + QLatin1Char('#') + QString::number(physical);
    if (m_scaledCache.contains(key))
        return m_scaledCache.value(key);

    const QPixmap src = source(kind);
    if (src.isNull())
        return QPixmap(); // 不复制 Python 的 _drawn_block 死代码
    QPixmap out = src;
    if (src.width() != physical) {
        const bool exact = physical > 0 && src.width() % physical == 0;
        out = src.scaled(physical, physical, Qt::KeepAspectRatio,
                         exact ? Qt::FastTransformation : Qt::SmoothTransformation);
    }
    if (!qFuzzyCompare(ratio, 1.0))
        out.setDevicePixelRatio(ratio);
    m_scaledCache.insert(key, out);
    return out;
}

QIcon SxclIcons::blockIcon(const QString &kind, int size) {
    // Python icons.py:203-208:多尺寸 {size, max(16, size/2), size*2}
    QList<int> sides;
    sides << size << qMax(16, size / 2) << size * 2;
    std::sort(sides.begin(), sides.end());
    QIcon icon;
    int last = -1;
    for (int s : sides) {
        if (s == last)
            continue;
        last = s;
        const QPixmap pm = blockPixmap(kind, s);
        if (!pm.isNull())
            icon.addPixmap(pm);
    }
    return icon;
}

QPixmap SxclIcons::grassBlockPixmap(int size) { return blockPixmap(QStringLiteral("vanilla"), size); }

QIcon SxclIcons::grassBlockIcon(int size) { return blockIcon(QStringLiteral("vanilla"), size); }

} // namespace sxcl::ui
