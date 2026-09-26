// 老侧边栏(NavPanel)叠层实验台 —— 用户 2026-09-26 交代它存在的意义:
//   「我是用来给你累积经验的,不是为了写一个好玩儿的东西」
// 两条**要带回老界面**的结论,每条都能在日志里被判词断言:
//   ① 栏与栏**零间距**相邻,分隔线由容器在 paintEvent 里画 **恒定 1 个设备像素**
//      (QPen 宽度 0 = cosmetic)。踩过的坑:1px 控件当线在 dpr=1.5 上会占 1~2 个物理像素 ——
//      实测相邻边界一条 2px、一条 1px(肉眼"一粗一细");QFrame::VLine 更糟:Fusion 默认
//      调色板是浅色,直接画成白线。
//   ② **同一时刻最多一条栏展开**(Expanded 状态机),要么全收起。状态在**外壳**手里:
//      栏只喊"我展开了/我收起了",由外壳决定把别的收起来。
//
// 用法:
//   sxcl_rails_demo.exe                            手点:加一层 / 依次展开下一条 / 全部收起 / 打印几何
//   sxcl_rails_demo.exe --selftest                 自动:依次展开各条,每步量几何 + 判词
//   sxcl_rails_demo.exe --layers 4 --shot x.png    叠 4 层,截图到 x.png 后退出

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdio>

#include "nav.h" // 老界面的侧边栏本体(NavPanel)

using sxcl::ui::NavItem;
using sxcl::ui::NavPanel;

namespace {

/** 一条栏:就是老界面的 NavPanel(真控件),外面套一层给日志用的壳。 */
class Rail {
public:
    Rail(const QString &title, int index, QWidget *parent) : m_title(title), m_index(index) {
        m_panel = new NavPanel(parent);
        m_panel->addItem(NavItem{QStringLiteral("a"), QStringLiteral("Home"), QString(),
                                 QStringLiteral("%1 · 第一项").arg(title), false, -1, QString(),
                                 QString()});
        m_panel->addItem(NavItem{QStringLiteral("b"), QStringLiteral("Update"), QString(),
                                 QStringLiteral("%1 · 第二项").arg(title), false, -1, QString(),
                                 QString()});
        m_panel->addItem(NavItem{QStringLiteral("c"), QStringLiteral("Setting"), QString(),
                                 QStringLiteral("设置"), true, -1, QString(), QString()});
    }
    NavPanel *panel() const { return m_panel; }
    int index() const { return m_index; }
    QString title() const { return m_title; }

private:
    NavPanel *m_panel = nullptr;
    QString m_title;
    int m_index = 0;
};

class Bench : public QWidget {
public:
    explicit Bench(QWidget *parent = nullptr) : QWidget(parent) {
        m_row = new QHBoxLayout(this);
        m_row->setContentsMargins(0, 0, 0, 0);
        m_row->setSpacing(0); // ① 严丝合缝:栏与栏零间距
        addRail(QStringLiteral("主导航"));

        auto *content = new QWidget(this);
        auto *cv = new QVBoxLayout(content);
        cv->setContentsMargins(24, 20, 24, 20);
        cv->setSpacing(8);
        cv->addWidget(new QLabel(QStringLiteral("内容区(右边这块就是页面)"), content));
        m_log = new QLabel(QStringLiteral("判词实时打在窗口里,同时进 stderr"), content);
        m_log->setWordWrap(true);
        cv->addWidget(m_log);

        auto *add = new QPushButton(QStringLiteral("加一层"), content);
        connect(add, &QPushButton::clicked, this, [this] { addRail(QStringLiteral("子栏")); });
        cv->addWidget(add, 0, Qt::AlignLeft);
        auto *expand = new QPushButton(QStringLiteral("依次展开下一条"), content);
        connect(expand, &QPushButton::clicked, this, [this] {
            if (m_rails.isEmpty()) {
                return;
            }
            requestExpand((m_lastExpanded + 1) % (int)m_rails.size());
        });
        cv->addWidget(expand, 0, Qt::AlignLeft);
        auto *collapse = new QPushButton(QStringLiteral("全部收起"), content);
        connect(collapse, &QPushButton::clicked, this, [this] {
            for (Rail *r : m_rails) {
                if (!r->panel()->collapsed()) {
                    r->panel()->setCollapsed(true);
                }
            }
        });
        cv->addWidget(collapse, 0, Qt::AlignLeft);
        auto *dg = new QPushButton(QStringLiteral("打印几何"), content);
        connect(dg, &QPushButton::clicked, this, [this] { dump(QStringLiteral("手动")); });
        cv->addWidget(dg, 0, Qt::AlignLeft);
        cv->addStretch(1);
        m_row->addWidget(content, 1);
        /* **零延迟**:分隔线画在**容器**上,而子控件自己重绘不会带上父控件 ——
         * 动画期间父控件必须每帧 update(),否则线会滞后一帧(用户 2026-09-26:
         * 「检测很硬…能不能零延迟?体验不太好」)。 */
        /* **常开**的跟随定时器(用户 2026-09-26:「还是慢啊…你自动互动的时候不慢」——
         * 因为手点走的是 NavPanel 自己的汉堡,不经过 requestExpand,我原来只在脚本那条路上
         * 起了同步与落定判定,于是"手点"这条路人手一条时间线)。
         * 现在:每 16ms 看一遍每条栏的**实时宽度** ——
         *   变了  -> update() 重画分隔线(零延迟跟随) + 标记"还在动";
         *   240ms 没变 -> 落定,才出判词。两条路一视同仁。 */
        m_sync = new QTimer(this);
        m_sync->setInterval(16);
        connect(m_sync, &QTimer::timeout, this, [this] {
            bool changed = false;
            for (int i = 0; i < m_rails.size(); ++i) {
                const int w = m_rails[i]->panel()->width();
                while (m_lastW.size() <= i) { m_lastW.push_back(-1); }
                if (m_lastW[i] != w) { m_lastW[i] = w; changed = true; }
            }
            if (changed) {
                update();
                m_settled = false;
                m_quietMs = 0;
            } else if (!m_settled) {
                m_quietMs += 16;
                if (m_quietMs >= 240) { m_settled = true; m_quietMs = 0; dump(QStringLiteral("落定")); }
            }
        });
        m_sync->start();
        requestExpand(0); // 一进来展开第一条(不然全是 48 的"死人样")
    }

    void addRail(const QString &title) {
        auto *rail = new Rail(title, (int)m_rails.size() + 1, this);
        NavPanel *panel = rail->panel();
        m_row->insertWidget((int)m_rails.size(), panel); // 栏与栏直接相邻(分隔线不占布局)
        m_rails.push_back(rail);
        /* ② **状态机在这里**:栏只报"我展开了/我收起了",谁该收起来由外壳决定。 */
        connect(panel, &NavPanel::collapsedChanged, this, [this, rail](bool collapsed) {
            if (collapsed) {
                if (m_lastExpanded == rail->index() - 1) {
                    m_lastExpanded = -1;
                }
                dump(QStringLiteral("栏#%1 收起完成").arg(rail->index()));
                return;
            }
            m_lastExpanded = rail->index() - 1;
            for (Rail *other : m_rails) {
                if (other != rail && !other->panel()->collapsed()) {
                    std::fprintf(stderr, "[rails] 状态机:栏#%d 展开了 -> 把栏#%d 收起来\n",
                                 rail->index(), other->index());
                    other->panel()->setCollapsed(true);
                }
            }
            dump(QStringLiteral("栏#%1 展开完成").arg(rail->index()));
        });
        std::fprintf(stderr, "[rails] 加了一层:栏#%d(%s)\n", rail->index(),
                     title.toUtf8().constData());
    }

    void requestExpand(int index) {
        if (index < 0 || index >= m_rails.size()) {
            return;
        }
        m_rails[index]->panel()->setCollapsed(false);
    }

    /** 分隔线:恒定 1 个**设备**像素(见文件头 ①)。线跟着栏的**实时**位置走。 */
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setPen(QPen(QColor(54, 54, 62), 0));
        for (int i = 1; i < m_rails.size(); ++i) {
            const QPoint tl = m_rails[i]->panel()->mapTo(this, QPoint(0, 0));
            p.drawLine(tl.x(), 0, tl.x(), height());
        }
    }

    void dump(const QString &why) {
        QString line = QStringLiteral("[rails] %1: ").arg(why);
        int expanded = 0;
        for (int i = 0; i < m_rails.size(); ++i) {
            NavPanel *p = m_rails[i]->panel();
            if (!p->collapsed()) {
                ++expanded;
            }
            line += QStringLiteral("栏#%1(w=%2%3) ")
                        .arg(m_rails[i]->index())
                        .arg(p->width())
                        .arg(p->collapsed() ? QStringLiteral(",收") : QString());
        }
        line += QStringLiteral("| 展开条数=%1").arg(expanded);
        std::fprintf(stderr, "%s\n", line.toUtf8().constData());
        if (!m_settled) {
            std::fprintf(stderr, "[rails]   (动画中,不判:等 collapsedChanged 再量)\n");
            if (m_log != nullptr) { m_log->setText(line + QStringLiteral("\n动画中…")); }
            return;
        }
        bool flush = true;
        for (int i = 1; i < m_rails.size(); ++i) {
            const int gap = m_rails[i]->panel()->x() -
                            (m_rails[i - 1]->panel()->x() + m_rails[i - 1]->panel()->width());
            if (gap != 0) { // 分隔线不占布局:紧贴(0px)才叫严丝合缝
                flush = false;
                std::fprintf(stderr, "[rails]   !! 栏#%d 与 栏#%d 之间缝隙 = %d px(应为 0)\n", i + 1, i,
                             gap);
            }
        }
        std::fprintf(stderr, "[rails] 判词: 严丝合缝=%s 只伸一条=%s\n", flush ? "OK" : "FAIL",
                     expanded <= 1 ? "OK" : "FAIL");
        if (m_log != nullptr) {
            m_log->setText(line + QStringLiteral("\n严丝合缝=%1 只伸一条=%2")
                                      .arg(flush ? QStringLiteral("OK") : QStringLiteral("FAIL"),
                                           expanded <= 1 ? QStringLiteral("OK") : QStringLiteral("FAIL")));
        }
    }

    void selftest() {
        QTimer::singleShot(400, this, [this] { addRail(QStringLiteral("子栏A")); });
        QTimer::singleShot(900, this, [this] { addRail(QStringLiteral("子栏B")); });
        QTimer::singleShot(1500, this, [this] { requestExpand(1); });
        QTimer::singleShot(2300, this, [this] { requestExpand(2); });
        QTimer::singleShot(3100, this, [this] { requestExpand(0); });
        QTimer::singleShot(3900, this, [this] {
            for (Rail *r : m_rails) {
                if (!r->panel()->collapsed()) {
                    r->panel()->setCollapsed(true);
                }
            }
        });
        QTimer::singleShot(4700, this, [this] { dump(QStringLiteral("全收起之后")); });
        QTimer::singleShot(5100, this, [this] { qApp->quit(); });
    }

private:
    QHBoxLayout *m_row = nullptr;
    QVector<Rail *> m_rails;
    QLabel *m_log = nullptr;
    QTimer *m_sync = nullptr;      // 动画期间每帧重画分隔线(零延迟)
    bool m_settled = true;        // 动完了没有(判词只在 settled 时出)
    QVector<int> m_lastW;         // 上一次看到的每条栏宽度
    int m_quietMs = 0;            // 连续多少 ms 没变化
    int m_lastExpanded = -1; // 状态机:当前展开的是哪一条(-1 = 全收起)
};

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    Bench bench;
    bench.resize(1200, 700);
    bench.setWindowTitle(QStringLiteral("老侧边栏(NavPanel)叠层实验台"));
    bench.show();
    const QStringList args = app.arguments();
    if (args.contains(QStringLiteral("--selftest"))) {
        bench.selftest();
    }
    int layers = 1;
    QString shot;
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--layers") && i + 1 < args.size()) {
            layers = args[++i].toInt();
        } else if (args[i] == QLatin1String("--shot") && i + 1 < args.size()) {
            shot = args[++i];
        }
    }
    for (int i = 1; i < layers; ++i) {
        bench.addRail(QStringLiteral("子栏"));
    }
    if (!shot.isEmpty()) {
        QTimer::singleShot(1500, &app, [&bench, shot]() {
            const bool ok = bench.grab().save(shot);
            std::fprintf(stderr, "[rails] shot %s %s\n", shot.toUtf8().constData(),
                         ok ? "OK" : "FAILED");
            QCoreApplication::quit();
        });
    }
    return app.exec();
}
