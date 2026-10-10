/*
 * test_interop.cpp — 跨工程信令互通测试（真实 TCP 回环）
 *
 * 目的：这是对“协议逐字节兼容”这一硬约束最强的验证。
 *       直接链接两个仓库里的**真实**实现：
 *         - Accessterminal_desktop::SignalingServer （室内机侧）
 *         - Accessterminal_T113::SignalingClient     （门口机侧）
 *       让它们在 127.0.0.1 上做真实的 TCP 收发，逐条校验信令。
 *
 * 与 tests/test_protocol.cpp 的区别：
 *       后者只验证 protocol.h 的常量与 toWire/fromWire 的一致性；
 *       本测试验证两端**实际代码路径**能否互通——包括分包（'\n' 切分）、
 *       粘包处理、Unknown 容错、双向时序。
 *
 * 两个仓库的类恰好同名（accessterminal::SignalingServer / SignalingClient），
 * 同名但不同仓库，故用不同命名空间宏包裹后分别引入。
 */
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QtNetwork/QTcpServer>

// 两个仓库的 protocol.h 内容逐字节一致，重复包含由 include guard 挡住即可，
// 但这里刻意让各自 TU 独立包含，以验证“两份 protocol.h 确实可互换”。
#include "core/net/protocol.h"

// 分别来自两个仓库的真实实现：
//   signaling_server.h 只存在于 Accessterminal_desktop
//   signaling_client.h 只存在于 Accessterminal_T113
#include "core/net/signaling_client.h"
#include "core/net/signaling_server.h"

using accessterminal::protocol::Signal;

// QSignalSpy 需要把自定义枚举放进 QVariant，因此必须声明元类型。
// 生产代码里由 widget.h 声明（T113 侧），测试中单独声明以免引入 UI 依赖。
Q_DECLARE_METATYPE(accessterminal::protocol::Signal)

class TestInterop : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // 核心：真实 TCP 回环下的逐条信令互通
    void serverReceivesCall();
    void serverReceivesStopSend();
    void serverReceivesUnlock();
    void clientReceivesAccept();
    void clientReceivesRecStop();
    void clientReceivesUnlock();

    // 边界：服务端解码器（用裸 socket 精确控制字节切分）
    void unknownSignalIsIgnored();
    void multipleSignalsInOnePacket();
    void partialPacketIsBuffered();
    void stopsendSplitIsNotMisreadAsStop();

    // 边界：客户端解码器（用服务端 socket 反向注入字节）
    void legacySignalWithoutNewline();
    void clientBuffersPartialPacket();
};

// 待测对象由外部（main 之前）创建；这里用全局指针，便于跨用例复用。
static accessterminal::SignalingServer* g_server = nullptr;
static accessterminal::SignalingClient* g_client = nullptr;
static quint16 g_port = 0;

/**
 * @brief 建立一条**独立的**裸 TCP 连接，用于向服务端精确注入字节。
 *
 * 为什么不用 g_client：这些测试要控制的是“服务端收到了什么字节、怎么
 * 切分”，需要自己决定每次 write 的内容与时机。走 g_client 会经过它自身
 * 的发送与解码逻辑，无法精确构造半包，也会把两个解码器的行为混在一起。
 *
 * 用 clientConnected 信号（而不是 clientCount 数值）确认服务端已接入：
 * 上一个用例断开连接后，服务端的 clientDisconnected 可能尚未被事件循环
 * 处理，此时读数会偏高，比较数值就会误判。
 *
 * @return 已连通的 socket；失败返回 nullptr（调用方负责 delete）。
 */
static QTcpSocket* makeRawPeer(int timeoutMs = 3000)
{
    QSignalSpy accepted(g_server,
                        &accessterminal::SignalingServer::clientConnected);
    QTcpSocket* raw = new QTcpSocket();
    raw->connectToHost(QHostAddress::LocalHost, g_port);
    if (!raw->waitForConnected(timeoutMs)) {
        delete raw;
        return nullptr;
    }
    // 等待服务端侧完成接入（信号在建立 spy 之后才可能发出）。
    QElapsedTimer t;
    t.start();
    while (accepted.isEmpty() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    if (accepted.isEmpty()) {
        delete raw;
        return nullptr;
    }
    return raw;
}

/// 断开并等待服务端处理完断开事件，避免影响下一个用例。
static void closeRawPeer(QTcpSocket* raw)
{
    if (!raw) {
        return;
    }
    raw->disconnectFromHost();
    if (raw->state() != QAbstractSocket::UnconnectedState) {
        raw->waitForDisconnected(2000);
    }
    delete raw;
    // 让服务端的 clientDisconnected 得到处理
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 200) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

void TestInterop::initTestCase()
{
    // 注册元类型，使 QSignalSpy 能正确取回 protocol::Signal。
    qRegisterMetaType<accessterminal::protocol::Signal>(
        "accessterminal::protocol::Signal");
    // SignalingServer::signalReceived 的第二个参数是 QTcpSocket*，
    // 不注册的话 QSignalSpy 取不到该参数，无法拿回执用的连接对象。
    qRegisterMetaType<QTcpSocket*>("QTcpSocket*");

    // 用一个空闲端口，避免与真实设备占用的 8887 冲突。
    QTcpServer probe;
    QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
    g_port = probe.serverPort();
    probe.close();

    g_server = new accessterminal::SignalingServer(this);
    QVERIFY2(g_server->listen(g_port),
             qPrintable(QString("服务端监听 %1 失败").arg(g_port)));

    g_client = new accessterminal::SignalingClient(this);
    g_client->start(QStringLiteral("127.0.0.1"), g_port);

    // 等待真实连接建立
    QTRY_VERIFY_WITH_TIMEOUT(g_server->clientCount() == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(g_client->isConnected(), 5000);
}

void TestInterop::cleanupTestCase()
{
    if (g_client) { g_client->stop(); }
    if (g_server) { g_server->stop(); }
}

// ---- 门口机 → 室内机 ----

void TestInterop::serverReceivesCall()
{
    QSignalSpy spy(g_server, &accessterminal::SignalingServer::signalReceived);
    QVERIFY(g_client->send(Signal::Call));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(spy.last().at(0).value<Signal>(), Signal::Call);
}

void TestInterop::serverReceivesStopSend()
{
    QSignalSpy spy(g_server, &accessterminal::SignalingServer::signalReceived);
    QVERIFY(g_client->send(Signal::StopSend));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    // 这条断言同时守卫了“重构中曾丢失 stopsend 发送”这一回归。
    QCOMPARE(spy.last().at(0).value<Signal>(), Signal::StopSend);
}

void TestInterop::serverReceivesUnlock()
{
    QSignalSpy spy(g_server, &accessterminal::SignalingServer::signalReceived);
    QVERIFY(g_client->send(Signal::Unlock));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(spy.last().at(0).value<Signal>(), Signal::Unlock);
}

// ---- 室内机 → 门口机 ----

void TestInterop::clientReceivesAccept()
{
    QSignalSpy spy(g_client, &accessterminal::SignalingClient::signalReceived);
    // 取服务端侧的连接对象来回执
    QTcpSocket* peer = nullptr;
    QSignalSpy connSpy(g_server, &accessterminal::SignalingServer::signalReceived);
    QVERIFY(g_client->send(Signal::Call));
    QTRY_VERIFY_WITH_TIMEOUT(connSpy.count() >= 1, 3000);
    peer = connSpy.last().at(1).value<QTcpSocket*>();
    QVERIFY(peer);

    QVERIFY(g_server->reply(peer, Signal::Accept));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(spy.last().at(0).value<Signal>(), Signal::Accept);
}

void TestInterop::clientReceivesRecStop()
{
    QSignalSpy spy(g_client, &accessterminal::SignalingClient::signalReceived);
    QSignalSpy connSpy(g_server, &accessterminal::SignalingServer::signalReceived);
    QVERIFY(g_client->send(Signal::Call));
    QTRY_VERIFY_WITH_TIMEOUT(connSpy.count() >= 1, 3000);
    QTcpSocket* peer = connSpy.last().at(1).value<QTcpSocket*>();
    QVERIFY(peer);

    QVERIFY(g_server->reply(peer, Signal::RecStop));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(spy.last().at(0).value<Signal>(), Signal::RecStop);
}

void TestInterop::clientReceivesUnlock()
{
    QSignalSpy spy(g_client, &accessterminal::SignalingClient::signalReceived);
    QSignalSpy connSpy(g_server, &accessterminal::SignalingServer::signalReceived);
    QVERIFY(g_client->send(Signal::Call));
    QTRY_VERIFY_WITH_TIMEOUT(connSpy.count() >= 1, 3000);
    QTcpSocket* peer = connSpy.last().at(1).value<QTcpSocket*>();
    QVERIFY(peer);

    QVERIFY(g_server->reply(peer, Signal::Unlock));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(spy.last().at(0).value<Signal>(), Signal::Unlock);
}

// ---- 边界情况 ----

void TestInterop::unknownSignalIsIgnored()
{
    // 向服务端注入一条无法识别的行：应被忽略，且连接保持存活。
    QSignalSpy connSpy(g_server, &accessterminal::SignalingServer::signalReceived);
    QTcpSocket* raw = makeRawPeer();
    QVERIFY(raw);

    // 排空建连过程中可能产生的信令
    connSpy.clear();

    raw->write("this-is-not-a-signal\n");
    raw->flush();

    QTest::qWait(300);
    QCOMPARE(connSpy.count(), 0);                       // 不产生信令
    QCOMPARE(raw->state(), QAbstractSocket::ConnectedState); // 不断开

    closeRawPeer(raw);
}

void TestInterop::multipleSignalsInOnePacket()
{
    // 一次 write 塞三条信令（粘包），服务端必须逐条切分。
    QSignalSpy spy(g_server, &accessterminal::SignalingServer::signalReceived);
    QTcpSocket* raw = makeRawPeer();
    QVERIFY(raw);
    spy.clear();

    raw->write("call\nunlock\nstopsend\n");
    raw->flush();

    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 3, 3000);
    QCOMPARE(spy.at(spy.count() - 3).at(0).value<Signal>(), Signal::Call);
    QCOMPARE(spy.at(spy.count() - 2).at(0).value<Signal>(), Signal::Unlock);
    QCOMPARE(spy.at(spy.count() - 1).at(0).value<Signal>(), Signal::StopSend);

    closeRawPeer(raw);
}

void TestInterop::partialPacketIsBuffered()
{
    // 半包："unl" 单独到达时不应产生任何信令；补上 "ock\n" 后应还原 unlock。
    QSignalSpy spy(g_server, &accessterminal::SignalingServer::signalReceived);
    QTcpSocket* raw = makeRawPeer();
    QVERIFY(raw);
    spy.clear();

    raw->write("unl");
    raw->flush();
    QTest::qWait(200);
    QCOMPARE(spy.count(), 0);          // 半包不得被当成信令

    raw->write("ock\n");
    raw->flush();
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(spy.last().at(0).value<Signal>(), Signal::Unlock);

    closeRawPeer(raw);
}

void TestInterop::stopsendSplitIsNotMisreadAsStop()
{
    // 歧义消除："stop" 是 "stopsend" 的前缀。
    // 若把 "stopsend\n" 拆成 "stop" + "send\n"，仅凭 "stop" 就判定会得到
    // 错误的 Stop——比丢包更危险，会触发错误行为（停掉监看推流而非通知
    // 对端停止发送）。必须等到能区分两者时才产生信令。
    QSignalSpy spy(g_server, &accessterminal::SignalingServer::signalReceived);
    QTcpSocket* raw = makeRawPeer();
    QVERIFY(raw);
    spy.clear();

    raw->write("stop");
    raw->flush();
    QTest::qWait(200);
    QCOMPARE(spy.count(), 0);          // 关键：此刻绝不能已产生 Stop

    raw->write("send\n");
    raw->flush();
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(spy.last().at(0).value<Signal>(), Signal::StopSend);

    closeRawPeer(raw);
}

void TestInterop::legacySignalWithoutNewline()
{
    // 兼容性回归：历史版本的室内机发出的 "recstop" **没有** '\n'
    // （见 baseline desktop/smartserver/widget.cpp:566）。
    // 旧版门口机用 split('\n', SkipEmptyParts) 解析，末尾无 '\n' 的片段
    // 也会被当作完整信令，因此能正常挂断。
    // 新版解码器必须保持同样的宽容语义，否则“新门口机 + 旧室内机”的组合
    // 会永远收不到 recstop。
    //
    // 这里注入到**客户端**侧：复用 g_server 侧连接反向写字节。
    QSignalSpy connSpy(g_server, &accessterminal::SignalingServer::signalReceived);
    QVERIFY(g_client->send(Signal::Call));
    QTRY_VERIFY_WITH_TIMEOUT(connSpy.count() >= 1, 3000);
    QTcpSocket* serverSide = connSpy.last().at(1).value<QTcpSocket*>();
    QVERIFY(serverSide);

    QSignalSpy spy(g_client, &accessterminal::SignalingClient::signalReceived);
    spy.clear();
    serverSide->write("recstop");      // 刻意不带 '\n'，模拟旧版
    serverSide->flush();

    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(spy.last().at(0).value<Signal>(), Signal::RecStop);
}

void TestInterop::clientBuffersPartialPacket()
{
    // 客户端侧的半包重组：注入 "unl"，确认未产生信令；补 "ock\n" 后还原。
    QSignalSpy connSpy(g_server, &accessterminal::SignalingServer::signalReceived);
    QVERIFY(g_client->send(Signal::Call));
    QTRY_VERIFY_WITH_TIMEOUT(connSpy.count() >= 1, 3000);
    QTcpSocket* serverSide = connSpy.last().at(1).value<QTcpSocket*>();
    QVERIFY(serverSide);

    QSignalSpy spy(g_client, &accessterminal::SignalingClient::signalReceived);
    spy.clear();
    serverSide->write("unl");
    serverSide->flush();
    QTest::qWait(200);
    QCOMPARE(spy.count(), 0);

    serverSide->write("ock\n");
    serverSide->flush();
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 3000);
    QCOMPARE(spy.last().at(0).value<Signal>(), Signal::Unlock);
}

QTEST_MAIN(TestInterop)
#include "test_interop.moc"
