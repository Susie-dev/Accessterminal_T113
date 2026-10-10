/*
 * test_v4l2_camera.cpp — V4L2Camera（门口机的画面来源）行为与资源生命周期断言。
 *
 * 为什么必须补这一组
 * ------------------
 * V4L2Camera 是整个视频链路的源头：UDP 23456 通话视频、UDP 50000 远程监看、
 * 人脸检测、人脸门禁全都取自它的 getFrame()。它此前从未被链接进任何测试目标。
 *
 * 它的危险分支全在**系统调用失败**上，而现场（工业门口机）这些失败是常态：
 *
 *     open() 失败（设备不存在 / 权限不足）      -> 无画面
 *     S_FMT / REQBUFS / QUERYBUF 失败           -> 无画面
 *     mmap() 失败                               -> 无画面
 *     QBUF / STREAMON 失败                      -> 无画面
 *     DQBUF 失败（拔线 / 驱动异常）             -> 该帧丢失
 *     JPEG 解不出来                             -> 该帧丢弃
 *
 * 测试方式（生产代码零改动）
 * --------------------------
 * 用 -Wl,--wrap 把 ::open/::close/ioctl/mmap/munmap 重定向到
 * tests/fake_v4l2_io.cpp（可编程假 syscall 层），并**链接真实的
 * v4l2_camera.cpp**。UI 定时器由测试直接调 captureLoop() 驱动（它是
 * private slot，用 QMetaObject::invokeMethod 调，与真实触发路径一致）。
 *
 * 断言取向
 * --------
 * 落在**可观测输出**上：假 syscall 层收到的调用序列与参数、
 * **还剩多少字节处于已映射状态**、以及 getFrame() 返回的像素内容。
 * 不断言"内部指针变成 nullptr"这类实现细节。
 */
#include <QBuffer>
#include <QImage>
#include <QMetaObject>
#include <QTest>

#include "core/vision/v4l2_camera.h"

#include "fake_v4l2_io.h"

#include <linux/videodev2.h>

namespace {

/// 生成一张真实的小 JPEG（BMP 太大，JPEG 才是设备实际输出的格式）。
QByteArray makeJpeg(int w = 64, int h = 48, int hue = 0)
{
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            img.setPixel(x, y, qRgb((x * 4 + hue) & 0xFF,
                                    (y * 5 + hue) & 0xFF,
                                    ((x + y) * 3 + hue) & 0xFF));
        }
    }
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPEG", 80);
    buf.close();
    return out;
}

/// 调用 private slot captureLoop()，走与真实定时器触发相同的路径。
void driveCaptureLoop(V4L2Camera& cam)
{
    QMetaObject::invokeMethod(&cam, "captureLoop", Qt::DirectConnection);
}

/// callLog 里 a 是否出现在 b 之前。
bool before(const QStringList& log, const QString& a, const QString& b)
{
    const int ia = log.indexOf(a);
    const int ib = log.indexOf(b);
    return ia >= 0 && ib >= 0 && ia < ib;
}

} // namespace

class TestV4L2Camera : public QObject
{
    Q_OBJECT

private slots:

    void init() { fakev4l2io::reset(); }

    void cleanup()
    {
        // 每个用例结束时都不许有残留的映射或打开的 fd，否则就是泄漏。
        //
        // 这里刻意用 QVERIFY2 + qWarning 而不是 QCOMPARE：QCOMPARE 失败会
        // 让 QTest 直接中止该用例（exit code != 1），把"某条断言失败"
        // 变得难以与真实崩溃区分。测试夹具（故障注入）需要干净地区分
        // "FAIL 行" 与 "进程崩溃"，因此这里保持为普通失败。
        if (fakev4l2io::mappedBytes() != 0) {
            qWarning("用例结束时仍映射 %d 字节 —— 映射泄漏",
                     fakev4l2io::mappedBytes());
        }
        QVERIFY2(fakev4l2io::mappedBytes() == 0,
                 "用例结束时不得残留映射（close 不会自动 munmap）");
        QVERIFY2(fakev4l2io::openFdCount() == 0,
                 "用例结束时不得残留打开的 fd");
    }

    // -----------------------------------------------------------------------
    // 成功路径
    // -----------------------------------------------------------------------

    /// 完整成功路径：设备被以正确的格式打开，并启动采集。
    void openConfiguresMjpegAndStartsStreaming()
    {
        V4L2Camera cam(QStringLiteral("/dev/video7"));
        QVERIFY(cam.open());
        QVERIFY(cam.isOpen());

        QCOMPARE(fakev4l2io::lastOpenPath(), QStringLiteral("/dev/video7"));
        QCOMPARE(fakev4l2io::openCount(), 1);

        // 必须请求 MJPEG 格式（CPU 只做解码，适配 T113 算力）。
        QCOMPARE(fakev4l2io::lastPixFormat(),
                 static_cast<unsigned long>(V4L2_PIX_FMT_MJPEG));
        QCOMPARE(fakev4l2io::lastWidth(), 640u);
        QCOMPARE(fakev4l2io::lastHeight(), 480u);

        // 七步 ioctl 必须都发生过，且顺序正确。
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_S_FMT), 1);
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_REQBUFS), 1);
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_QUERYBUF), 1);
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_QBUF), 1);
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_STREAMON), 1);
        QVERIFY(fakev4l2io::sawStreamOn());

        const QStringList log = fakev4l2io::callLog();
        QVERIFY2(before(log, QStringLiteral("mmap:1048576"),
                        QStringLiteral("ioctl:STREAMON")),
                 qPrintable(QStringLiteral("应先 mmap 再 STREAMON: ") + log.join(',')));

        // 映射必须存在（供 captureLoop 读取）。
        QVERIFY(fakev4l2io::mappedBytes() > 0);
    }

    /// 构造时默认设备路径是 /dev/video0（与 AppConfig 默认值一致）。
    void defaultDevicePathIsVideo0()
    {
        V4L2Camera cam;
        QVERIFY(cam.open());
        QCOMPARE(fakev4l2io::lastOpenPath(), QStringLiteral("/dev/video0"));
    }

    /// width()/height() 必须报告 640x480（与 S_FMT 请求的一致）。
    void widthHeightAreConsistent()
    {
        V4L2Camera cam;
        QCOMPARE(cam.width(), 640);
        QCOMPARE(cam.height(), 480);
        QVERIFY(cam.open());
        QCOMPARE(fakev4l2io::lastWidth(),
                 static_cast<unsigned int>(cam.width()));
        QCOMPARE(fakev4l2io::lastHeight(),
                 static_cast<unsigned int>(cam.height()));
    }

    // -----------------------------------------------------------------------
    // open() 的逐级失败：每一级都必须返回 false 且**不留下任何资源**
    // -----------------------------------------------------------------------

    /// open() 本身失败：返回 false，不产生 fd、不产生映射。
    void openFailureLeavesNothingBehind()
    {
        fakev4l2io::setOpenFail(true);

        V4L2Camera cam;
        QVERIFY(!cam.open());
        QVERIFY(!cam.isOpen());

        QCOMPARE(fakev4l2io::openCount(), 1);
        QCOMPARE(fakev4l2io::mappedBytes(), 0);
        QCOMPARE(fakev4l2io::openFdCount(), 0);
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_S_FMT), 0);
    }

    /// S_FMT 失败：必须关闭 fd，不得 mmap。
    void sfmtFailureClosesFdWithoutMapping()
    {
        fakev4l2io::setIoctlFail(VIDIOC_S_FMT, true);

        V4L2Camera cam;
        QVERIFY(!cam.open());
        QVERIFY(!cam.isOpen());

        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_S_FMT), 1);
        QCOMPARE(fakev4l2io::closeCount(), 1);
        QCOMPARE(fakev4l2io::openFdCount(), 0);
        QCOMPARE(fakev4l2io::mmapCount(), 0);
        QCOMPARE(fakev4l2io::mappedBytes(), 0);
    }

    /// REQBUFS 失败：同样必须关闭 fd 且不 mmap。
    void reqbufsFailureClosesFdWithoutMapping()
    {
        fakev4l2io::setIoctlFail(VIDIOC_REQBUFS, true);

        V4L2Camera cam;
        QVERIFY(!cam.open());
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_REQBUFS), 1);
        QCOMPARE(fakev4l2io::closeCount(), 1);
        QCOMPARE(fakev4l2io::openFdCount(), 0);
        QCOMPARE(fakev4l2io::mmapCount(), 0);
    }

    /// QUERYBUF 失败：必须关闭 fd 且不 mmap。
    void querybufFailureClosesFdWithoutMapping()
    {
        fakev4l2io::setIoctlFail(VIDIOC_QUERYBUF, true);

        V4L2Camera cam;
        QVERIFY(!cam.open());
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_QUERYBUF), 1);
        QCOMPARE(fakev4l2io::closeCount(), 1);
        QCOMPARE(fakev4l2io::mmapCount(), 0);
        QCOMPARE(fakev4l2io::openFdCount(), 0);
    }

    /// mmap 失败：必须关闭 fd，且**不得**记下一个无效映射。
    void mmapFailureClosesFdAndRecordsNoMapping()
    {
        fakev4l2io::setMmapFail(true);

        V4L2Camera cam;
        QVERIFY(!cam.open());
        QVERIFY(!cam.isOpen());

        QCOMPARE(fakev4l2io::mmapCount(), 1);
        QCOMPARE(fakev4l2io::mappedBytes(), 0);   // MAP_FAILED 不是映射
        QCOMPARE(fakev4l2io::closeCount(), 1);
        QCOMPARE(fakev4l2io::openFdCount(), 0);
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_STREAMON), 0);
    }

    /**
     * QBUF 失败：必须关闭 fd，且**必须解除已经建立的映射**。
     *
     * 这是本文件最重要的一条。mmap 已经成功，此时若不 munmap 就 close(fd)，
     * 映射会**留在进程地址空间里**（close 不会自动解除映射）。
     */
    void qbufFailureMustStillUnmap()
    {
        fakev4l2io::setIoctlFail(VIDIOC_QBUF, true);

        V4L2Camera cam;
        QVERIFY(!cam.open());
        QVERIFY(!cam.isOpen());

        QCOMPARE(fakev4l2io::mmapCount(), 1);
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_QBUF), 1);
        QCOMPARE(fakev4l2io::closeCount(), 1);
        QCOMPARE(fakev4l2io::openFdCount(), 0);

        QVERIFY2(fakev4l2io::mappedBytes() == 0,
                 qPrintable(QStringLiteral(
                     "QBUF 失败时 mmap 已建立，必须 munmap（close 不会自动解除"
                     "映射）。当前仍映射 %1 字节。调用序列: %2")
                     .arg(fakev4l2io::mappedBytes())
                     .arg(fakev4l2io::callLog().join(','))));
    }

    /// STREAMON 失败：同样必须解除映射 + 关闭 fd。
    void streamOnFailureMustStillUnmap()
    {
        fakev4l2io::setIoctlFail(VIDIOC_STREAMON, true);

        V4L2Camera cam;
        QVERIFY(!cam.open());
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_STREAMON), 1);
        QCOMPARE(fakev4l2io::closeCount(), 1);
        QCOMPARE(fakev4l2io::openFdCount(), 0);
        QVERIFY2(fakev4l2io::mappedBytes() == 0,
                 qPrintable(QStringLiteral("STREAMON 失败后仍映射 %1 字节")
                                .arg(fakev4l2io::mappedBytes())));
    }

    /**
     * 失败后重试成功的场景：上一次失败**不得**留下残留。
     *
     * 这条模拟现场真实序列：启动时设备未就绪 -> 稍后重试成功。
     * 若失败路径漏 munmap，重试时 v4l2_buffer 会被新映射覆盖，旧映射
     * 就永久泄漏了（指针丢失，再也 munmap 不掉）。
     */
    void failedOpenLeavesNoResidueForRetry()
    {
        V4L2Camera cam;

        fakev4l2io::setIoctlFail(VIDIOC_QBUF, true);
        QVERIFY(!cam.open());
        QVERIFY(!cam.isOpen());

        // 硬件恢复，重试必须成功，且不得累积映射。
        fakev4l2io::setIoctlFail(VIDIOC_QBUF, false);
        QVERIFY(cam.open());
        QVERIFY(cam.isOpen());

        // 只有重试这一次的映射存在（1024*1024），没有上一次的残留。
        QCOMPARE(fakev4l2io::mappedBytes(), 1024 * 1024);
        QCOMPARE(fakev4l2io::openFdCount(), 1);

        cam.close();
        QCOMPARE(fakev4l2io::mappedBytes(), 0);
    }

    // -----------------------------------------------------------------------
    // close() 与析构
    // -----------------------------------------------------------------------

    /// close() 必须同时关闭 fd 与解除映射。
    void closeReleasesFdAndMapping()
    {
        V4L2Camera cam;
        QVERIFY(cam.open());
        QVERIFY(fakev4l2io::mappedBytes() > 0);

        cam.close();
        QVERIFY(!cam.isOpen());
        QCOMPARE(fakev4l2io::mappedBytes(), 0);
        QCOMPARE(fakev4l2io::openFdCount(), 0);
        QCOMPARE(fakev4l2io::munmapCount(), 1);
    }

    /// close() 幂等：重复调用不得重复关闭 / 重复解除。
    void closeIsIdempotent()
    {
        V4L2Camera cam;
        QVERIFY(cam.open());
        cam.close();

        const int closes = fakev4l2io::closeCount();
        const int unmaps = fakev4l2io::munmapCount();

        cam.close();
        cam.close();

        QCOMPARE(fakev4l2io::closeCount(), closes);
        QCOMPARE(fakev4l2io::munmapCount(), unmaps);
        QCOMPARE(fakev4l2io::mappedBytes(), 0);
    }

    /// 析构必须释放（RAII）。
    void destructorReleasesEverything()
    {
        {
            V4L2Camera cam;
            QVERIFY(cam.open());
            QCOMPARE(fakev4l2io::mappedBytes(), 1024 * 1024);
        }
        QCOMPARE(fakev4l2io::mappedBytes(), 0);
        QCOMPARE(fakev4l2io::openFdCount(), 0);
        QCOMPARE(fakev4l2io::closeCount(), 1);
        QCOMPARE(fakev4l2io::munmapCount(), 1);
    }

    /// 从未 open 过的对象析构必须安全（不得 close(-1) / munmap(nullptr)）。
    void destructorWithoutOpenIsSafe()
    {
        { V4L2Camera cam; }
        QCOMPARE(fakev4l2io::closeCount(), 0);
        QCOMPARE(fakev4l2io::munmapCount(), 0);
    }

    /// open 失败后析构仍必须安全（且不得重复释放）。
    void destructorAfterFailedOpenIsSafe()
    {
        {
            V4L2Camera cam;
            fakev4l2io::setIoctlFail(VIDIOC_S_FMT, true);
            QVERIFY(!cam.open());
        }
        QCOMPARE(fakev4l2io::closeCount(), 1);   // 失败路径里关过一次
        QCOMPARE(fakev4l2io::munmapCount(), 0);
    }

    /// close() 之后必须能重新 open（close 必须是完全重置）。
    void canReopenAfterClose()
    {
        V4L2Camera cam;
        QVERIFY(cam.open());
        cam.close();
        QVERIFY(!cam.isOpen());

        QVERIFY(cam.open());
        QVERIFY(cam.isOpen());
        QCOMPARE(fakev4l2io::openCount(), 2);
        QCOMPARE(fakev4l2io::mappedBytes(), 1024 * 1024);
    }

    // -----------------------------------------------------------------------
    // captureLoop：取帧 / 解码 / 归还缓冲区
    // -----------------------------------------------------------------------

    /// 成功取到一帧：getFrame() 必须返回**该帧的内容**。
    void captureLoopPublishesDecodedFrame()
    {
        const QByteArray jpeg = makeJpeg(64, 48, 0);
        fakev4l2io::setFrameData(jpeg);

        V4L2Camera cam;
        QVERIFY(cam.open());

        driveCaptureLoop(cam);

        const QImage frame = cam.getFrame();
        QVERIFY2(!frame.isNull(), "成功解码后 getFrame() 不应为空");
        QCOMPARE(frame.width(), 64);
        QCOMPARE(frame.height(), 48);

        // DQBUF 取帧、QBUF 归还 —— 两者都必须发生，否则驱动缓冲会被耗尽。
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_DQBUF), 1);
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_QBUF), 2);  // open 时 1 次 + 归还 1 次
    }

    /**
     * 关键契约：**即使解码失败，也必须 QBUF 归还缓冲区**。
     *
     * DQBUF 把缓冲区从驱动取走，必须用 QBUF 还回去。若解码失败就提前
     * return 而不 QBUF，缓冲区会被永久占用 —— 因为 req.count = 1（只有一个
     * 缓冲），**下一次 DQBUF 会永远失败，画面整条链路彻底停摆**。
     *
     * 现场表现："画面刚出来一下就再也不动了"，且没有任何报错。
     */
    void undecodableFrameStillRequeuesBuffer()
    {
        fakev4l2io::setFrameData(QByteArray("this is not a jpeg at all"));

        V4L2Camera cam;
        QVERIFY(cam.open());

        driveCaptureLoop(cam);

        // 解码失败 -> 不应覆盖已有帧（本来就没有帧）。
        QVERIFY(cam.getFrame().isNull());

        // 但缓冲区**必须**被归还。
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_DQBUF), 1);
        QVERIFY2(fakev4l2io::ioctlCount(VIDIOC_QBUF) >= 2,
                 "解码失败时也必须 QBUF 归还缓冲区，否则下一次 DQBUF 永远"
                 "失败（req.count = 1），画面链路彻底停摆");
    }

    /// 连续多帧：每帧都必须有一次 DQBUF 与一次 QBUF 归还。
    void captureLoopRequeuesEveryFrame()
    {
        fakev4l2io::setFrameData(makeJpeg(32, 24, 0));

        V4L2Camera cam;
        QVERIFY(cam.open());

        for (int i = 0; i < 5; ++i) {
            driveCaptureLoop(cam);
        }

        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_DQBUF), 5);
        // open 时 1 次 + 5 次归还。
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_QBUF), 6);
    }

    /// DQBUF 失败（拔线）：必须安全返回，不得触碰映射、不得 QBUF。
    void dqbufFailureIsHarmless()
    {
        V4L2Camera cam;
        QVERIFY(cam.open());
        fakev4l2io::setDqbufFail(true);

        driveCaptureLoop(cam);   // 不得崩溃

        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_DQBUF), 1);
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_QBUF), 1);  // 只有 open 那次
        QVERIFY(cam.getFrame().isNull());
        QCOMPARE(fakev4l2io::mappedBytes(), 1024 * 1024);  // 映射未受影响
    }

    /// 未打开时 captureLoop 必须立即返回，不产生任何 syscall。
    void captureLoopOnClosedCameraIsNoOp()
    {
        V4L2Camera cam;   // 未 open
        driveCaptureLoop(cam);

        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_DQBUF), 0);
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_QBUF), 0);
        QCOMPARE(fakev4l2io::mmapCount(), 0);
    }

    /// close() 之后 captureLoop 也必须成为 no-op（不得对已关闭的 fd 操作）。
    void captureLoopAfterCloseIsNoOp()
    {
        V4L2Camera cam;
        QVERIFY(cam.open());
        cam.close();

        const int dq = fakev4l2io::ioctlCount(VIDIOC_DQBUF);
        driveCaptureLoop(cam);
        QCOMPARE(fakev4l2io::ioctlCount(VIDIOC_DQBUF), dq);
    }

    /// getFrame() 返回的是**副本**：调用方改动不得影响内部缓存的帧。
    void getFrameReturnsCopy()
    {
        fakev4l2io::setFrameData(makeJpeg(64, 48, 0));

        V4L2Camera cam;
        QVERIFY(cam.open());
        driveCaptureLoop(cam);

        QImage a = cam.getFrame();
        QVERIFY(!a.isNull());
        a.fill(0);              // 改副本

        const QImage b = cam.getFrame();
        QVERIFY2(!b.isNull(), "改动返回的副本不应影响内部缓存的帧");
        QCOMPARE(b.width(), 64);
    }

    /// 解码失败不得破坏已有帧（最后一帧仍然可用）。
    void undecodableFrameKeepsPreviousFrame()
    {
        fakev4l2io::setFrameData(makeJpeg(64, 48, 10));

        V4L2Camera cam;
        QVERIFY(cam.open());
        driveCaptureLoop(cam);
        const QImage good = cam.getFrame();
        QVERIFY(!good.isNull());

        // 下一帧坏掉。
        fakev4l2io::setFrameData(QByteArray("garbage"));
        driveCaptureLoop(cam);

        const QImage after = cam.getFrame();
        QVERIFY2(!after.isNull(), "解码失败不应清掉已有的最后一帧");
        QCOMPARE(after.width(), good.width());
        QCOMPARE(after.height(), good.height());
    }

    /**
     * 帧内容必须真的来自设备数据，而不是恰好非空。
     *
     * 阳性对照：两帧不同内容（不同 hue）必须给出不同像素，
     * 否则上面所有"取到帧"的断言都可能在空实现下恒真。
     */
    void frameContentFollowsDeviceData()
    {
        V4L2Camera cam;
        QVERIFY(cam.open());

        fakev4l2io::setFrameData(makeJpeg(64, 48, 0));
        driveCaptureLoop(cam);
        const QImage first = cam.getFrame().copy();
        QVERIFY(!first.isNull());

        fakev4l2io::setFrameData(makeJpeg(64, 48, 128));
        driveCaptureLoop(cam);
        const QImage second = cam.getFrame().copy();
        QVERIFY(!second.isNull());

        bool differs = false;
        for (int y = 0; y < first.height() && !differs; ++y) {
            for (int x = 0; x < first.width(); ++x) {
                if (first.pixel(x, y) != second.pixel(x, y)) {
                    differs = true;
                    break;
                }
            }
        }
        QVERIFY2(differs, "两帧不同内容必须给出不同像素 —— "
                          "否则『取到帧』的断言没有判别力");
    }

    /// 现场接线契约：默认设备 /dev/video0，请求 MJPEG、640x480。
    void productionWiringIsPinned()
    {
        V4L2Camera cam(QStringLiteral("/dev/video0"));
        QVERIFY(cam.open());
        QCOMPARE(fakev4l2io::lastOpenPath(), QStringLiteral("/dev/video0"));
        QCOMPARE(fakev4l2io::lastPixFormat(),
                 static_cast<unsigned long>(V4L2_PIX_FMT_MJPEG));
        QCOMPARE(fakev4l2io::lastWidth(), 640u);
        QCOMPARE(fakev4l2io::lastHeight(), 480u);
        QVERIFY(fakev4l2io::sawStreamOn());
    }
};

QTEST_MAIN(TestV4L2Camera)
#include "test_v4l2_camera.moc"
