/**
 * @file test_form_call.cpp
 * @brief 通话窗体（`src/ui/form_call.cpp`）单元测试 —— 重点是 **stopsend 的发源地**。
 *
 * 为什么这个文件特别值得测
 * ------------------------
 * `src/ui/form_call.cpp` 是门口机**唯一**发出 `stopsend` 信令的地方：
 *
 *     connect(ui->btnstop, &QPushButton::clicked, this, [this]() {
 *         QWidget* w = parentWidget();
 *         while (w && !qobject_cast<Widget*>(w)) w = w->parentWidget();
 *         if (widget) {
 *             widget->audioManager()->stop();     // 打断提示音
 *             widget->notifyPeerStopSend();       // -> TCP 8887 "stopsend\n"
 *         }
 *         emit hangupRequested();
 *     });
 *
 * `stopsend` 在硬约束清单里（TCP 8887 信令语义必须与 baseline 逐字节兼容）。
 * 在此之前：
 *
 *   - `protocol.h` 里 `Signal::StopSend` 与 wire 串 `"stopsend"` 的映射**有**
 *     测试（`test_protocol.cpp`：toWire / fromWire / 前缀歧义）；
 *   - `SignalingClient::send(StopSend)` 会写出 `"stopsend\n"` **有**测试
 *     （`test_signaling_client.cpp`、interop）；
 *   - 但**"点下停止按钮 → stopsend 真的被发出去"这条接线，从未被任何测试
 *     目标链接**（`form_call.cpp` 一直挂在"从未被链接的产线文件"清单上）。
 *
 * 也就是说，两头都钉住了，**中间那一段没人守**。把 `widget->notifyPeerStopSend()`
 * 那一行删掉，此前全部 15 个测试目标都会照常通过 —— 门口机会变成"点了挂断，
 * 但室内机不知道本端已停发"。这正是"有测试 ≠ 被测到"。
 *
 * 本测试的做法
 * ------------
 * 起一个**真实** `QTcpServer` 监听，用**真实** `SignalingClient` 连上它，把
 * 真实 `Widget` 替身挂成 `Form_CALL` 的父窗口，然后 `QTest::mouseClick` 真的
 * 点下停止按钮，最后断言**对端 socket 收到的字节恰好是 `"stopsend\n"`**。
 *
 * 断言落在线上字节上，而不是"某个标志位变成了 true"。这样任何一处接线断裂
 * （不发、发错信令、发多次、发成 `"stop\n"`）都会红。
 *
 * 替换了什么、没替换什么
 * ----------------------
 *   | 用真的 | 用替身 |
 *   |---|---|
 *   | `form_call.cpp` / `form_call.ui`（真 uic） | `Widget`（tests/shadows/ui/widget.h） |
 *   | **真实** `SignalingClient` + 真实 TCP socket | `AudioManager`（影子头，避免真 ALSA） |
 *   | `QPushButton` 与真实信号槽连接、`hangupRequested` 信号 | — |
 *
 * 唯一的替身是 `Widget`。它的 `notifyPeerStopSend()` 是 3 行真实现的副本
 * （真 `widget.cpp` 因依赖 OpenCV 与整个主程序门面而无法链接），这一点在
 * 影子头里**如实标注**了；真实现本身由静态规则
 * `check_form_call_stopsend_is_wired` 钉住。
 */
#include <QApplication>
#include <QDebug>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtTest>

#include <functional>

#include "config/app_config.h"
#include "core/net/protocol.h"
#include "core/net/signaling_client.h"
#include "managers/audio_manager.h"
#include "ui/form_call.h"
#include "ui/widget.h"

using accessterminal::SignalingClient;
using accessterminal::protocol::Signal;

// QSignalSpy 要把自定义枚举塞进 QVariant：Q_DECLARE_METATYPE 只保证
// value<T>() 能编译，跨队列投递还需要运行期 qRegisterMetaType（见 README 第 29 条）。
Q_DECLARE_METATYPE(accessterminal::protocol::Signal)

namespace {

/// 等一个条件成立，最多 timeoutMs 毫秒。**绝不**用固定时长 sleep。
bool waitFor(const std::function<bool()>& pred, int timeoutMs = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(5);
    }
    return pred();
}

/// 造一个"干净"的挂断按钮点击环境：真实 server + 真实 client + 真父窗口。
struct Fixture {
    QTcpServer server;
    SignalingClient client;
    Widget mainWin;
    AudioManager mgr;
    QTcpSocket* peer = nullptr;
    Form_CALL* form = nullptr;

    /// 起监听、连上、接受连接，返回是否成功。
    bool up()
    {
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            qWarning() << "[form-call-test] 无法监听:" << server.errorString();
            return false;
        }
        client.start(QStringLiteral("127.0.0.1"), server.serverPort());
        if (!waitFor([this] { return server.hasPendingConnections(); })) {
            qWarning() << "[form-call-test] 客户端未连上";
            return false;
        }
        peer = server.nextPendingConnection();
        if (!peer) {
            return false;
        }
        if (!waitFor([this] { return client.isConnected(); })) {
            qWarning() << "[form-call-test] isConnected 未变真";
            return false;
        }
        mainWin.setAudioManagerForTest(&mgr);
        mainWin.setSignalingClientForTest(&client);
        form = new Form_CALL(&mainWin);
        return true;
    }

    ~Fixture() { client.stop(); }

    /// 对端已收到的全部字节。
    QByteArray received()
    {
        if (!peer) {
            return QByteArray();
        }
        peer->waitForReadyRead(0);
        return peer->readAll();
    }

    /// 等对端收到 n 字节。
    QByteArray awaitBytes(int n, int timeoutMs = 5000)
    {
        QByteArray acc;
        QElapsedTimer t;
        t.start();
        while (acc.size() < n && t.elapsed() < timeoutMs) {
            if (peer->waitForReadyRead(20)) {
                acc += peer->readAll();
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        return acc;
    }

    QPushButton* stopButton() const
    {
        return form->findChild<QPushButton*>(QStringLiteral("btnstop"));
    }

    QLabel* label() const
    {
        return form->findChild<QLabel*>(QStringLiteral("label"));
    }

    /// 点下停止按钮。
    void clickStop()
    {
        QPushButton* b = stopButton();
        QVERIFY(b != nullptr);
        QTest::mouseClick(b, Qt::LeftButton);
    }
};

} // namespace

class TestFormCall : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // ---- 界面装配 ----
    void stopButtonAndLabelExist();
    void stopButtonDoesNotTakeFocus();
    void callFormIsTranslucent();

    // ---- 本文件的核心：stopsend 的线上字节 ----
    void clickingStopSendsExactlyStopSendOnTheWire();
    void stopSendUsesTheProtocolFrameNotALiteral();
    void clickingStopSendsStopSendOnlyOnce();
    void noStopSendWhenNotConnected();
    void stopSendDoesNotGetSentBeforeTheButtonIsClicked();
    void clickingStopTwiceSendsTwoStopSends();

    // ---- 挂断信号 ----
    void clickingStopEmitsHangupRequested();
    void hangupRequestedIsEmittedEvenWithoutAWidgetParent();
    void clickingStopDoesNotCloseTheWindowItself();

    // ---- 提示音打断 ----
    void clickingStopStopsTheAudioManager();
    void noAudioStopWhenParentIsNotWidget();

    // ---- 计时器 ----
    void startCallTimerShowsZeroAndRestartsTheCounter();
    void stopCallTimerFreezesTheDisplay();
    void setStatusTextOverridesTheLabel();

    // ---- 正对照 ----
    void tcpRoundTripWorksInThisFixture();
};

void TestFormCall::initTestCase()
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    qRegisterMetaType<accessterminal::protocol::Signal>(
        "accessterminal::protocol::Signal");
}

void TestFormCall::init()
{
    // 每个用例都重新读一遍配置，避免上一个用例的残留。
    accessterminal::AppConfig::instance().load(QString());
}

// ---------------------------------------------------------------------------
// 界面装配
// ---------------------------------------------------------------------------

void TestFormCall::stopButtonAndLabelExist()
{
    Fixture f;
    QVERIFY(f.up());
    QVERIFY(f.stopButton() != nullptr);
    QVERIFY(f.label() != nullptr);
}

void TestFormCall::stopButtonDoesNotTakeFocus()
{
    // 门口机是触摸屏：停止键抢焦点会导致按下态显示异常。构造函数里显式设了
    // NoFocus，这是产品行为，钉住。
    Fixture f;
    QVERIFY(f.up());
    QCOMPARE(f.stopButton()->focusPolicy(), Qt::NoFocus);
}

void TestFormCall::callFormIsTranslucent()
{
    // 通话窗体是叠在主界面上的浮层，必须透明背景，否则会挡住摄像头画面。
    Fixture f;
    QVERIFY(f.up());
    QVERIFY2(f.form->testAttribute(Qt::WA_TranslucentBackground),
             "通话窗体必须保持半透明背景（浮层），见 form_call.cpp:22");
}

// ---------------------------------------------------------------------------
// stopsend 的线上字节（本文件的核心）
// ---------------------------------------------------------------------------

void TestFormCall::clickingStopSendsExactlyStopSendOnTheWire()
{
    // 这是本文件存在的首要理由：把"点下停止按钮"和"TCP 8887 上出现
    // stopsend\n"之间的接线钉住。此前两头都有测试，中间这段没人守。
    Fixture f;
    QVERIFY(f.up());

    // 正对照：先确认这条 TCP 通路真的能用（否则下面的断言可能真空成立）。
    QVERIFY(f.client.send(Signal::Call));
    QCOMPARE(f.awaitBytes(5), QByteArray("call\n"));

    f.clickStop();

    // 断言落在**对端真正收到的字节**上。
    const QByteArray got = f.awaitBytes(9);
    QCOMPARE(got, QByteArray("stopsend\n"));
}

void TestFormCall::stopSendUsesTheProtocolFrameNotALiteral()
{
    // 除了"收到了 stopsend\n"，还要确认它**以 \n 结尾**（协议要求逐字节兼容）。
    // 只看 contains("stopsend") 是不够的：漏掉换行会让室内机的行解码器一直
    // 等下一个包，表现为"点了挂断，室内机没反应"。
    Fixture f;
    QVERIFY(f.up());
    f.clickStop();

    const QByteArray got = f.awaitBytes(9);
    QVERIFY2(got.endsWith('\n'),
             qPrintable(QStringLiteral("stopsend 必须以 \\n 结尾，实际=%1")
                            .arg(QString::fromLatin1(got.toHex()))));
    QCOMPARE(got, QByteArray("stopsend\n"));
    // 不能是 "stop\n"（"stop" 是 "stopsend" 的前缀，语义完全不同：
    // stop 是"停止取流"，stopsend 是"本端停止发送"）。
    QVERIFY2(got != QByteArray("stop\n"),
             "发成了 stop 而不是 stopsend —— 前缀歧义导致语义错误");
}

void TestFormCall::clickingStopSendsStopSendOnlyOnce()
{
    // 第 35 轮在 form_passwd 上发现的真实缺陷形态：uic 的
    // `connectSlotsByName` 与显式 connect 叠加，会让槽跑两遍。form_call 的
    // 停止按钮是 **lambda** 连接（名字不符合 on_<对象名>_<信号名> 约定），
    // 所以理论上不会叠加 —— 但这条断言把结论钉死，将来若有人把它改成
    // 具名槽 `on_btnstop_clicked`，就会立刻红。
    Fixture f;
    QVERIFY(f.up());
    f.clickStop();
    QCOMPARE(f.awaitBytes(9), QByteArray("stopsend\n"));

    // 给足时间，确认**没有第二份**跟着来。
    QTest::qWait(150);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCOMPARE(f.received(), QByteArray());
    QCOMPARE(f.mainWin.notifyStopSendCalls, 1);
}

void TestFormCall::noStopSendWhenNotConnected()
{
    // 未连接时点停止：不得崩，也不得往未打开的 socket 上写。
    // SignalingClient::send() 内部已做"未连接则告警并返回 false"的判断
    // （重构前的等价判断在 Form_CALL 里，见 notifyPeerStopSend 的注释）。
    Fixture f;
    QVERIFY(f.up());
    f.client.stop();  // 主动断开
    QVERIFY(waitFor([&f] { return !f.client.isConnected(); }));

    QSignalSpy spy(f.form, &Form_CALL::hangupRequested);
    f.clickStop();

    // 挂断信号仍要发（这是 UI 契约，与网络无关）。
    QCOMPARE(spy.count(), 1);
    // 但不应有字节到达对端。
    QTest::qWait(120);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QVERIFY2(f.received().isEmpty(),
             "未连接时不应写出任何字节");
}

void TestFormCall::stopSendDoesNotGetSentBeforeTheButtonIsClicked()
{
    // **负面断言**：构造窗体本身不得发出 stopsend。
    // 缺了它，"点一下收到 stopsend"可能是构造时就发了、而点击什么都没做
    // —— 那样断言照样通过（README 第 28 条）。
    Fixture f;
    QVERIFY(f.up());

    QTest::qWait(150);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QVERIFY2(f.received().isEmpty(),
             "还没点停止按钮，就不该有任何信令被发出");
    QCOMPARE(f.mainWin.notifyStopSendCalls, 0);
}

void TestFormCall::clickingStopTwiceSendsTwoStopSends()
{
    // 按钮不是一次性的：现场可能连点。两次点击必须发两条，不能只发第一条
    // （那说明连接被误当成"只连一次"的 one-shot）。
    Fixture f;
    QVERIFY(f.up());

    f.clickStop();
    QCOMPARE(f.awaitBytes(9), QByteArray("stopsend\n"));
    f.clickStop();
    QCOMPARE(f.awaitBytes(9), QByteArray("stopsend\n"));

    QCOMPARE(f.mainWin.notifyStopSendCalls, 2);
}

// ---------------------------------------------------------------------------
// 挂断信号
// ---------------------------------------------------------------------------

void TestFormCall::clickingStopEmitsHangupRequested()
{
    Fixture f;
    QVERIFY(f.up());
    QSignalSpy spy(f.form, &Form_CALL::hangupRequested);
    QCOMPARE(spy.count(), 0);

    f.clickStop();
    QCOMPARE(spy.count(), 1);
}

void TestFormCall::hangupRequestedIsEmittedEvenWithoutAWidgetParent()
{
    // 父链上没有 Widget 时：audioManager/notifyPeerStopSend 都不执行，
    // 但 hangupRequested **必须照发** —— 否则挂断会彻底失灵
    // （这是个真实的失败模式：窗体被单独构造时就是这种父子关系）。
    Form_CALL form;  // 没有 Widget 祖先
    QSignalSpy spy(&form, &Form_CALL::hangupRequested);

    QPushButton* b = form.findChild<QPushButton*>(QStringLiteral("btnstop"));
    QVERIFY(b != nullptr);
    QTest::mouseClick(b, Qt::LeftButton);

    QCOMPARE(spy.count(), 1);
}

void TestFormCall::clickingStopDoesNotCloseTheWindowItself()
{
    // 源码注释写着"不主动关闭窗口，交由主界面处理"（form_call.cpp:52）。
    // 这是刻意的职责划分：窗体自己关掉会让主界面的后续状态机拿不到
    // hangupRequested 之后的收尾时机。
    Fixture f;
    QVERIFY(f.up());

    // 用 isHidden() 而不是 isVisible() 来判定"有没有被关掉"。
    //
    // 为什么：isVisible() 要求**它和所有祖先**都被 show 过。本窗体是
    // Widget 替身的子控件，而父窗口没有 show()（测的是窗体自身行为，不该
    // 依赖父窗口状态），所以在 offscreen 下 isVisible() 恒为 false ——
    // 用它做断言既测不到 close()，还会因为无关原因失败（第一版就踩了）。
    // isHidden() 直接对应 close()/hide() 的效果，与祖先状态无关。
    //
    // 正对照（紧跟其后）：证明这条判据**真的能**检出被关掉的情况 ——
    // 主动 close() 一次，isHidden() 必须变真。没有这一步，下面的断言可能
    // 恒为真而毫无判别力（README 第 41 条）。
    QVERIFY2(!f.form->isHidden(), "刚构造的窗体不应处于已隐藏状态");
    f.form->close();
    QVERIFY2(f.form->isHidden(),
             "正对照：close() 之后 isHidden() 必须为真，否则本用例的判据无效");
    f.form->show();
    QVERIFY2(!f.form->isHidden(), "show() 之后应当回到未隐藏状态");

    f.clickStop();
    QTest::qWait(80);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    QVERIFY2(!f.form->isHidden(),
             "Form_CALL 不应自己关闭窗口，应由主界面处理"
             "（源码注释：\"不主动关闭窗口，交由主界面处理\"）");
}

// ---------------------------------------------------------------------------
// 提示音打断
// ---------------------------------------------------------------------------

void TestFormCall::clickingStopStopsTheAudioManager()
{
    // 点停止要打断主界面正在播的提示音（calling.pcm / conn.pcm）。
    Fixture f;
    QVERIFY(f.up());
    f.mgr.resetForTest();

    f.clickStop();

    QCOMPARE(f.mgr.stopCalls, 1);
}

void TestFormCall::noAudioStopWhenParentIsNotWidget()
{
    // 没有 Widget 祖先时不得崩（真 AudioManager 指针取不到）。
    Form_CALL form;
    QPushButton* b = form.findChild<QPushButton*>(QStringLiteral("btnstop"));
    QVERIFY(b != nullptr);

    QSignalSpy spy(&form, &Form_CALL::hangupRequested);
    QTest::mouseClick(b, Qt::LeftButton);

    QCOMPARE(spy.count(), 1);
}

// ---------------------------------------------------------------------------
// 计时器
// ---------------------------------------------------------------------------

void TestFormCall::startCallTimerShowsZeroAndRestartsTheCounter()
{
    Fixture f;
    QVERIFY(f.up());

    // 先让计数跑起来，制造"非零"状态。
    f.form->startCallTimer();
    QTest::qWait(1200);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const QString afterRun = f.label()->text();
    QVERIFY2(afterRun != QStringLiteral("通话中 00:00"),
             qPrintable(QStringLiteral("计时器应当推进，实际=%1").arg(afterRun)));

    // 再次 start 必须**归零**。
    f.form->startCallTimer();
    QCOMPARE(f.label()->text(), QStringLiteral("通话中 00:00"));

    // 但上面那一行**没有判别力**：`startCallTimer()` 末尾无条件写了
    // `ui->label->setText("通话中 00:00")`，所以即使把 `callSeconds = 0;`
    // 删掉，标签照样是 00:00 —— 第 36 轮故障注入实测的**假绿**（rc=0）。
    //
    // 真正的可观测差异在**下一次 tick 的秒数**：
    //   归零了   -> 下一个 tick 是 00:01
    //   没归零   -> 接着上次的数往下走（比如 00:02、00:03）
    // 所以这里**等标签第一次变化**再断言它恰好是 00:01。
    //
    // 注意这是"等一个性质成立"而不是"睡固定时长"：tick 间隔 1000ms、轮询
    // 10ms，第一次观察到的变化必然是 00:01，与机器快慢无关（README 第 27 条）。
    QElapsedTimer t;
    t.start();
    QString firstChange;
    while (firstChange.isEmpty() && t.elapsed() < 4000) {
        QTest::qWait(10);
        const QString now = f.label()->text();
        if (now != QStringLiteral("通话中 00:00")) {
            firstChange = now;
        }
    }
    QVERIFY2(!firstChange.isEmpty(), "归零后计时器应当继续走");
    QCOMPARE(firstChange, QStringLiteral("通话中 00:01"));
}

void TestFormCall::stopCallTimerFreezesTheDisplay()
{
    Fixture f;
    QVERIFY(f.up());

    f.form->startCallTimer();
    QTest::qWait(1200);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    f.form->stopCallTimer();

    const QString frozen = f.label()->text();
    QTest::qWait(1200);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QCOMPARE(f.label()->text(), frozen);
}

void TestFormCall::setStatusTextOverridesTheLabel()
{
    Fixture f;
    QVERIFY(f.up());

    f.form->setStatusText(QStringLiteral("被叫已挂断"));
    QCOMPARE(f.label()->text(), QStringLiteral("被叫已挂断"));

    // 计时器仍在跑时，setStatusText 的文本允许被下一次 tick 覆盖 ——
    // 这里只断言"立即生效"，不去规定它与 tick 的先后（那是竞态）。
}

// ---------------------------------------------------------------------------
// 正对照
// ---------------------------------------------------------------------------

void TestFormCall::tcpRoundTripWorksInThisFixture()
{
    // 上面所有"对端收到 X 字节"的断言都依赖这条 TCP 通路真的能用。
    // 若 fixture 坏了（没连上、端口没监听），那些断言会**真空成立**。
    // 这里发一条已知信令并断言对端收到，证明通路是通的。
    Fixture f;
    QVERIFY(f.up());

    QVERIFY(f.client.send(Signal::Unlock));
    QCOMPARE(f.awaitBytes(7), QByteArray("unlock\n"));

    QVERIFY(f.client.send(Signal::StopSend));
    QCOMPARE(f.awaitBytes(9), QByteArray("stopsend\n"));
}

QTEST_MAIN(TestFormCall)
#include "test_form_call.moc"
