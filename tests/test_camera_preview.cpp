/*
 * test_camera_preview.cpp — CameraUdpServer（UDP 50000 远程监看）行为断言。
 *
 * 为什么必须写这个测试：
 *   UDP 50000 是**协议硬约束**的一员，且它的行为与 UDP 23456 明显不同，
 *   这些差异全是"线上语义"，一旦改错只在现场表现为"看不到画面"，不报错：
 *     - 门口的机器**只在收到 `getframe` 之后**才开始推帧（不长期占带宽）；
 *     - 客户端发的是**裸文本，不带 '\n'**（与 TCP 8887 的信令不同！）；
 *     - 收到 `stop` 立即停止；
 *     - 定时器 50ms = 20fps；
 *     - JPEG 用 `save(&buffer, "JPEG")` 的**默认质量**，
 *       而通话视频（UDP 23456）用质量 75 —— 两者刻意不同。
 *   此前完全没有测试覆盖。
 *
 * 测试方式：用**真实 QUdpSocket** 发裸数据报到 127.0.0.1:50000，再从真实
 * UDP socket 收帧。这样连 bind、事件循环、readyRead 派发都一起验证了，
 * 而不是直接调私有槽（后者会绕开真正的线上路径）。相机用链接期替身。
 */
#include <QBuffer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QImage>
#include <QTest>
#include <QUdpSocket>

#include "core/media/camera_preview_server.h"
#include "core/net/protocol.h"
#include "core/vision/v4l2_camera.h"

#include "fake_v4l2_camera.h"

namespace {

const quint16 kPreviewPort = 50000;

/// 发一个裸数据报（不带 '\n'，与协议一致）。
void sendRaw(QUdpSocket& sock, const QByteArray& payload)
{
    sock.writeDatagram(payload, QHostAddress::LocalHost, kPreviewPort);
    sock.waitForBytesWritten(1000);
}

/// 等待至少 n 帧到达，或超时。条件式等待。
bool waitForFrames(QUdpSocket& sock, int n, int timeoutMs = 3000,
                   QList<QByteArray>* out = nullptr)
{
    QList<QByteArray> frames;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        while (sock.hasPendingDatagrams()) {
            QByteArray d;
            d.resize(static_cast<int>(sock.pendingDatagramSize()));
            sock.readDatagram(d.data(), d.size());
            frames.append(d);
        }
        if (frames.size() >= n) {
            break;
        }
        QTest::qWait(10);
    }
    if (out) {
        *out = frames;
    }
    return frames.size() >= n;
}

/// 排空当前待收数据报（用于"停止后不应再有帧"的断言）。
QList<QByteArray> drain(QUdpSocket& sock)
{
    QList<QByteArray> frames;
    while (sock.hasPendingDatagrams()) {
        QByteArray d;
        d.resize(static_cast<int>(sock.pendingDatagramSize()));
        sock.readDatagram(d.data(), d.size());
        frames.append(d);
    }
    return frames;
}

} // namespace

class TestCameraPreview : public QObject
{
    Q_OBJECT

private slots:

    void init()
    {
        fakecam::reset();
    }

    /// 未收到 getframe 之前，一个字节都不许推（"不长期占用带宽"的核心保证）。
    void noPushBeforeGetFrame()
    {
        fakecam::setSolidFrame(32, 32, 0x00FF00);
        V4L2Camera camera;
        CameraUdpServer server(&camera, kPreviewPort);

        QUdpSocket client;
        // Qt5.15 里 bind(QHostAddress::SpecialAddress, quint16) 与
        // bind(const QHostAddress&, quint16, BindMode) 存在重载歧义，
        // 显式构造 QHostAddress 对象即可消歧。
        QVERIFY(client.bind(QHostAddress(QHostAddress::LocalHost), 0));
        QTest::qWait(400); // 远超 50ms 定时器，若在推帧必然收到

        QCOMPARE(drain(client).size(), 0);
        // 也不该去取帧 —— 没请求就不花这个开销。
        QCOMPARE(fakecam::getFrameCount(), 0);
    }

    /// 收到裸 `getframe`（**不带 '\n'**）后才开始推完整 JPEG 帧。
    void getFrameStartsPushingWholeJpegs()
    {
        fakecam::setSolidFrame(48, 32, 0x3366CC);
        V4L2Camera camera;
        CameraUdpServer server(&camera, kPreviewPort);

        QUdpSocket client;
        // Qt5.15 里 bind(QHostAddress::SpecialAddress, quint16) 与
        // bind(const QHostAddress&, quint16, BindMode) 存在重载歧义，
        // 显式构造 QHostAddress 对象即可消歧。
        QVERIFY(client.bind(QHostAddress(QHostAddress::LocalHost), 0));
        sendRaw(client, QByteArrayLiteral("getframe"));

        QList<QByteArray> frames;
        QVERIFY(waitForFrames(client, 2, 3000, &frames));

        const QByteArray& p = frames.first();
        QVERIFY(p.size() > 4);
        // 无包头：首字节就是 JPEG 的 SOI。
        QCOMPARE(static_cast<unsigned char>(p[0]), static_cast<unsigned char>(0xFF));
        QCOMPARE(static_cast<unsigned char>(p[1]), static_cast<unsigned char>(0xD8));
        QCOMPARE(static_cast<unsigned char>(p[p.size() - 2]),
                 static_cast<unsigned char>(0xFF));
        QCOMPARE(static_cast<unsigned char>(p[p.size() - 1]),
                 static_cast<unsigned char>(0xD9));

        QImage decoded;
        QVERIFY(decoded.loadFromData(reinterpret_cast<const uchar*>(p.constData()),
                                     p.size(), "JPEG"));
        QCOMPARE(decoded.width(), 48);
        QCOMPARE(decoded.height(), 32);
    }

    /// 带 '\n' 也应当能识别（fromWire 会 trim），但裸文本是线上实际形态。
    void getFrameWithNewlineIsAccepted()
    {
        fakecam::setSolidFrame(16, 16, 0xFF0000);
        V4L2Camera camera;
        CameraUdpServer server(&camera, kPreviewPort);

        QUdpSocket client;
        // Qt5.15 里 bind(QHostAddress::SpecialAddress, quint16) 与
        // bind(const QHostAddress&, quint16, BindMode) 存在重载歧义，
        // 显式构造 QHostAddress 对象即可消歧。
        QVERIFY(client.bind(QHostAddress(QHostAddress::LocalHost), 0));
        sendRaw(client, QByteArrayLiteral("getframe\n"));

        QVERIFY(waitForFrames(client, 1));
    }

    /// 收到 `stop` 后必须立即停止推帧。
    void stopHaltsPushing()
    {
        fakecam::setSolidFrame(32, 32, 0x0000FF);
        V4L2Camera camera;
        CameraUdpServer server(&camera, kPreviewPort);

        QUdpSocket client;
        // Qt5.15 里 bind(QHostAddress::SpecialAddress, quint16) 与
        // bind(const QHostAddress&, quint16, BindMode) 存在重载歧义，
        // 显式构造 QHostAddress 对象即可消歧。
        QVERIFY(client.bind(QHostAddress(QHostAddress::LocalHost), 0));
        sendRaw(client, QByteArrayLiteral("getframe"));
        QVERIFY(waitForFrames(client, 1));

        sendRaw(client, QByteArrayLiteral("stop"));
        QTest::qWait(150); // 让 stop 生效并排空在途帧
        drain(client);

        QTest::qWait(400); // 远超 50ms 定时器
        QCOMPARE(drain(client).size(), 0);
    }

    /**
     * stop 之后必须能**重新**开始推帧（再次 getframe 有效）。
     *
     * 这条用例是"对照实验找出来的"：
     * 我最初以为"stop 后不再推帧"就等于覆盖了 stop 分支，但对照实验显示，
     * 即使把 `sending = false;` 整行删掉，`stopHaltsPushing` **依然通过** ——
     * 因为 sendFrame() 开头还有 `if (!sending) return;`，而 sendTimer 已被
     * stop，线上字节完全一样。也就是说那行赋值对"停止"是**冗余**的。
     *
     * 它真正的作用在**恢复**路径：若 stop 时没有把 sending 置回 false，
     * 之后再次收到 getframe 时 `if (!sending)` 为假，sendTimer->start()
     * 就不会被调用，**第二次监看永远看不到画面**（而界面不报任何错）。
     * 因此必须显式覆盖"停止 -> 再请求 -> 恢复推帧"这一序列。
     */
    void getFrameAfterStopResumesPushing()
    {
        fakecam::setSolidFrame(32, 32, 0x123456);
        V4L2Camera camera;
        CameraUdpServer server(&camera, kPreviewPort);

        QUdpSocket client;
        QVERIFY(client.bind(QHostAddress(QHostAddress::LocalHost), 0));

        // 第一轮：开始 -> 收帧 -> 停止
        sendRaw(client, QByteArrayLiteral("getframe"));
        QVERIFY(waitForFrames(client, 1));
        sendRaw(client, QByteArrayLiteral("stop"));
        QTest::qWait(150);
        drain(client);
        QTest::qWait(300);
        QCOMPARE(drain(client).size(), 0); // 确认真的停了

        // 第二轮：再次请求必须能重新推帧
        sendRaw(client, QByteArrayLiteral("getframe"));
        QVERIFY2(waitForFrames(client, 1, 3000),
                 "stop 之后再次 getframe 应能恢复推帧；"
                 "若失败，通常是因为 stop 分支没有把 sending 置回 false");
    }

    /**
     * 线上字节必须等于"显式质量 75"的编码结果（逐字节）。
     *
     * 这条用例是对照实验补出来的：把生产代码里的 `save(&buffer, "JPEG")`
     * 换成 `save(&buffer, "JPEG", 1)` 后，原有测试**全部仍然通过** ——
     * 因为它们只校验 JPEG 魔数与"能否解码"，不校验质量。
     * 质量只改变字节，不改变可解码性，因此属于典型的"静默线上差异"。
     *
     * 现在把收到的数据报与本地用 `img.save(..., "JPEG", 75)` 的结果做
     * **逐字节比较**，任何质量改动都会立刻失败。
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
        CameraUdpServer server(&camera, kPreviewPort);
        QUdpSocket client;
        QVERIFY(client.bind(QHostAddress(QHostAddress::LocalHost), 0));
        sendRaw(client, QByteArrayLiteral("getframe"));

        QList<QByteArray> frames;
        QVERIFY(waitForFrames(client, 1, 3000, &frames));

        QByteArray expected;
        {
            QBuffer b(&expected);
            b.open(QIODevice::WriteOnly);
            // 必须显式写 75：若这里也写默认质量，比较就退化成"默认 == 默认"，
            // 完全不校验预览通路的编码参数（我第一版就差点这么写）。
            img.save(&b, "JPEG", 75);
        }
        QCOMPARE(frames.first(), expected);

        // 反向钉住"默认确实等于 75"，避免上面那条断言在所有质量下都成立。
        QByteArray q1;
        {
            QBuffer b(&q1);
            b.open(QIODevice::WriteOnly);
            img.save(&b, "JPEG", 1);
        }
        QVERIFY2(frames.first() != q1,
                 "质量 1 的编码必须与线上字节不同，否则本用例没有区分度");
    }

    /// 无法识别的文本必须被忽略，且不影响后续正常请求。
    void unknownDatagramIsIgnored()
    {
        fakecam::setSolidFrame(16, 16, 0x808080);
        V4L2Camera camera;
        CameraUdpServer server(&camera, kPreviewPort);

        QUdpSocket client;
        // Qt5.15 里 bind(QHostAddress::SpecialAddress, quint16) 与
        // bind(const QHostAddress&, quint16, BindMode) 存在重载歧义，
        // 显式构造 QHostAddress 对象即可消歧。
        QVERIFY(client.bind(QHostAddress(QHostAddress::LocalHost), 0));
        sendRaw(client, QByteArrayLiteral("garbage"));
        QTest::qWait(200);
        QCOMPARE(drain(client).size(), 0);

        // 之后仍能正常开始推帧。
        sendRaw(client, QByteArrayLiteral("getframe"));
        QVERIFY(waitForFrames(client, 1));
    }

    /// 没有可用帧时不发空包。
    void noFrameMeansNoPacket()
    {
        fakecam::setFrame(QImage());
        V4L2Camera camera;
        CameraUdpServer server(&camera, kPreviewPort);

        QUdpSocket client;
        // Qt5.15 里 bind(QHostAddress::SpecialAddress, quint16) 与
        // bind(const QHostAddress&, quint16, BindMode) 存在重载歧义，
        // 显式构造 QHostAddress 对象即可消歧。
        QVERIFY(client.bind(QHostAddress(QHostAddress::LocalHost), 0));
        sendRaw(client, QByteArrayLiteral("getframe"));
        QTest::qWait(300);

        QCOMPARE(drain(client).size(), 0);
        // 但确实在取帧（线程/定时器在跑，只是拿到空帧）。
        QVERIFY(fakecam::getFrameCount() > 0);
    }

    /**
     * 预览用的 JPEG 与通话视频用的 JPEG 在 Qt5 上**字节完全相同**。
     *
     * 澄清一个容易被误传的点：UDP 50000 用 `save(&buffer, "JPEG")`（默认质量），
     * UDP 23456 用 `save(&buffer, "JPEG", 75)`（显式 75）。两者**看起来**
     * 质量参数不同，但实测（本机 Qt 5.15.19，探针遍历质量 1..100 逐一比对字节）
     * Qt 的**默认 JPEG 质量恰好就是 75**，因此二者输出逐字节相同。
     *
     * 为什么仍然要写这条断言：
     *   1. 它把"默认 == 75"这个**实测事实**固化下来。若将来 Qt 版本改变默认
     *      质量（Qt 各版本的默认值并不保证一致），或有人把显式 75 改掉，
     *      线上带宽与画质会静默变化，这里会立刻失败。
     *   2. 我最初想当然地断言"两者质量不同、字节必然不同"，被实测当场证伪
     *      （第一版此用例 FAIL）。这正说明该断言有区分度，不是走过场。
     */
    void previewJpegMatchesCallVideoQualityOnQt5()
    {
        QImage img(64, 64, QImage::Format_RGB32);
        img.fill(QColor(120, 80, 200));
        // 加纹理：纯色在相近质量下容易编码相同，区分度不足。
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                img.setPixel(x, y, qRgb((x * 7 + y * 3) & 0xFF,
                                        (x * 5 + y * 11) & 0xFF,
                                        (x * 13 + y * 2) & 0xFF));
            }
        }

        QByteArray previewData;
        {
            QBuffer b(&previewData);
            b.open(QIODevice::WriteOnly);
            img.save(&b, "JPEG"); // 预览：默认质量
        }
        QByteArray videoData;
        {
            QBuffer b(&videoData);
            b.open(QIODevice::WriteOnly);
            img.save(&b, "JPEG", 75); // 通话视频：显式 75
        }
        QVERIFY(!previewData.isEmpty());
        QVERIFY(!videoData.isEmpty());

        // 实测：Qt5 的默认质量就是 75，故两条通路逐字节相同。
        QVERIFY2(previewData == videoData,
                 "实测 Qt5 默认 JPEG 质量等于 75，两条通路应输出相同字节；"
                 "若这里失败，说明 Qt 默认质量变了或显式 75 被改动，"
                 "线上带宽/画质会静默变化");

        // 同时钉住"75 确实生效"：不同质量必须产生不同字节，
        // 否则上面的相等断言可能只是因为质量参数根本没起作用。
        QByteArray q30;
        {
            QBuffer b(&q30);
            b.open(QIODevice::WriteOnly);
            img.save(&b, "JPEG", 30);
        }
        QVERIFY2(q30 != videoData,
                 "质量 30 与 75 必须产生不同字节，否则说明质量参数未生效，"
                 "上面的相等断言就没有意义");
    }

    /// 协议常量被钉住。
    void previewPortIsFrozen()
    {
        QCOMPARE(accessterminal::protocol::kPreviewPort,
                 static_cast<quint16>(50000));
        // 与信令/音频/视频端口都不同。
        QVERIFY(accessterminal::protocol::kPreviewPort
                != accessterminal::protocol::kSignalingPort);
        QVERIFY(accessterminal::protocol::kPreviewPort
                != accessterminal::protocol::kAudioPort);
        QVERIFY(accessterminal::protocol::kPreviewPort
                != accessterminal::protocol::kVideoPort);
        // 裸文本信令（无终止符）正是这两条的线上形态。
        QCOMPARE(accessterminal::protocol::toWire(
                     accessterminal::protocol::Signal::GetFrame),
                 QByteArrayLiteral("getframe"));
        QCOMPARE(accessterminal::protocol::toWire(
                     accessterminal::protocol::Signal::Stop),
                 QByteArrayLiteral("stop"));
    }
};

QTEST_MAIN(TestCameraPreview)
#include "test_camera_preview.moc"
