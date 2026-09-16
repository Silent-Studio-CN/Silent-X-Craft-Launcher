/* SVG 归一化:把 viewBox 收紧到"墨迹包围盒"(自动留边距)。
 *
 * 为什么要它:PCL 的图标路径各自在自己的坐标系里画,有的几何很小(在 1024 视图框里几乎看不见),
 * 有的很大。UI 里要的是"视觉大小一致",而这正好等价于按墨迹包围盒归一化。
 * 纯 Qt 实现,可重复执行;每次重跑抽取脚本后跑一遍即可。
 *
 * 用法: sxcl-svgnorm <目录> [边距比例,默认 0.04]
 */
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <cstdio>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "用法: sxcl-svgnorm <目录> [pad]\n");
        return 2;
    }
    const QString dir = QString::fromLocal8Bit(argv[1]);
    const double pad = argc > 2 ? QString::fromLocal8Bit(argv[2]).toDouble() : 0.04;
    const int N = 512; /* 采样分辨率:够定位包围盒,又不慢 */
    QDir d(dir);
    const QStringList files = d.entryList(QStringList() << "*.svg", QDir::Files, QDir::Name);
    int changed = 0, skipped = 0;
    for (const QString &f : files) {
        const QString path = d.filePath(f);
        QFile in(path);
        if (!in.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QString text = QString::fromUtf8(in.readAll());
        in.close();

        QImage img(N, N, QImage::Format_ARGB32);
        img.fill(Qt::transparent);
        QSvgRenderer r(path);
        if (!r.isValid()) { ++skipped; continue; }
        {
            QPainter p(&img);
            r.render(&p);
        }
        int minX = N, minY = N, maxX = -1, maxY = -1;
        for (int y = 0; y < N; ++y) {
            const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < N; ++x) {
                if (qAlpha(line[x]) > 16) {
                    minX = qMin(minX, x); maxX = qMax(maxX, x);
                    minY = qMin(minY, y); maxY = qMax(maxY, y);
                }
            }
        }
        if (maxX < 0) { ++skipped; continue; } /* 空白图标不动它 */

        /* 采样像素 -> SVG 用户单位(视图框 1024) */
        const QRegularExpression vb(QStringLiteral("viewBox=\"([^\"]*)\""));
        const QRegularExpressionMatch m = vb.match(text);
        double vx = 0, vy = 0, vw = 1024, vh = 1024;
        if (m.hasMatch()) {
            const QStringList parts = m.captured(1).split(QRegularExpression(QStringLiteral("[ ,]+")),
                                                          Qt::SkipEmptyParts);
            if (parts.size() == 4) { vx = parts[0].toDouble(); vy = parts[1].toDouble();
                                     vw = parts[2].toDouble(); vh = parts[3].toDouble(); }
        }
        const double sx = vw / N, sy = vh / N;
        double bx = vx + minX * sx, by = vy + minY * sy;
        double bw = (maxX - minX + 1) * sx, bh = (maxY - minY + 1) * sy;
        const double px = bw * pad, py = bh * pad;
        bx -= px; by -= py; bw += 2 * px; bh += 2 * py;

        char buf[160];
        std::snprintf(buf, sizeof(buf), "viewBox=\"%.2f %.2f %.2f %.2f\"", bx, by, bw, bh);
        text.replace(m.capturedStart(0), m.capturedLength(0), QString::fromLatin1(buf));
        QFile out(path);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream ts(&out);
            ts.setEncoding(QStringConverter::Utf8);
            ts << text;
            out.close();
            ++changed;
        }
    }
    std::printf("归一化完成: 调整 %d 个, 跳过(空白/无效) %d 个\n", changed, skipped);
    return 0;
}
