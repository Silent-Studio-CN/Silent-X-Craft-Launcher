// 侧边栏"叠层 + 折叠"演示与自检(docs/27 §2 的实验台)。
//
// 用户 2026-09-26:「给我一个测试,有 LOG 的 DEMO,点一下按钮侧边栏就加一层,看 BUG/特性」。
// 所以这个程序就干两件事:
//   1. 手点:窗口左边两条栏(可再叠第三条),点栏头的 ☰ 折叠/展开 —— 窗口标题栏下面那排按钮里
//      「加一层」每点一次就多一条栏;每条栏都是**外壳的槽**(兄弟关系,中间那条分隔线由容器画),
//      不是"页面里自己拼的两列"。
//   2. 自检:--selftest 跑一遍"加层 -> 逐条折叠/展开 -> 量几何",把每一步的
//      x/宽/栏间缝隙/伸出条数打进日志,最后给出两条判词:
//        严丝合缝: 相邻两栏的 left 必须等于前一条的 right(容器 spacing=0 + 1px 分隔线)
//        只伸一条: 同一时刻展开的栏数必须 <= 1(Expanded 状态机在外壳手里)
//
// 为什么单独做成 demo:老界面里这两条是"每个页面自己拼两列"的写法,量不出干净的数;
// 这里把变量收敛到最小,先证明"能严丝合缝、能只伸一条",再回老界面照这个改。

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cstdio>

namespace {

constexpr int kWide = 200;
constexpr int kNarrow = 48;

/** 一条栏:栏头(汉堡 + 标题) + 若干行。折叠 = 宽度在 kWide/kNarrow 之间动 220ms OutCubic。 */
class Rail : public QWidget {
public:
    Rail(const QString &title, int index, QWidget *parent = nullptr)
        : QWidget(parent), m_title(title), m_index(index) {
        setFixedWidth(kWide);
        auto *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        auto *head = new QWidget(this);
        auto *h = new QHBoxLayout(head);
        h->setContentsMargins(6, 6, 6, 6);
        h->setSpacing(6);
        auto *burger = new QToolButton(head);
        burger->setText(QStringLiteral("☰"));
        burger->setFixedSize(28, 28);
        burger->setToolTip(QStringLiteral("折叠 / 展开这一条"));
        connect(burger, &QToolButton::clicked, this, [this] { toggle(); });
        h->addWidget(burger);
        m_label = new QLabel(m_title, head);
        h->addWidget(m_label);
        h->addStretch(1);
        v->addWidget(head);
        for (int i = 0; i < 4; ++i) {
            auto *row = new QLabel(QStringLiteral("· %1 行 %2").arg(m_title).arg(i + 1), this);
            row->setContentsMargins(12, 6, 12, 6);
            v->addWidget(row);
        }
        v->addStretch(1);
    }

    bool collapsed() const { return m_collapsed; }
    int index() const { return m_index; }

    void toggle() { setCollapsed(!m_collapsed); }

    /** 折叠/展开:只为演示与自检,动画结束会打一行日志(量几何用)。 */
    void setCollapsed(bool on) {
        if (m_collapsed == on && width() == (on ? kNarrow : kWide)) {
            return;
        }
        m_collapsed = on;
        m_label->setVisible(!on);
        const int from = width();
        const int to = on ? kNarrow : kWide;
        auto *ani = new QPropertyAnimation(this, "fixedWidth", this);
        ani->setStartValue(from);
        ani->setEndValue(to);
        ani->setDuration(220);
        ani->setEasingCurve(QEasingCurve::OutCubic);
        ani->start(QAbstractAnimation::DeleteWhenStopped);
        std::fprintf(stderr, "[rails] 栏#%d %s (%d -> %d, 220ms OutCubic)\n", m_index,
                     on ? "收起" : "展开", from, to);
    }

private:
    QString m_title;
    int m_index = 0;
    bool m_collapsed = false;
    QLabel *m_label = nullptr;
};

/** 外壳:一个 QHBoxLayout(spacing=0) 装所有栏 + 内容区;分隔线由容器画(这里用 1px 的 QFrame)。 */
class Shell : public QWidget {
public:
    explicit Shell(QWidget *parent = nullptr) : QWidget(parent) {
        m_row = new QHBoxLayout(this);
        m_row->setContentsMargins(0, 0, 0, 0);
        m_row->setSpacing(0); // **严丝合缝的关键**:栏与栏之间零间距,只由 1px 分隔线分开
        addRail(QStringLiteral("主导航"));

        auto *content = new QWidget(this);
        auto *cv = new QVBoxLayout(content);
        cv->setContentsMargins(24, 20, 24, 20);
        cv->setSpacing(10);
        auto *t = new QLabel(QStringLiteral("内容区"), content);
        cv->addWidget(t);
        m_log = new QLabel(QStringLiteral("日志写在 stderr,也可以点下面按钮看几何"), content);
        m_log->setWordWrap(true);
        cv->addWidget(m_log);
        auto *add = new QPushButton(QStringLiteral("加一层"), content);
        connect(add, &QPushButton::clicked, this, [this] { addRail(QStringLiteral("子栏")); });
        cv->addWidget(add, 0, Qt::AlignLeft);
        auto *dumpBtn = new QPushButton(QStringLiteral("打印几何"), content);
        connect(dumpBtn, &QPushButton::clicked, this, [this] { dump(QStringLiteral("手动")); });
        cv->addWidget(dumpBtn, 0, Qt::AlignLeft);
        auto *toggle = new QPushButton(QStringLiteral("折叠/展开 第 1 条"), content);
        connect(toggle, &QPushButton::clicked, this, [this] {
            for (Rail *r : m_rails) {
                if (r->index() == 1) {
                    r->toggle();
                    break;
                }
            }
        });
        cv->addWidget(toggle, 0, Qt::AlignLeft);
        cv->addStretch(1);
        m_row->addWidget(content, 1);
        QTimer::singleShot(300, this, [this] { dump(QStringLiteral("初始")); });
    }

    void addRail(const QString &title) {
        auto *rail = new Rail(title, (int)m_rails.size() + 1, this);
        m_row->insertWidget((int)m_rails.size(), rail); // 栏依次插在内容区左边
        if (!m_rails.isEmpty()) {
            auto *sep = new QFrame(this); // 分隔线:1px,由**容器**画(不让栏自己留 margin)
            sep->setFixedWidth(1);
            sep->setFrameShape(QFrame::VLine);
            m_row->insertWidget((int)m_rails.size(), sep);
            m_seps.push_back(sep);
        }
        m_rails.push_back(rail);
        if (m_rails.size() > 1) {
            QTimer::singleShot(320, this, [this, title] { dump(QStringLiteral("加了一层(%1)").arg(title)); });
        }
    }

    /** 把几何打出来 —— 这就是"有 LOG 的 demo"的 LOG 本体。 */
    void dump(const QString &why) {
        QString line = QStringLiteral("[rails] %1: ").arg(why);
        int expanded = 0;
        for (int i = 0; i < m_rails.size(); ++i) {
            Rail *r = m_rails[i];
            if (!r->collapsed()) {
                ++expanded;
            }
            line += QStringLiteral("栏#%1(x=%2 w=%3%4) ")
                        .arg(r->index())
                        .arg(r->x())
                        .arg(r->width())
                        .arg(r->collapsed() ? QStringLiteral(",收") : QString());
        }
        line += QStringLiteral("| 展开条数=%1").arg(expanded);
        std::fprintf(stderr, "%s\n", line.toUtf8().constData());
        // 两条判词(严丝合缝 / 只伸一条)—— 这两条就是要拿去老界面照做的
        bool flush = true;
        for (int i = 1; i < m_rails.size(); ++i) {
            const int gap = m_rails[i]->x() - (m_rails[i - 1]->x() + m_rails[i - 1]->width());
            if (gap != 1) { // 1 = 那条分隔线
                flush = false;
                std::fprintf(stderr, "[rails]   !! 栏#%d 与 栏#%d 之间缝隙 = %d px(应为 1)\n", i + 1, i,
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
        QTimer::singleShot(200, this, [this] { addRail(QStringLiteral("子栏A")); });
        QTimer::singleShot(700, this, [this] { addRail(QStringLiteral("子栏B")); });
        QTimer::singleShot(1200, this, [this] {
            std::fprintf(stderr, "[rails] === 下面演示「同时伸两条」会发生什么 ===\n");
            if (m_rails.size() >= 2) {
                m_rails[0]->toggle(); // 展开第 1 条(此时第 2/3 条也是展开的)
            }
        });
        QTimer::singleShot(2000, this, [this] { dump(QStringLiteral("三条都展开之后")); });
        QTimer::singleShot(2400, this, [this] { qApp->quit(); });
    }

private:
    QHBoxLayout *m_row = nullptr;
    QVector<Rail *> m_rails;
    QVector<QWidget *> m_seps;
    QLabel *m_log = nullptr;
};

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    Shell shell;
    shell.resize(900, 560);
    shell.setWindowTitle(QStringLiteral("侧边栏叠层实验台(rails demo)"));
    shell.show();
    if (app.arguments().contains(QStringLiteral("--selftest"))) {
        shell.selftest();
    }
    return app.exec();
}
