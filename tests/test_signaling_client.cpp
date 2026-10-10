/*
 * test_signaling_client.cpp — SignalingClient（门口机 TCP 8887 信令客户端）
 *
 * 为什么补这一组：第 24 轮首次量化覆盖率后发现 src/core/net/signaling_client.cpp
 * 有 16/61 行从未执行 —— 恰好是**发送失败**与**断线重连**这两条路径：
 *
 *   47-51  重连定时器到点后重新 connectToHost
 *   87-88  send(Unknown) 被拒
 *   91-92  未连接时 send() 被拒
 *   124-131 socket 错误 -> errorOccurred + 主动安排重连
 *   138-139 安排重连（去重）
 *
 * 这几行全在 TCP 8887 这条**硬约束**路径上。互操作测试已经覆盖了正常收发，
 * 但"连不上时会不会退化成静默失败/疯狂重试"一直没人验证。
 *
 * 判据取向：断言落在**可观测输出**上 —— 信号是否发出、发了几次、字节有没
 * 有真的写出去。不断言"内部标志位变成 X"。
 */
#include <QtTest/QtTest>

#include <QSignalSpy>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

#include "core/net/signaling_client.h"

using accessterminal::SignalingClient;
using accessterminal::protocol::Signal;

// QSignalSpy 要把自定义枚举塞进 QVariant：Q_DECLARE_METATYPE 只保证
// value<T>() 能编译，跨队列投递还需要运行期 qRegisterMetaType（见第 29 条）。
Q_DECLARE_METATYPE(accessterminal::protocol::Signal)

namespace {
/// 找一个空闲端口：先监听再取端口，随后关闭。避免与其它测试抢固定端口。
quint16 freePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 p = probe.serverPort();
    probe.close();
    return p;
}

/**
 * 捕获 qWarning/qDebug 文本，用来断言**到底执行了哪条日志分支**。
 *
 * 为什么需要它（第 24 轮实测结论，务必先读）：
 * 对**未连接**的 QTcpSocket 调 write()，Qt 自己就返回 -1 并打印
 * `QIODevice::write (QTcpSocket): device not open`。因此
 *
 *     bool ok = client.send(Signal::Call);   // 未连接
 *     QVERIFY(!ok);
 *
 * **无论** send() 里有没有那段 `if (!isConnected()) return false;` 都会通过 ——
 * 一个是提前拦住，一个是放过去让 write() 失败，返回值一模一样。
 * 故障注入验证时这条确实"变异后仍通过"，属**真实的假绿**。
 *
 * 能区分两者的可观测差异是**哪条日志被打印**：有守卫就打印我们自己的
 * "未连接，发送失败"，没有守卫就只打印 Qt 的 device not open。
 * 于是把日志当可观测输出断言 —— 这正是"断言要落在代码的输出上"的应用。
 */
class MsgCapture
{
public:
    MsgCapture()
    {
        s_lines.clear();
        s_prev = qInstallMessageHandler(&MsgCapture::handler);
    }
    ~MsgCapture() { qInstallMessageHandler(s_prev); }

    static bool contains(const QString& needle)
    {
        for (int i = 0; i < s_lines.size(); ++i) {
            if (s_lines.at(i).contains(needle)) {
                return true;
            }
        }
        return false;
    }
    static QString joined() { return s_lines.join(QStringLiteral(" | ")); }

private:
    static void handler(QtMsgType, const QMessageLogContext&,
                        const QString& msg)
    {
        s_lines << msg;
    }
    static QStringList s_lines;
    static QtMessageHandler s_prev;
};

QStringList MsgCapture::s_lines;
QtMessageHandler MsgCapture::s_prev = nullptr;
} // namespace

class TestSignalingClient : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // ---- 发送失败路径（不需要连接）----
    void sendUnknownSignalIsRejected();
    void sendWhileNotConnectedIsRejected();
    void sendUnknownWhileConnectedIsRejectedAndWritesNothing();

    // ---- 正常连接与解码（补足失败路径的对照面）----
    void connectEmitsConnectedChangedExactlyOnce();
    void sendWritesExactWireBytes();
    void splitAndCombinedPacketsBothDecode();
    void stopWhileConnectedEmitsDisconnectedOnce();

    // ---- 错误与重连路径（本组的主要目标）----
    void connectionRefusedEmitsErrorOccurred();
    void refusedConnectionSchedulesReconnectThatActuallyRetries();
    void stopPreventsFurtherReconnectAttempts();
};

void TestSignalingClient::initTestCase()
{
    qRegisterMetaType<accessterminal::protocol::Signal>(
        "accessterminal::protocol::Signal");
}

// ---------------------------------------------------------------------------
// 发送失败：send(Unknown) 的帧是**空的**，绝不能把空字节写到 socket 上。
// 空帧若被写出去，对端解码器不会产生任何信令，却会污染字节流的边界。
// ---------------------------------------------------------------------------
void TestSignalingClient::sendUnknownSignalIsRejected()
{
    SignalingClient client;
    QSignalSpy errSpy(&client, &SignalingClient::errorOccurred);

    MsgCapture cap;
    QVERIFY(!client.send(Signal::Unknown));
    // 明确断言"什么都没发出去"：不是只看返回值。
    QCOMPARE(errSpy.count(), 0);   // 走的是 qWarning 分支，不发 errorOccurred
    // 且确实走了 send() 自己的空帧守卫 —— 见 MsgCapture 的说明：
    // 只看返回值无法区分"守卫拦住"与"放过去让底层失败"。
    QVERIFY2(MsgCapture::contains(QStringLiteral("尝试发送未知信令")),
             qPrintable(QStringLiteral("未走空帧守卫，实际日志: %1")
                        .arg(MsgCapture::joined())));
}

// ---------------------------------------------------------------------------
// 未连接时 send() 必须返回 false，而不是把字节塞进 socket 的写缓冲
// （那样会静默丢失，且用户以为发出去了）。
//
// 关键：断言必须落在**日志**上，不能只看返回值 —— 未连接的 QTcpSocket
// write() 本身就返回 -1，没有守卫也会"通过"（第 24 轮实测）。
// ---------------------------------------------------------------------------
void TestSignalingClient::sendWhileNotConnectedIsRejected()
{
    SignalingClient client;
    QVERIFY(!client.isConnected());

    MsgCapture cap;
    QVERIFY(!client.send(Signal::Call));
    QVERIFY2(MsgCapture::contains(QStringLiteral("未连接，发送失败")),
             qPrintable(QStringLiteral("未走未连接守卫，实际日志: %1")
                        .arg(MsgCapture::joined())));

    MsgCapture cap2;
    QVERIFY(!client.send(Signal::StopSend));
    QVERIFY(MsgCapture::contains(QStringLiteral("未连接，发送失败")));

    MsgCapture cap3;
    QVERIFY(!client.send(Signal::Unlock));
    QVERIFY(MsgCapture::contains(QStringLiteral("未连接，发送失败")));
}

// ---------------------------------------------------------------------------
// **已连接**时 send(Unknown) 必须返回 false，且对端一个字节都收不到。
//
// 为什么必须补这一条（第 24 轮故障注入发现的假绿）：
// "未连接时 send(Unknown) 返回 false"**无法**证明空帧守卫存在 ——
// 未连接的 socket write() 本来就返回 -1。把守卫删掉，测试照样通过。
//
// 已连接时就不同了：write(空) 返回 0，而 payload.size() 也是 0，
// 若没有空帧守卫，`written == payload.size()` 会让 send() **返回 true**
// （谎报成功），并且真的往 socket 上写了一次零长数据。两个后果都可观测。
// ---------------------------------------------------------------------------
void TestSignalingClient::sendUnknownWhileConnectedIsRejectedAndWritesNothing()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    SignalingClient client;
    client.start(QStringLiteral("127.0.0.1"), server.serverPort());
    QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
    QTcpSocket* peer = server.nextPendingConnection();
    QVERIFY(peer);
    QVERIFY(peer->waitForConnected(3000));

    MsgCapture cap;
    QVERIFY2(!client.send(Signal::Unknown),
             "已连接时 send(Unknown) 竟然返回 true —— "
             "空帧守卫缺失，会把零长数据当成功发出（谎报）");
    QVERIFY2(MsgCapture::contains(QStringLiteral("尝试发送未知信令")),
             qPrintable(QStringLiteral("未走空帧守卫，实际日志: %1")
                        .arg(MsgCapture::joined())));

    // 对端必须收不到任何字节
    peer->waitForReadyRead(300);
    QCOMPARE(peer->readAll(), QByteArray());

    // 对照：有效信令仍必须正常写出，证明上面的"零字节"不是因为连接坏了
    QVERIFY(client.send(Signal::Call));
    QVERIFY(peer->waitForReadyRead(3000));
    QCOMPARE(peer->readAll(), QByteArray("call\n"));

    delete peer;
}

// ---------------------------------------------------------------------------
// connectedChanged 只在**状态真正翻转**时发一次。
// 重复发会让 UI 反复播放"已连接"提示音。
//
// 【已知局限：m_wasConnected 守卫在本用例下不可区分，第 24 轮实测】
// 探针（tests/probe_semantics.cpp 场景 C）显示：已连接时再调 start()，
// Qt 会**拒绝**这次 connectToHost 并先发一个 socket error
// "Trying to connect while connection is in progress"，根本走不到第二次
// onConnected。同一次连接内 connected 只触发一次。
// 于是把 `if (!m_wasConnected)` 去掉，本用例**仍然通过** —— 这是真实的
// 不可区分，不是断言写弱了。Qt 里构造不出"同一次连接内 connected 二次触发"
// 的场景，故如实记录局限，不写一条永远为真的假断言（第 21 轮同一处理）。
// 本用例仍有价值：它钉住"一次连接只发一次 true"，防止将来改动引入重复派发。
// ---------------------------------------------------------------------------
void TestSignalingClient::connectEmitsConnectedChangedExactlyOnce()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    SignalingClient client;
    QSignalSpy connSpy(&client, &SignalingClient::connectedChanged);

    client.start(QStringLiteral("127.0.0.1"), server.serverPort());
    QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 3000);
    QCOMPARE(connSpy.count(), 1);
    QCOMPARE(connSpy.at(0).at(0).toBool(), true);
}

// ---------------------------------------------------------------------------
// 发出去的字节必须逐字节正确：协议是硬约束，任何多余/缺失的 '\n' 都会
// 让对端解码失败。
// ---------------------------------------------------------------------------
void TestSignalingClient::sendWritesExactWireBytes()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    SignalingClient client;
    client.start(QStringLiteral("127.0.0.1"), server.serverPort());
    // 必须先等**客户端自己**进入 Connected，再 send()。
    // 只等 server.hasPendingConnections() 不够：服务端已经 accept，但客户端
    // 侧的 QTcpSocket 可能还停在 ConnectingState，此时 send() 会走
    // "未连接，发送失败" 分支并返回 false（本测试首版就是这么失败的）。
    QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
    QTcpSocket* peer = server.nextPendingConnection();
    QVERIFY(peer);
    QVERIFY(peer->waitForConnected(3000));

    QVERIFY(client.send(Signal::Call));
    QVERIFY(peer->waitForReadyRead(3000));
    QCOMPARE(peer->readAll(), QByteArray("call\n"));

    QVERIFY(client.send(Signal::Unlock));
    QVERIFY(peer->waitForReadyRead(3000));
    QCOMPARE(peer->readAll(), QByteArray("unlock\n"));

    delete peer;
}

// ---------------------------------------------------------------------------
// TCP 是字节流：半包与多包都必须正确解码。旧版会静默丢弃半包，
// 这里两边都钉住。
// ---------------------------------------------------------------------------
void TestSignalingClient::splitAndCombinedPacketsBothDecode()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    SignalingClient client;
    QSignalSpy sigSpy(&client, &SignalingClient::signalReceived);

    client.start(QStringLiteral("127.0.0.1"), server.serverPort());
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
    QTcpSocket* peer = server.nextPendingConnection();
    QVERIFY(peer);
    QVERIFY(peer->waitForConnected(3000));

    // 半包：先发 "cal"，此时不应解出任何信令
    peer->write("cal");
    peer->flush();
    QTest::qWait(200);
    QCOMPARE(sigSpy.count(), 0);

    // 补齐后应解出 Call
    peer->write("l\n");
    peer->flush();
    QTRY_COMPARE_WITH_TIMEOUT(sigSpy.count(), 1, 3000);
    QCOMPARE(sigSpy.at(0).at(0).value<Signal>(), Signal::Call);

    // 一个包里塞两条：必须解出两条，且顺序正确
    peer->write("stopsend\nunlock\n");
    peer->flush();
    QTRY_COMPARE_WITH_TIMEOUT(sigSpy.count(), 3, 3000);
    QCOMPARE(sigSpy.at(1).at(0).value<Signal>(), Signal::StopSend);
    QCOMPARE(sigSpy.at(2).at(0).value<Signal>(), Signal::Unlock);

    delete peer;
}

// ---------------------------------------------------------------------------
// stop() 断开连接时应发出 connectedChanged(false) —— 且只发一次。
// ---------------------------------------------------------------------------
void TestSignalingClient::stopWhileConnectedEmitsDisconnectedOnce()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    SignalingClient client;
    QSignalSpy connSpy(&client, &SignalingClient::connectedChanged);

    client.start(QStringLiteral("127.0.0.1"), server.serverPort());
    QTRY_VERIFY_WITH_TIMEOUT(client.isConnected(), 3000);

    client.stop();
    QTRY_COMPARE_WITH_TIMEOUT(connSpy.count(), 2, 3000);
    QCOMPARE(connSpy.at(0).at(0).toBool(), true);
    QCOMPARE(connSpy.at(1).at(0).toBool(), false);
}

// ---------------------------------------------------------------------------
// 连接被拒：必须发出 errorOccurred（供日志/UI），且不崩、不阻塞。
// 用户在那头没开机时，这是最常见的路径。
// ---------------------------------------------------------------------------
void TestSignalingClient::connectionRefusedEmitsErrorOccurred()
{
    const quint16 dead = freePort();
    QVERIFY(dead != 0);

    SignalingClient client;
    QSignalSpy errSpy(&client, &SignalingClient::errorOccurred);

    client.start(QStringLiteral("127.0.0.1"), dead);
    QTRY_VERIFY_WITH_TIMEOUT(errSpy.count() >= 1, 5000);
    QVERIFY(!client.isConnected());
    // 错误消息不能为空：空消息等于把故障藏起来。
    QVERIFY(!errSpy.at(0).at(0).toString().isEmpty());
}

/*
 * ---------------------------------------------------------------------------
 * 关于本组"重连"用例的时序事实（第 24 轮实测，务必先读再改）
 *
 * 本机（Windows / Qt 5.15）上向 127.0.0.1 一个无人监听的端口 connectToHost，
 * 失败并非立刻返回：`Connection refused` 的 socket 错误约在 **4.1 秒**后才到达。
 * 因此一轮"重连"的真实周期是：
 *
 *      3 秒（重连定时器） + 约 4.1 秒（这次连接尝试失败） ≈ 7.1 秒
 *
 * 实测时间线（probe_reconnect.cpp，30 秒窗口）：
 *      t≈4.1s / 11.2s / 18.2s / 25.3s   各一次 errorOccurred
 *
 * 教训：绝对不要用"3 秒后应该出现第二次错误"这类**基于设计常量的固定等待**。
 * 首版就是那么写的，结果断言失败 —— 看上去像"重连只安排未执行"的**真实缺陷**，
 * 实际是测试的等待窗口短于一次真实往返。差一步就把正常代码误判成 bug。
 * 正确做法：用 QTRY_* 等待**属性**（错误计数增加），给它足够宽的上界。
 * ---------------------------------------------------------------------------
 */

// ---------------------------------------------------------------------------
// 被拒之后必须**真的**按间隔重试，而不是安排一次就再也不动。
// 这条覆盖重连定时器的回调（signaling_client.cpp:47-51）。
//
// 判据：等待出现**第二次** errorOccurred。若只安排不执行，计数会永远停在 1。
// 上界给到 20 秒（一轮实测约 7.1 秒，留足余量）。
// ---------------------------------------------------------------------------
void TestSignalingClient::refusedConnectionSchedulesReconnectThatActuallyRetries()
{
    const quint16 dead = freePort();
    QVERIFY(dead != 0);

    SignalingClient client;
    QSignalSpy errSpy(&client, &SignalingClient::errorOccurred);

    client.start(QStringLiteral("127.0.0.1"), dead);
    QTRY_VERIFY_WITH_TIMEOUT(errSpy.count() >= 1, 15000);

    const int afterFirst = errSpy.count();
    // 等待"又发生了一次"——等属性，不等固定时长。
    QTRY_VERIFY_WITH_TIMEOUT(errSpy.count() > afterFirst, 20000);
}

// ---------------------------------------------------------------------------
// stop() 之后不得再重试。这是"用户主动挂断后程序还在后台反复拨号"的
// 典型缺陷面：不安全，也会刷爆日志。
//
// 场景刻意选**"还在连接中就取消"**（start() 后立刻 stop()），而不是
// "等第一次错误出现后再 stop()"。理由（第 24 轮实测）：
//   后者的 stop() 会顺手把已经启动的重连定时器 stop() 掉，于是
//   `if (!m_running) return;` 这道守卫**根本走不到**，故障注入验证里
//   把守卫删掉测试仍然通过（假绿）。
//   前者则在 socket 处于 ConnectingState 时取消：连接失败的错误稍后才到，
//   此时 m_running 已为 false，scheduleReconnect() 的守卫成为**唯一**的
//   拦截点 —— 删掉它就会出现后续重试，测试才能真正区分。
//
// 这条是**否定断言**，必须给足"本该发生"的时间：一轮实测约 7.1 秒，
// 故等 20 秒（>2 轮）。
//
// 【重复守卫，务必知道】`m_running` 在**两处**独立守卫重连：
//   (1) scheduleReconnect() 开头       `if (!m_running) return;`
//   (2) 重连定时器回调内               `if (m_running) { ...connectToHost... }`
// 因此**单独**删掉任意一处，行为都不变 —— 故障注入验证里"只删一处"会显示
// 仍然通过，那不是假绿，而是守卫确实冗余。要证明本用例有效，必须**同时**
// 删掉两处（等价于"移除重连功能"）才会失败。
// ---------------------------------------------------------------------------
void TestSignalingClient::stopPreventsFurtherReconnectAttempts()
{
    const quint16 dead = freePort();
    QVERIFY(dead != 0);

    SignalingClient client;
    QSignalSpy errSpy(&client, &SignalingClient::errorOccurred);

    client.start(QStringLiteral("127.0.0.1"), dead);
    // 故意**不等**任何错误：仍在连接中（ConnectingState）就取消。
    client.stop();

    // 第一次错误（"Connection refused"）是**连接尝试本身**失败产生的，
    // 与重连无关：即使 m_running 已为假、socket 已 disconnectFromHost，
    // 这次尝试仍会以错误收场（第 24 轮探针实测为 t≈8.6s）。
    // 因此不能断言"零错误"—— 那会让正确代码也失败。
    // 先等出这次不可避免的错误、记为基线，再看之后是否还会新增。
    QTRY_VERIFY_WITH_TIMEOUT(errSpy.count() >= 1, 25000);
    const int baseline = errSpy.count();

    // 此后跨过 2 个完整重连周期（实测一轮约 7.1 秒）。
    // m_running 守卫缺失 -> 这里必然新增；守卫在 -> 纹丝不动。
    QTest::qWait(20000);
    QVERIFY2(errSpy.count() == baseline,
             qPrintable(QStringLiteral("stop() 之后仍在重连：基线 %1 次，"
                                       "20 秒后变成 %2 次")
                        .arg(baseline).arg(errSpy.count())));
}

QTEST_MAIN(TestSignalingClient)
#include "test_signaling_client.moc"
