/**
 * @file test_audio_sender.cpp
 * @brief AudioSenderThread（UDP 12345 上行音频）的单元测试。
 *
 * 为什么必须测：这条通路的**字节格式是协议硬约束** —— 裸 PCM、S16_LE、
 * 16000 Hz、单声道、每包 320 帧（640 字节）、**无包头**。室内机按包直接
 * 送进 ALSA，所以包长或字节序错一点，现场只表现为"声音不对/有杂音"，
 * 不会报任何错误。
 *
 * 此前它从未被真正验证过：tools/devstubs 的 sendto() 只是"返回 len"的桩，
 * 既不记录载荷，其 inet_pton() 还**永远返回 0**，导致 run() 在建连阶段就
 * return，根本走不到发送路径。本测试改用 tests/fake_net.cpp（可记录载荷）
 * 与 tests/fake_alsa.cpp（可注入采集错误），把整条通路跑起来并对**线上字节**
 * 做断言。
 *
 * 同时回归第 11 轮缺陷：`connectionLost` 曾经只被声明、从未发射，使"采集
 * 通道失效立即挂断"这条兜底路径实际是死的。
 */
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>

#include <cstring>

#include <sys/socket.h>
#include <arpa/inet.h>

#include "fake_alsa.h"
#include "fake_net.h"

// 需要读取私有成员做断言（sockfd/serverIp/serverPort），并直接调用 run()。
// 关键：必须先把 audio_sender_thread.h 依赖的**所有**系统/Qt 头文件包含完，
// 否则 `private -> public` 会泄漏进 <atomic>、<alsa/asoundlib.h> 等库头，
// 导致难以理解的编译错误。这些头都有 include guard，后续重复包含无副作用。
#include <QThread>
#include <QString>
#include <atomic>
#include <alsa/asoundlib.h>

#define private public
#include "core/media/audio_sender_thread.h"
#undef private

#include "core/net/protocol.h"

namespace {

const char* kTestIp = "192.168.1.4";

/**
 * 启动线程，一直等到"至少发出 n 个包"或超时，然后停止。
 *
 * 不用固定 qWait()：线程启动 + snd_pcm_open + （EAGAIN 路径的休眠）叠加起来
 * 有几十毫秒抖动，固定等待会写出偶发失败的测试。这里等的是**性质**本身。
 */
bool waitForPackets(AudioSenderThread& t, int n, int timeoutMs)
{
    t.start();
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (fakenet::sendtoCount() >= n) {
            return true;
        }
        QTest::qWait(5);
    }
    return fakenet::sendtoCount() >= n;
}

void stopAndJoin(AudioSenderThread& t)
{
    t.stop();
    t.wait(4000);
}

} // namespace

class TestAudioSender : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void uplinkPacketsMatchProtocolExactly();
    void packetSizeIsFrozenConstant();
    void sendsToConfiguredAddressAndPort();
    void noPacketHasAHeader();
    void underrunRecoversWithoutLosingAudio();
    void eagainDoesNotAbortCapture();
    void fatalReadErrorEmitsConnectionLost();
    void openFailureEmitsConnectionLost();
    void badIpEmitsConnectionLost();
    void sendFailureEmitsConnectionLost();
    void finishedSignalAlwaysFires();
    void stopEndsThreadPromptly();
    void cleanupTestCase();
};

void TestAudioSender::init()
{
    fakealsa::reset();
    fakenet::reset();
}

void TestAudioSender::cleanupTestCase()
{
    fakealsa::reset();
    fakenet::reset();
}

/**
 * 核心协议契约：每个数据报都是 640 字节的裸 PCM，无包头、无长度字段。
 *
 * 640 = kAudioFramesPerPacket(320) * kAudioChannels(1) * sizeof(int16_t)(2)。
 * 若将来有人改成"加个头"或改了每包帧数，本用例会失败——这正是要防的。
 */
void TestAudioSender::uplinkPacketsMatchProtocolExactly()
{
    AudioSenderThread t(/*sockfd=*/7, QString::fromLatin1(kTestIp),
                        accessterminal::protocol::kAudioPort);

    QVERIFY2(waitForPackets(t, 3, 3000), "应当至少发送了若干音频包");
    stopAndJoin(t);

    const std::vector<fakenet::Datagram> sent = fakenet::sent();
    QVERIFY2(!sent.empty(), "应当至少发送了一个音频包");

    const int expectedBytes = accessterminal::protocol::kAudioFramesPerPacket *
                              accessterminal::protocol::kAudioChannels *
                              static_cast<int>(sizeof(int16_t));
    QCOMPARE(expectedBytes, 640); // 640 字节/包，协议冻结值

    for (size_t i = 0; i < sent.size(); ++i) {
        QVERIFY2(static_cast<int>(sent[i].payload.size()) == expectedBytes,
                 "每个音频包都必须是 640 字节的裸 PCM（无包头）");
    }
}

/// 每包帧数是协议冻结常量，不能被悄悄改动。
void TestAudioSender::packetSizeIsFrozenConstant()
{
    QCOMPARE(accessterminal::protocol::kAudioSampleRate, 16000);
    QCOMPARE(accessterminal::protocol::kAudioChannels, 1);
    QCOMPARE(accessterminal::protocol::kAudioFramesPerPacket, 320);
    QCOMPARE(accessterminal::protocol::kAudioPort, static_cast<quint16>(12345));

    // ALSA 侧也必须是 S16_LE / 16000 / 单声道（与线上格式一致）
    AudioSenderThread t(7, QString::fromLatin1(kTestIp),
                        accessterminal::protocol::kAudioPort);
    // 等到参数被真正设置好（open + set_* 在 run() 开头完成）
    QVERIFY(waitForPackets(t, 1, 3000));
    stopAndJoin(t);
    QCOMPARE(fakealsa::lastFormat(), 2); // SND_PCM_FORMAT_S16_LE
    QCOMPARE(fakealsa::lastRate(), 16000);
    QCOMPARE(fakealsa::lastChannels(), 1);
}

/// 必须发到 AppConfig/参数指定的门口机（室内机）地址与 12345 端口。
void TestAudioSender::sendsToConfiguredAddressAndPort()
{
    const quint16 port = accessterminal::protocol::kAudioPort;
    AudioSenderThread t(9, QString::fromLatin1(kTestIp), port);

    QVERIFY(waitForPackets(t, 2, 3000));
    stopAndJoin(t);

    const std::vector<fakenet::Datagram> sent = fakenet::sent();
    QVERIFY(!sent.empty());
    for (size_t i = 0; i < sent.size(); ++i) {
        QCOMPARE(sent[i].fd, 9);            // 用的是外部传入的 fd，不自建
        QCOMPARE(sent[i].destPort, port);   // 12345
        // 192.168.1.4 的网络序
        const unsigned int expect = (192u << 24) | (168u << 16) | (1u << 8) | 4u;
        QCOMPARE(sent[i].destAddr, htonl(expect));
    }
}

/**
 * 明确断言"没有包头"：载荷首字节必须与 fake ALSA 生成的 PCM 数据一致
 * （填充值为 递增序号 + i%7），而不是任何长度/魔数前缀。
 */
void TestAudioSender::noPacketHasAHeader()
{
    fakealsa::setReadFill(0);
    AudioSenderThread t(7, QString::fromLatin1(kTestIp),
                        accessterminal::protocol::kAudioPort);
    QVERIFY(waitForPackets(t, 1, 3000));
    stopAndJoin(t);

    const std::vector<fakenet::Datagram> sent = fakenet::sent();
    QVERIFY(!sent.empty());

    // 第 k 个成功读取用填充值 k。第一个包首字节应为 0+0=0，
    // 若实现塞了包头，这里就会是别的值。
    const unsigned char first = static_cast<unsigned char>(sent[0].payload[0]);
    QVERIFY2(first < 8, "载荷首字节必须是 PCM 数据本身，不能是包头/魔数");

    // 包内数据也应符合填充规律
    for (int i = 0; i < 16; ++i) {
        QCOMPARE(static_cast<unsigned char>(sent[0].payload[i]),
                 static_cast<unsigned char>(i % 7));
    }
}

/**
 * 第 11 轮同类问题的回归：采集 underrun(-EPIPE) 必须 prepare 后继续，
 * 不能中断采集、更不能丢掉后续音频。
 */
void TestAudioSender::underrunRecoversWithoutLosingAudio()
{
    fakealsa::setReadEpipeFirst(2);
    AudioSenderThread t(7, QString::fromLatin1(kTestIp),
                        accessterminal::protocol::kAudioPort);

    QSignalSpy lost(&t, SIGNAL(connectionLost()));

    QVERIFY2(waitForPackets(t, 1, 3000), "underrun 之后仍必须继续发送音频");
    stopAndJoin(t);

    QCOMPARE(lost.count(), 0);                    // underrun 不是"连接丢失"
    QVERIFY(fakealsa::prepareCount() >= 2);       // 每次都 prepare
    QVERIFY(!fakenet::sent().empty());
}

/// -EAGAIN 是"暂时没数据"，必须重试而不是结束线程。
void TestAudioSender::eagainDoesNotAbortCapture()
{
    fakealsa::setReadEagainFirst(3);
    AudioSenderThread t(7, QString::fromLatin1(kTestIp),
                        accessterminal::protocol::kAudioPort);

    QSignalSpy lost(&t, SIGNAL(connectionLost()));

    // 等到确实发出至少一个包，而不是固定睡 80ms（EAGAIN 路径会额外休眠，
    // 固定等待会偶发失败）。
    QVERIFY2(waitForPackets(t, 1, 3000), "EAGAIN 之后仍必须继续采集并发送");
    stopAndJoin(t);

    QCOMPARE(lost.count(), 0);
    QVERIFY(!fakenet::sent().empty());
}

/**
 * 核心回归（第 11 轮）：不可恢复的采集错误必须发射 connectionLost，
 * 否则界面会一直停在"通话中"而实际已无上行音频。
 */
void TestAudioSender::fatalReadErrorEmitsConnectionLost()
{
    fakealsa::setReadFatalAfter(1); // 第 2 次 readi 起返回 -EIO
    AudioSenderThread t(7, QString::fromLatin1(kTestIp),
                        accessterminal::protocol::kAudioPort);

    QSignalSpy lost(&t, SIGNAL(connectionLost()));
    QSignalSpy err(&t, SIGNAL(errorMsg(QString)));

    t.start();
    QVERIFY2(t.wait(4000), "致命读错误后线程应尽快结束");

    QVERIFY2(err.count() >= 1, "应报告错误信息");
    QCOMPARE(lost.count(), 1); // **关键**：这正是重构前从未发射的信号
}

/// 打不开采集设备 -> 必须 connectionLost（否则通话单向静音而界面无知觉）。
void TestAudioSender::openFailureEmitsConnectionLost()
{
    fakealsa::setOpenFail(true);
    AudioSenderThread t(7, QString::fromLatin1(kTestIp),
                        accessterminal::protocol::kAudioPort);

    QSignalSpy lost(&t, SIGNAL(connectionLost()));
    t.start();
    QVERIFY2(t.wait(4000), "打开失败后线程应立即结束");

    QCOMPARE(lost.count(), 1);
    QCOMPARE(fakenet::sendtoCount(), 0); // 没有设备就不该发任何包
}

/// 非法服务器 IP -> inet_pton 失败 -> connectionLost。
void TestAudioSender::badIpEmitsConnectionLost()
{
    AudioSenderThread t(7, QStringLiteral("not-an-ip"),
                        accessterminal::protocol::kAudioPort);

    QSignalSpy lost(&t, SIGNAL(connectionLost()));
    t.start();
    QVERIFY2(t.wait(4000), "IP 非法时应立即结束");

    QCOMPARE(lost.count(), 1);
    QCOMPARE(fakenet::sendtoCount(), 0);
}

/**
 * 核心回归（第 11 轮）：sendto 失败必须 connectionLost。
 *
 * sockfd 是发送用的 UDP socket，sendto 失败不会有任何其它可观测迹象，
 * 界面依旧显示"通话中"。这是重构前真正悬空的兜底路径。
 */
void TestAudioSender::sendFailureEmitsConnectionLost()
{
    fakenet::setSendFail(true);
    AudioSenderThread t(7, QString::fromLatin1(kTestIp),
                        accessterminal::protocol::kAudioPort);

    QSignalSpy lost(&t, SIGNAL(connectionLost()));
    QSignalSpy err(&t, SIGNAL(errorMsg(QString)));

    t.start();
    QVERIFY2(t.wait(4000), "发送失败后应结束采集");

    QVERIFY(err.count() >= 1);
    QCOMPARE(lost.count(), 1);
    QCOMPARE(fakenet::sendtoCount(), 1); // 只尝试一次就退出
}

/// 无论正常结束还是出错结束，finishedMsg 都必须发射（Widget 依赖它收尾）。
void TestAudioSender::finishedSignalAlwaysFires()
{
    {
        AudioSenderThread t(7, QString::fromLatin1(kTestIp),
                            accessterminal::protocol::kAudioPort);
        QSignalSpy fin(&t, SIGNAL(finishedMsg()));
        QVERIFY(waitForPackets(t, 1, 3000));
        stopAndJoin(t);
        QCOMPARE(fin.count(), 1);
    }
    {
        fakealsa::setReadFatalAfter(0);
        AudioSenderThread t(7, QString::fromLatin1(kTestIp),
                            accessterminal::protocol::kAudioPort);
        QSignalSpy fin(&t, SIGNAL(finishedMsg()));
        t.start();
        QVERIFY(t.wait(4000));
        QCOMPARE(fin.count(), 1); // 出错路径也要发
    }
}

/// stop() 必须让线程及时退出（不能卡在采集循环里）。
void TestAudioSender::stopEndsThreadPromptly()
{
    AudioSenderThread t(7, QString::fromLatin1(kTestIp),
                        accessterminal::protocol::kAudioPort);
    t.start();
    QTest::qWait(30);
    t.stop();
    QElapsedTimer timer;
    timer.start();
    QVERIFY2(t.wait(3000), "stop() 后线程应在 3 秒内退出");
    QVERIFY2(timer.elapsed() < 3000, "退出耗时过长，说明没有响应 stop()");
}

QTEST_MAIN(TestAudioSender)
#include "test_audio_sender.moc"
