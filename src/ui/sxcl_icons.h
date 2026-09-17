#pragma once
// sxcl_icons —— 方块图标(对应 Python 版 src/app/icons.py)
//
// 语义与缩放规则逐条照抄 Python 版:
//   * 数据源:assets/icons/blocks/*.png(13 个,64x64),键盘映射见 BLOCK_FILES;
//   * DPR:物理尺寸 = round(size * 屏幕缩放比),生成后 setDevicePixelRatio 标回去,
//     这样 125%/150% 屏上不会糊(icons.py:187-201 的"图标分辨率极低"根因修复);
//   * 缩放:整数倍用最近邻(FastTransformation),非整数倍才平滑(icons.py:164-179);
//   * QIcon 多尺寸:size、max(16,size/2)、size*2(icons.py:203-208);
//   * 状态映射:state_icon_kind()(icons.py:78-97)与 BLOCK_FILES(icons.py:101-116)。
// 注意:Python 版 `icons.py:197` 会在缺资源时调用 `_drawn_block`,而该函数**在仓库里没有定义**
// (死代码;因 assets 存在所以从不触发)。C 版不复制这个 bug:缺资源时返回空图并回报一次。
#include <QHash>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace sxcl::ui {

class SxclIcons {
public:
    static SxclIcons &instance();

    // 资产目录:编译期 SXCL_UI_BLOCK_DIR -> 环境变量 SXCL_BLOCK_DIR -> exe 旁 assets/icons/blocks
    void setBlockDir(const QString &dir);
    QString blockDir() const;
    bool resolveBlockDir();

    bool hasAsset(const QString &kind) const;
    static QStringList kinds();

    // 逻辑尺寸 size,内部按 DPR 放大
    QPixmap blockPixmap(const QString &kind, int size = 24);
    QIcon blockIcon(const QString &kind, int size = 24);
    QPixmap grassBlockPixmap(int size = 24);
    QIcon grassBlockIcon(int size = 24);

    // 状态 -> 图标种类(原版/快照/旧版/各加载器/出错)
    static QString stateIconKind(const QString &versionType, const QStringList &loaders, bool broken = false);
    // kind -> PNG 文件名(对应 Python BLOCK_FILES)
    static QString blockFile(const QString &kind);

private:
    SxclIcons() = default;
    QPixmap source(const QString &kind); // 原图,带缓存

    QString m_dir;
    QHash<QString, QPixmap> m_sourceCache;
    QHash<QString, QPixmap> m_scaledCache;
};

} // namespace sxcl::ui
