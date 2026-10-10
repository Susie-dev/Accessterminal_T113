/**
 * @file test_audio_receiver.cpp
 * @brief AudioReceiverThread（UDP 12345 下行音频播放）的单元测试。
 *
 * 与 AudioSenderThread 是同一类问题：这条通路的字节格式是协议硬约束
 * （裸 PCM、S16_LE/16000/单声道、每包 320 帧 640 字节、无包头），
 * 而它此前**从未被真正执行过** —— tools/devstubs 的 select() 恒返回 0、
 * recvfrom() 恒返回 -1，于是 run() 里两个循环体**一次都没跑过**。
 *
 * 本测试用 fake_net（可投递待收数据报 + 可注入 errno）与 fake_alsa
 * （可注入 write 错误 + 假 mixer）把整条播放通路跑起来。
 *
 * ## 一个必须遵守的时序约束（实测踩过）
 *
 * run() 开头有一段专门的循环，把**启动前已堆积**在 socket 里的包全部丢弃
 * （避免一上来就播放过期音频）。因此测试**不能在 start() 之前投递数据**：
 * 那些包会被丢弃阶段吃掉，永远到不了播放路径，症状是 writeiCount 恒为 0。
 * 所有用例统一走 startFeedAndWait()：先启动、等丢弃阶段过去、再边投边等。
 */
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <sys/select.h>
#include <alsa/asoundlib.h>

#include "fake_alsa.h"
#include "fake_net.h"

// 需要读私有成员并直接观察 run() 行为。
// 必须先把依赖的系统/Qt 头包含完，避免 private->public 泄漏进库头。
#include <QThread>
#include <QString>
#include <atomic>
#include <fcntl.h>

#define private public
#include "core/media/audio_receiver_thread.h"
#undef private

#include "core/net/protocol.h"

namespace {

/// 单包 640 字节 = 320 帧 * 1 声道 * 2 字节。
const int kPacketBytes = 640;

/**
 * RAII 守卫：无论断言是否提前返回，都保证线程被 stop+wait。
 *
 * 这不是洁癖：QtTest 的 QVERIFY 失败会直接 return，若此时线程仍在跑，
 * 成员析构会触发 `QThread: Destroyed while thread is still running`
 * 致命错误并 abort —— 真正的失败原因会被这条致命信息淹没。
 */
struct ThreadGuard {
    AudioReceiverThread& t;
    explicit ThreadGuard(AudioReceiverThread& thread) : t(thread) {}
    ~ThreadGuard()
    {
        t.stop();
        t.wait(4000);
    }
};

/**
 * 启动线程 -> 等丢弃阶段结束 -> 边投递边等待写满 packets 个包。
 * @return true 表示确实完成了 packets 次 snd_pcm_writei
 */
bool startFeedAndWait(AudioReceiverThread& t, int packets,
                      int timeoutMs = 4000)
{
    t.start();
    // 丢弃阶段在队列为空时立即超时跳出，故一个短等待足够越过它。
    QTest::qWait(80);

    QElapsedTimer timer;
    timer.start();
    int pushed = 0;
    while (timer.elapsed() < timeoutMs) {
        if (pushed < packets) {
            fakenet::pushIncomingFilled(1, kPacketBytes, pushed * 16);
            ++pushed;
        }
        if (fakealsa::writeiCount() >= packets) {
            return true;
        }
        QTest::qWait(10);
    }
    return fakealsa::writeiCount() >= packets;
}

/// 等到 writeiCount 至少达到 n（用于错误注入后仍要等后续写）。
bool waitWritei(int n, int timeoutMs = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (fakealsa::writeiCount() >= n) {
            return true;
        }
        QTest::qWait(5);
    }
    return fakealsa::writeiCount() >= n;
}

} // namespace

class TestAudioReceiver : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void playedPacketsMatchProtocolExactly();
    void negotiatesProtocolPlaybackFormat();
    void playsOnlyAfterDataArrives();
    void zeroLengthDatagramIsIgnored();
    void startupDiscardsStaleBacklog();

    void underrunRecoversAndKeepsPlaying();
    void eagainOnWriteIsNotFatal();
    void fatalWriteErrorEmitsErrorAndStops();
    void recvErrorEmitsConnectionLost();
    void eagainOnRecvIsNotConnectionLost();
    void eintrOnRecvRetries();
    void openFailureEmitsErrorAndReturns();
    void stopEndsThreadPromptly();

    void volumeIsClampedAndApplied();
    void volumeMissingControlIsSafe();
    void volumeMixerOpenFailureIsSafe();
};

void TestAudioReceiver::init()
{
    fakealsa::reset();
    fakenet::reset();
}

// ---------------------------------------------------------------------------
// 协议格式
// ---------------------------------------------------------------------------

/// 每个 640 字节的 UDP 包必须恰好写成 320 帧，且内容原样送达 ALSA。
void TestAudioReceiver::playedPacketsMatchProtocolExactly()
{
    AudioReceiverThread t(/*sockfd=*/5);
    ThreadGuard guard(t);

    QVERIFY(startFeedAndWait(t, 3));

    const std::vector<std::vector<char> > chunks = fakealsa::chunks();
    QVERIFY(!chunks.empty());
    for (size_t i = 0; i < chunks.size(); ++i) {
        QCOMPARE(static_cast<int>(chunks[i].size()), kPacketBytes);
    }
    // 每包 640 字节 = 320 帧
    QCOMPARE(fakealsa::framesWritten() / 320, static_cast<int>(chunks.size()));
}

/// 播放侧 ALSA 参数必须与协议一致。
void TestAudioReceiver::negotiatesProtocolPlaybackFormat()
{
    QCOMPARE(accessterminal::protocol::kAudioSampleRate, 16000);
    QCOMPARE(accessterminal::protocol::kAudioChannels, 1);
    QCOMPARE(accessterminal::protocol::kAudioFramesPerPacket, 320);

    AudioReceiverThread t(5);
    ThreadGuard guard(t);
    QVERIFY(startFeedAndWait(t, 1));

    QCOMPARE(fakealsa::lastFormat(), 2); // SND_PCM_FORMAT_S16_LE
    QCOMPARE(fakealsa::lastRate(), 16000);
    QCOMPARE(fakealsa::lastChannels(), 1);
}

/// 没有数据时不应写任何东西（select 超时后继续等，而不是忙等或退出）。
void TestAudioReceiver::playsOnlyAfterDataArrives()
{
    AudioReceiverThread t(5);
    ThreadGuard guard(t);
    t.start();
    QTest::qWait(150); // 远超 20ms 的 select 超时
    QCOMPARE(fakealsa::writeiCount(), 0);
    QVERIFY(t.running.load()); // 仍在等待，没有自行退出
}

/**
 * 长度为零的数据报必须被忽略，不能喂给 ALSA。
 *
 * 若把 bytes==0 当正常包处理，framesExpected=0，snd_pcm_writei(...,0)
 * 在真实 ALSA 上返回 0，会被 `rcw != framesExpected` 判为"短写"刷日志。
 */
void TestAudioReceiver::zeroLengthDatagramIsIgnored()
{
    AudioReceiverThread t(5);
    ThreadGuard guard(t);
    t.start();
    QTest::qWait(80); // 越过丢弃阶段

    fakenet::pushIncoming(std::vector<char>()); // 零长
    QTest::qWait(80);
    QCOMPARE(fakealsa::writeiCount(), 0);       // 零长包不产生写

    fakenet::pushIncomingFilled(1, kPacketBytes, 7);
    QVERIFY(waitWritei(1));
    QCOMPARE(fakealsa::writeiCount(), 1);       // 只有正常包被写
}

/**
 * 启动前堆积的包必须被丢弃，不能播放过期音频（否则端到端延迟会一直累积）。
 */
void TestAudioReceiver::startupDiscardsStaleBacklog()
{
    // 故意在 start() 之前入队 5 个"过期"包
    fakenet::pushIncomingFilled(5, kPacketBytes, 0);

    AudioReceiverThread t(5);
    ThreadGuard guard(t);
    t.start();
    QTest::qWait(150); // 丢弃阶段应已把它们全部吃掉

    QCOMPARE(fakealsa::writeiCount(), 0); // 一个都没播
    QCOMPARE(static_cast<int>(fakenet::received().size()), 5); // 但确实被读走了
}

// ---------------------------------------------------------------------------
// 错误处理
// ---------------------------------------------------------------------------

/// XRUN（-EPIPE）后必须 prepare 并继续播放，不能中断通话。
void TestAudioReceiver::underrunRecoversAndKeepsPlaying()
{
    fakealsa::setEpipeFirst(2);

    AudioReceiverThread t(5);
    ThreadGuard guard(t);
    QSignalSpy lost(&t, SIGNAL(connectionLost()));

    QVERIFY(startFeedAndWait(t, 4)); // 2 次 EPIPE + 2 次成功

    QCOMPARE(lost.count(), 0);
    QVERIFY(fakealsa::prepareCount() >= 2);
    QVERIFY2(fakealsa::framesWritten() > 0, "XRUN 之后仍必须继续播放");
}

/**
 * **关键**：非阻塞设备上的 -EAGAIN 是"缓冲区暂时满"，不是错误。
 *
 * 接收线程以 SND_PCM_NONBLOCK 打开设备（见 run() 的 snd_pcm_open），
 * 因此 snd_pcm_writei 返回 -EAGAIN 是**完全可达的**。若把它按 rcw<0 的
 * 一般错误处理，就会 qWarning + 结束播放线程 —— 表现为通话中途突然单向
 * 无声，且因为 errorMsg 只打日志，界面毫无察觉。
 */
void TestAudioReceiver::eagainOnWriteIsNotFatal()
{
    fakealsa::setWriteEagainFirst(3);

    AudioReceiverThread t(5);
    ThreadGuard guard(t);
    QSignalSpy lost(&t, SIGNAL(connectionLost()));
    QSignalSpy err(&t, SIGNAL(errorMsg(QString)));

    QVERIFY(startFeedAndWait(t, 4)); // 3 次 EAGAIN + 1 次成功

    QCOMPARE(err.count(), 0);   // EAGAIN 不是错误
    QCOMPARE(lost.count(), 0);  // 也不是连接丢失
    QVERIFY2(fakealsa::framesWritten() > 0, "EAGAIN 之后必须继续播放");
}

/// 不可恢复的写错误：报告错误并结束线程（不无限刷日志）。
void TestAudioReceiver::fatalWriteErrorEmitsErrorAndStops()
{
    fakealsa::setFatalError(true);

    AudioReceiverThread t(5);
    ThreadGuard guard(t);
    QSignalSpy err(&t, SIGNAL(errorMsg(QString)));
    t.start();
    QTest::qWait(80);
    fakenet::pushIncomingFilled(1, kPacketBytes, 0);

    QVERIFY(waitWritei(1));
    QVERIFY2(t.wait(4000), "不可恢复写错误后应结束播放线程");
    QVERIFY(err.count() >= 1);
    QCOMPARE(fakealsa::writeiCount(), 1); // 只尝试一次
}

/// recvfrom 真错误（如 EIO）必须报告 connectionLost（触发挂断），并结束线程。
void TestAudioReceiver::recvErrorEmitsConnectionLost()
{
    AudioReceiverThread t(5);
    ThreadGuard guard(t);
    QSignalSpy lost(&t, SIGNAL(connectionLost()));
    QSignalSpy err(&t, SIGNAL(errorMsg(QString)));
    t.start();
    QTest::qWait(80); // 越过丢弃阶段后再注入，否则会被丢弃阶段消耗掉

    // 注意：recvfrom 只有在 select 报告可读时才会被调用，所以必须先让队列
    // 非空（否则 select 直接超时，根本走不到 recvfrom），再让 recvfrom 失败。
    fakenet::setRecvFailFirst(1, EIO);
    fakenet::pushIncomingFilled(1, kPacketBytes, 0);

    QVERIFY2(t.wait(4000), "recvfrom 真错误后应结束");
    QCOMPARE(lost.count(), 1);
    QVERIFY(err.count() >= 1);
}

/**
 * **关键回归**：recvfrom 的 -EAGAIN 绝不能当成连接断开。
 *
 * 收包循环必须一直收到 EAGAIN 才停；若把 EAGAIN 判为断开，第一轮收完就会
 * 误挂断整个通话。
 */
void TestAudioReceiver::eagainOnRecvIsNotConnectionLost()
{
    AudioReceiverThread t(5);
    ThreadGuard guard(t);
    QSignalSpy lost(&t, SIGNAL(connectionLost()));
    t.start();
    QTest::qWait(80);
    fakenet::setRecvFailFirst(3, EAGAIN);

    QVERIFY(startFeedAndWait(t, 2, 3000)); // 中途夹着 3 次 EAGAIN

    QCOMPARE(lost.count(), 0);
    QVERIFY2(fakealsa::framesWritten() > 0, "EAGAIN 后必须继续收包播放");
}

/// recvfrom 被信号打断（EINTR）必须重试，不算错误。
void TestAudioReceiver::eintrOnRecvRetries()
{
    AudioReceiverThread t(5);
    ThreadGuard guard(t);
    QSignalSpy lost(&t, SIGNAL(connectionLost()));
    t.start();
    QTest::qWait(80);
    fakenet::setRecvFailFirst(2, EINTR);

    QVERIFY(startFeedAndWait(t, 1, 3000));

    QCOMPARE(lost.count(), 0);
}

/// 打不开播放设备：报告错误、直接返回，不写任何音频。
void TestAudioReceiver::openFailureEmitsErrorAndReturns()
{
    fakealsa::setOpenFail(true);

    AudioReceiverThread t(5);
    ThreadGuard guard(t);
    QSignalSpy err(&t, SIGNAL(errorMsg(QString)));
    QSignalSpy fin(&t, SIGNAL(finishedMsg()));
    t.start();
    QVERIFY2(t.wait(4000), "打开失败后应立即返回");

    QCOMPARE(err.count(), 1);
    QCOMPARE(fakealsa::writeiCount(), 0);
    // 打开失败是提前 return —— 与 baseline 一致，这条路径不发 finishedMsg。
    // 这是**已记录的既有行为**，不是本测试要改变的东西。
    QCOMPARE(fin.count(), 0);
}

/// stop() 必须让线程及时退出（select 超时为 20ms，不应久等）。
void TestAudioReceiver::stopEndsThreadPromptly()
{
    AudioReceiverThread t(5);
    t.start();
    QTest::qWait(60);
    t.stop();
    QElapsedTimer timer;
    timer.start();
    QVERIFY2(t.wait(3000), "stop() 后应在 3 秒内退出");
    QVERIFY(timer.elapsed() < 3000);
}

// ---------------------------------------------------------------------------
// 音量（假 mixer）
// ---------------------------------------------------------------------------

/// setVolume 必须夹取到 [0,100] 并按区间换算成 mixer 值。
void TestAudioReceiver::volumeIsClampedAndApplied()
{
    fakealsa::setVolumeRange(0, 200);

    AudioReceiverThread t(5);
    t.setVolume(50);
    QCOMPARE(fakealsa::lastVolumeSet(), static_cast<long>(100)); // 200*50/100

    t.setVolume(-10);            // 夹到 0
    QCOMPARE(fakealsa::lastVolumeSet(), static_cast<long>(0));

    t.setVolume(1000);           // 夹到 100
    QCOMPARE(fakealsa::lastVolumeSet(), static_cast<long>(200));
}

/// 控件不存在时只告警，不能崩溃。
void TestAudioReceiver::volumeMissingControlIsSafe()
{
    fakealsa::setSelemFound(false);
    AudioReceiverThread t(5);
    t.setVolume(40); // 不应崩溃
    QCOMPARE(fakealsa::lastVolumeSet(), static_cast<long>(-1)); // 未设置
}

/// mixer 打不开时安全返回（这正是重构时修过的空指针崩溃）。
void TestAudioReceiver::volumeMixerOpenFailureIsSafe()
{
    fakealsa::setMixerOpenFail(true);
    AudioReceiverThread t(5);
    t.setVolume(40); // 不应崩溃
    QCOMPARE(fakealsa::lastVolumeSet(), static_cast<long>(-1));
}

QTEST_MAIN(TestAudioReceiver)
#include "test_audio_receiver.moc"
