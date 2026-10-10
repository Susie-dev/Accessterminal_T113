/*
 * test_video_push.cpp — CameraPushThread（UDP 23456 视频推流）行为断言。
 *
 * 为什么必须写这个测试：
 *   UDP 23456 是**协议硬约束**的一员（每包一个完整 JPEG，不重组、不重传，
 *   端口固定 23456），但此前**没有任何测试覆盖**。它与刚修过缺陷的两个音频
 *   线程属于同一类"看起来有测试、其实一步没跑"的通路：
 *   - devstubs 的 sendto() 只"返回 len"，不记录载荷；
 *   - 其 inet_pton() 永远返回 0（本文件用可记录的 fake_net 替代）；
 *   - 真实 V4L2Camera 需要 /dev/video0，开发机上不存在。
 *   因此这里用 tests/fake_net.cpp 记录线上字节，用 tests/fake_v4l2_camera.cpp
 *   替换相机（链接期替身，生产代码不改一行）。
 *
 * 线程时序：run() 每轮"取帧 -> 编码 -> sendto -> 分片 sleep(40ms)"。
 * 所有"应该有包"的断言都等**性质**成立（包数到达），不等固定时长。
 *
 * 踩坑记录：最初我把 camera 参数写成 0（即 nullptr），于是 run() 里的
 * `camera ? camera->getFrame() : QImage()` 永远走空分支，一个包都不发、
 * getFrameCount() 恒为 0。测试必须传入**真实的替身对象**（fakecam 已提供
 * V4L2Camera 的成员定义），否则测的就不是取帧通路。
 */
#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTest>
#include <QSignalSpy>

#include "core/media/video_push_thread.h"
#include "core/net/protocol.h"
#include "core/vision/v4l2_camera.h"

#include "fake_net.h"
#include "fake_v4l2_camera.h"

namespace {

const unsigned short kVideoPort = 23456;

/// 保证任何断言提前返回都不会留下仍在运行的 QThread
/// （否则析构时 Qt 会 QFATAL: Destroyed while thread is still running，
///  把真正的失败原因淹没）。
struct ThreadGuard {
    CameraPushThread& t;
    explicit ThreadGuard(CameraPushThread& thread) : t(thread) {}
    ~ThreadGuard() { t.stop(); t.wait(4000); }
    ThreadGuard(const ThreadGuard&) = delete;
    ThreadGuard& operator=(const ThreadGuard&) = delete;
};

/// 等到至少 n 个数据报被发出，或超时。条件式等待，避免固定时长导致偶发失败。
bool waitForPackets(int n, int timeoutMs = 4000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (fakenet::sendtoCount() >= n) {
            return true;
        }
        QTest::qWait(10);
    }
    return fakenet::sendtoCount() >= n;
}

} // namespace

class TestVideoPush : public QObject
{
    Q_OBJECT

private slots:

    void init()
    {
        fakenet::reset();
        fakecam::reset();
    }

    /// 每个数据报都必须是**完整可解压的 JPEG**，不带任何自定义包头。
    void eachPacketIsAWholeJpegWithoutHeader()
    {
        fakecam::setSolidFrame(64, 48, 0x3366CC);
        V4L2Camera camera;
        CameraPushThread t(&camera, QStringLiteral("192.168.1.4"), kVideoPort);
        ThreadGuard guard(t);
        t.start();
        QVERIFY(waitForPackets(1));

        const std::vector<fakenet::Datagram> sent = fakenet::sent();
        QVERIFY(!sent.empty());
        const std::vector<char>& p = sent[0].payload;
        QVERIFY(p.size() > 4);

        // JPEG 魔数 SOI = FF D8；收尾 EOI = FF D9。
        // 这就是"无包头"的证据：第 0 字节就是图像本身，不是长度或魔数前缀。
        QCOMPARE(static_cast<unsigned char>(p[0]), static_cast<unsigned char>(0xFF));
        QCOMPARE(static_cast<unsigned char>(p[1]), static_cast<unsigned char>(0xD8));
        QCOMPARE(static_cast<unsigned char>(p[p.size() - 2]),
                 static_cast<unsigned char>(0xFF));
        QCOMPARE(static_cast<unsigned char>(p[p.size() - 1]),
                 static_cast<unsigned char>(0xD9));

        // 并确认真能被解压回一张图（证明是完整帧，不是被截断的片段）。
        QImage decoded;
        QVERIFY(decoded.loadFromData(reinterpret_cast<const uchar*>(&p[0]),
                                     static_cast<int>(p.size()), "JPEG"));
        QCOMPARE(decoded.width(), 64);
        QCOMPARE(decoded.height(), 48);
    }

    /// 发往配置的 IP 与**固定端口 23456**。
    void sendsToConfiguredAddressAndVideoPort()
    {
        fakecam::setSolidFrame(16, 16, 0x00FF00);
        V4L2Camera camera;
        CameraPushThread t(&camera, QStringLiteral("192.168.1.4"), kVideoPort);
        ThreadGuard guard(t);
        t.start();
        QVERIFY(waitForPackets(1));

        const fakenet::Datagram d = fakenet::sent()[0];
        // 192.168.1.4 网络序
        QCOMPARE(d.destAddr, 0x0401A8C0U);
        QCOMPARE(d.destPort, static_cast<unsigned short>(23456));
        QCOMPARE(d.destPort, static_cast<unsigned short>(
                     accessterminal::protocol::kVideoPort));
    }

    /**
     * 线上字节必须等于"显式质量 75"的编码结果（逐字节）。
     *
     * 对照实验补出来的用例：把生产代码的 `save(&buffer, "JPEG", 75)`
     * 改成 `save(&buffer, "JPEG", 1)` 后，原有测试**全部仍然通过** ——
     * 它们只校验 JPEG 魔数与"能否解码"，而质量只改变字节、不改变可解码性，
     * 属于典型的"静默线上差异"（带宽与画质变了，谁也不报错）。
     *
     * 现在拿收到的数据报与本地 `img.save(..., "JPEG", 75)` 做逐字节比较。
     * 注意不能拿"默认质量"做期望值：那样比较会退化成默认==默认，失去意义
     * （虽然实测 Qt5 默认恰好也是 75，但断言必须显式钉住 75 这个数）。
     */
    void wireJpegMatchesExplicitQuality75()
    {
        const int w = 40, h = 24;
        QImage img(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                img.setPixel(x, y, qRgb((x * 9 + y * 5) & 0xFF,
                                        (x * 3 + y * 17) & 0xFF,
                                        (x * 21 + y * 7) & 0xFF));
            }
        }
        fakecam::setFrame(img);

        V4L2Camera camera;
        CameraPushThread t(&camera, QStringLiteral("192.168.1.4"), kVideoPort);
        ThreadGuard guard(t);
        t.start();
        QVERIFY(waitForPackets(1));

        const std::vector<char> p = fakenet::sent()[0].payload;
        const QByteArray got(&p[0], static_cast<int>(p.size()));

        QByteArray expected;
        {
            QBuffer b(&expected);
            b.open(QIODevice::WriteOnly);
            img.save(&b, "JPEG", 75); // 通话视频固定 75
        }
        QCOMPARE(got, expected);

        // 反向钉住区分度：换一个质量必须产生不同字节。
        QByteArray q1;
        {
            QBuffer b(&q1);
            b.open(QIODevice::WriteOnly);
            img.save(&b, "JPEG", 1);
        }
        QVERIFY2(got != q1,
                 "质量 1 的编码必须与线上字节不同，否则本用例没有区分度");
    }

    /// 协议常量被钉住：四个端口互不重叠。
    void videoPortIsFrozenAndDistinct()
    {
        QCOMPARE(accessterminal::protocol::kVideoPort,
                 static_cast<quint16>(23456));
        QCOMPARE(accessterminal::protocol::kSignalingPort,
                 static_cast<quint16>(8887));
        QCOMPARE(accessterminal::protocol::kAudioPort,
                 static_cast<quint16>(12345));
        QCOMPARE(accessterminal::protocol::kPreviewPort,
                 static_cast<quint16>(50000));
        QVERIFY(accessterminal::protocol::kVideoPort
                != accessterminal::protocol::kAudioPort);
        QVERIFY(accessterminal::protocol::kVideoPort
                != accessterminal::protocol::kPreviewPort);
    }

    /// 没有可用帧时**不能发空包**（否则对端会收到 0 字节数据报）。
    void noFrameMeansNoPacket()
    {
        fakecam::setFrame(QImage()); // 明确"暂无帧"
        V4L2Camera camera;
        CameraPushThread t(&camera, QStringLiteral("192.168.1.4"), kVideoPort);
        ThreadGuard guard(t);
        t.start();
        QTest::qWait(300);

        QCOMPARE(fakenet::sendtoCount(), 0);
        // 但仍应在取帧（只是取到了空帧），说明线程确实在跑而不是立刻退出。
        QVERIFY(fakecam::getFrameCount() > 0);
    }

    /// 多帧都要发出，且都发往同一目标（长通话的稳定性）。
    void everyFrameIsPushed()
    {
        fakecam::setSolidFrame(32, 32, 0xABCDEF);
        V4L2Camera camera;
        CameraPushThread t(&camera, QStringLiteral("10.0.0.7"), kVideoPort);
        ThreadGuard guard(t);
        t.start();
        QVERIFY(waitForPackets(3));

        const std::vector<fakenet::Datagram> sent = fakenet::sent();
        QVERIFY(sent.size() >= 3);
        for (size_t i = 0; i < sent.size(); ++i) {
            QCOMPARE(sent[i].destPort, static_cast<unsigned short>(23456));
            QCOMPARE(sent[i].destAddr, 0x0700000AU); // 10.0.0.7
            QVERIFY(!sent[i].payload.empty());
        }
    }

    /// socket() 失败必须干净退出，且一个包都不发（兜底路径）。
    void socketFailureEmitsNothing()
    {
        fakecam::setSolidFrame(16, 16, 0xFF0000);
        fakenet::setSocketFail(true);
        V4L2Camera camera;
        CameraPushThread t(&camera, QStringLiteral("192.168.1.4"), kVideoPort);
        ThreadGuard guard(t);
        t.start();

        QVERIFY(t.wait(4000)); // 应自行结束
        QCOMPARE(fakenet::sendtoCount(), 0);
        QCOMPARE(fakecam::getFrameCount(), 0); // 连取帧都不应发生
    }

    /// stop() 必须及时生效：不该等一整轮 40ms 分片才退出。
    void stopEndsThreadPromptly()
    {
        fakecam::setSolidFrame(320, 240, 0x123456); // 稍大，编码有耗时
        V4L2Camera camera;
        CameraPushThread t(&camera, QStringLiteral("192.168.1.4"), kVideoPort);
        ThreadGuard guard(t);
        t.start();
        QVERIFY(waitForPackets(1));

        QElapsedTimer timer;
        timer.start();
        t.stop();
        const bool finished = t.wait(3000);
        const qint64 ms = timer.elapsed();

        QVERIFY2(finished, "stop() 后线程应在超时内结束");
        // 一轮分片 sleep 是 8*5ms=40ms；给足余量但必须明显小于长阻塞。
        QVERIFY2(ms < 1500, qPrintable(QString("stop() 耗时 %1 ms，疑似未分片").arg(ms)));
    }

    /// 停止标志一旦置位不可逆：run() 内不得重置（第 10 轮真实缺陷的回归）。
    void stopBeforeStartIsNotLostByRun()
    {
        fakecam::setSolidFrame(16, 16, 0x00FFFF);
        V4L2Camera camera;
        CameraPushThread t(&camera, QStringLiteral("192.168.1.4"), kVideoPort);
        ThreadGuard guard(t);
        // 模拟"start() 与 run() 之间到达 stop()"：先 stop 再 start。
        t.stop();
        t.start();

        QVERIFY2(t.wait(4000), "先 stop 再 start 时线程不应开始推流");
        QCOMPARE(fakenet::sendtoCount(), 0);
    }
};

QTEST_MAIN(TestVideoPush)
#include "test_video_push.moc"
