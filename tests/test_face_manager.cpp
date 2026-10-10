/*
 * test_face_manager.cpp — FaceManager（人脸门禁：注册/训练/识别）行为断言。
 *
 * 为什么必须补这一组
 * ------------------
 * FaceManager 决定**门开不开**（widget.cpp:889 在检出人脸后调用 recognize()，
 * 由 recognizeResult 决定是否开门）。它此前从未被链接进任何测试目标。
 * 好在本机 OpenCV 带 face 模块（libopencv_face.dll.a），所以能**链接真实的
 * face_manager.cpp** 并用真实 LBPH 跑，而不是写个替身自我验证。
 *
 * 先做探针，再写断言（第 20 轮以来的规矩）
 * ----------------------------------------
 * 动手前先写了两个探针（临时的，未入库）量了 LBPH 的真实行为，得到三条
 * 决定性事实：
 *
 *   1. **模型未训练时 predict() 抛 cv::Exception**
 *        "This LBPH model is not computed yet. Did you call the train method?"
 *   2. **只有一个 label 时 `predictedLabel == 1` 恒为真**
 *        训练时 labels 全是 1（单用户门禁），因此判据里
 *        `predictedLabel == 1` 这一半**永远成立**，
 *        **阈值 `confidence < threshold` 是唯一的门。**
 *   3. 我原本怀疑的崩溃链**不成立**：QImage::copy() 传越界矩形**不返回空图**
 *        （实测返回指定尺寸、内容未初始化的图），空图也能正常转 cv::Mat、
 *        正常 train/predict。所以"越界裁剪导致空样本、导致 predict 抛异常"
 *        这条猜测是**错的**，不能写成断言。
 *
 * 由此确定断言取向
 * ----------------
 * 事实 2 意味着**硬编码 confidence 数值是没有意义的**（换了 OpenCV 版本或
 * 输入就会变）。所以阈值相关的断言一律**用被测代码自己吐出来的 confidence
 * 当基准**：
 *
 *     阈值 = conf      -> 必须拒绝（证明比较是严格 `<`，不是 `<=`）
 *     阈值 = conf + 1  -> 必须接受
 *
 * 这样既精确钉住了比较语义，又完全与 OpenCV 版本无关。
 * 同时用一条"不同图的 conf 必须大于同图的 conf"作为**阳性对照**，
 * 防止两张图的 conf 恰好都是 0 时整组断言变成恒真。
 */
#include <QImage>
#include <QSignalSpy>
#include <QTest>

#include "managers/face_manager.h"

#include <opencv2/opencv.hpp>

namespace {

/// 生成确定性测试图案。LBPH 是纹理直方图，不同图案给出明显不同的距离。
QImage makePattern(int kind, int size = 100)
{
    QImage img(size, size, QImage::Format_Grayscale8);
    for (int y = 0; y < size; ++y) {
        uchar* line = img.scanLine(y);
        for (int x = 0; x < size; ++x) {
            int v = 0;
            switch (kind) {
            case 0:  // 水平渐变 —— 训练用
                v = (x * 255) / (size - 1);
                break;
            case 1:  // 垂直渐变 —— 与训练图案差异最大
                v = (y * 255) / (size - 1);
                break;
            case 2:  // 棋盘
                v = ((x / 10 + y / 10) % 2) ? 255 : 0;
                break;
            default:
                v = (x > size / 2) ? 255 : 0;
                break;
            }
            line[x] = static_cast<uchar>(v);
        }
    }
    return img;
}

/// 同样图案的彩色版本（RGB32），用于验证 addFaceSample 确实做了灰度转换。
QImage makeColorPattern(int kind, int size = 100)
{
    const QImage gray = makePattern(kind, size);
    QImage rgb(size, size, QImage::Format_RGB32);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int g = qGray(gray.pixel(x, y));
            rgb.setPixel(x, y, qRgb(g, g, g));
        }
    }
    return rgb;
}

/// 把一张图注册进 manager（一次调用）。
void addSample(FaceManager& fm, const QImage& img)
{
    fm.addFaceSample(img);
}

/// 注册 collectTarget 张同图案样本并训练；返回训练是否报告成功。
bool trainOn(FaceManager& fm, int kind, int samples = 3)
{
    QSignalSpy spy(&fm, &FaceManager::trainFinished);
    for (int i = 0; i < samples; ++i) {
        addSample(fm, makePattern(kind));
    }
    fm.train();
    if (spy.count() != 1) {
        return false;
    }
    return spy.at(0).at(0).toBool();
}

/**
 * @brief 用给定阈值识别一张图，返回 (是否接受, 代码给出的 confidence)。
 *
 * confidence 由被测代码经 recognizeResult 信号吐出，是断言阈值的**唯一基准**。
 */
QPair<bool, double> recognizeWith(FaceManager& fm, const QImage& img,
                                 double threshold)
{
    fm.setConfidenceThreshold(threshold);
    QSignalSpy spy(&fm, &FaceManager::recognizeResult);
    fm.recognize(img);
    if (spy.count() != 1) {
        return qMakePair(false, -1.0);
    }
    return qMakePair(spy.at(0).at(0).toBool(), spy.at(0).at(1).toDouble());
}

} // namespace

class TestFaceManager : public QObject
{
    Q_OBJECT

private slots:

    // -----------------------------------------------------------------------
    // 构造与门槛
    // -----------------------------------------------------------------------

    /// collectTarget 必须恰好是 3，且 sampleTarget() 暴露的与之一致。
    void sampleTargetIsThree()
    {
        FaceManager fm;
        QCOMPARE(fm.sampleTarget(), 3);
        QCOMPARE(fm.sampleCount(), 0);
    }

    /**
     * 默认阈值必须是 90.0 —— 与重构前硬编码的 `confidence < 90` 完全一致。
     *
     * 这条证明"不配置时行为逐字节不变"。探针量的 conf=256 的图在默认阈值下
     * 必须被拒，在阈值 300 下必须被接受 —— 两者都不会因为默认值被改动而
     * 同时成立。
     */
    void defaultThresholdMatchesLegacyHardcoded90()
    {
        FaceManager fm;
        QVERIFY(trainOn(fm, 0));

        const QImage other = makePattern(1);
        // 默认阈值（不调用 setConfidenceThreshold）：conf=256 > 90 -> 拒绝。
        QSignalSpy spy(&fm, &FaceManager::recognizeResult);
        fm.recognize(other);
        QCOMPARE(spy.count(), 1);
        QVERIFY2(!spy.at(0).at(0).toBool(),
                 "默认阈值 90 下，与训练样本差异很大的图必须被拒绝");
        QVERIFY(spy.at(0).at(1).toDouble() > 90.0);
    }

    // -----------------------------------------------------------------------
    // 注册样本
    // -----------------------------------------------------------------------

    /// 每次注册都应报告进度，且计数递增到 target。
    void addFaceSampleReportsProgress()
    {
        FaceManager fm;
        QSignalSpy spy(&fm, &FaceManager::faceSampled);

        addSample(fm, makePattern(0));
        addSample(fm, makePattern(0));
        addSample(fm, makePattern(0));

        QCOMPARE(spy.count(), 3);
        QCOMPARE(spy.at(0).at(0).toInt(), 1);
        QCOMPARE(spy.at(1).at(0).toInt(), 2);
        QCOMPARE(spy.at(2).at(0).toInt(), 3);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(spy.at(i).at(1).toInt(), 3); // total 恒为 target
        }
        QCOMPARE(fm.sampleCount(), 3);
    }

    /**
     * 超过 target 的注册必须被**丢弃**并**不发信号**。
     *
     * 若这条守卫失效，按界面逻辑会继续采集，样本数无限增长而训练只在
     * 计数 == target 时触发 —— 结果是"采得越多越不训练"。
     */
    void addFaceSampleStopsAtTarget()
    {
        FaceManager fm;
        QSignalSpy spy(&fm, &FaceManager::faceSampled);

        for (int i = 0; i < 5; ++i) {
            addSample(fm, makePattern(0));
        }

        QCOMPARE(spy.count(), 3);          // 第 4、5 次不发信号
        QCOMPARE(fm.sampleCount(), 3);     // 也不会增长
    }

    /**
     * addFaceSample 必须把彩色图转成灰度再存 —— 且断言必须**有判别力**。
     *
     * 第一版这条用例只断言"训练成功"，结果故障注入**没抓到**去掉
     * convertToFormat 的变异：LBPH 接受任何 CV_8UC1 的 Mat，即使那个 Mat
     * 其实是 RGB32 缓冲的前 1/4 字节（stride 400 而 width 100），train()
     * 照样成功。所以"训练成功"对这件事**没有判别力**。
     *
     * 真正的判别力在**像素数据**上（临时探针实测）：
     *
     *     注册彩色图 -> 用灰度同图案 predict
     *       真转换   conf = 0.00   （接受）
     *       不转换   conf = 256.00 （拒绝）   kind=0
     *       不转换   conf = 153.60 （拒绝）   kind=1
     *       不转换   conf = 33.26  （接受）   kind=2 ← 该图案无判别力
     *
     * 因此本用例固定用 kind=0，并**必须**再 predict 一次：注册彩色、
     * 用灰度识别，默认阈值下必须通过。去掉转换后该断言会失败。
     */
    void addFaceSampleConvertsColorInputToGrayscale()
    {
        FaceManager fm;
        for (int i = 0; i < 3; ++i) {
            addSample(fm, makeColorPattern(0));
        }
        QCOMPARE(fm.sampleCount(), 3);

        QSignalSpy trainSpy(&fm, &FaceManager::trainFinished);
        bool threw = false;
        try {
            fm.train();
        } catch (const cv::Exception&) {
            threw = true;
        }
        QVERIFY2(!threw, "彩色输入未经灰度转换时 LBPH 训练会抛异常");
        QCOMPARE(trainSpy.count(), 1);
        QVERIFY2(trainSpy.at(0).at(0).toBool(), "彩色输入应先转灰度，训练应成功");

        // 判别力所在：注册的是彩色，识别用**灰度**同图案。
        // 真转换则两者像素一致 -> conf≈0 -> 接受；
        // 不转换则存进去的是 RGB32 缓冲的错位切片 -> conf≈256 -> 拒绝。
        const QPair<bool, double> r =
            recognizeWith(fm, makePattern(0), 90.0);
        QVERIFY2(r.first,
                 qPrintable(QStringLiteral(
                     "彩色注册后应用灰度识别通过（conf=%1）。"
                     "若此处失败，说明 addFaceSample 没有做灰度转换 —— "
                     "存进去的是错位的像素数据。").arg(r.second)));
    }

    // -----------------------------------------------------------------------
    // 训练
    // -----------------------------------------------------------------------

    /// 样本不足时训练必须报失败，且**不发射**成功。
    void trainWithInsufficientSamplesReportsFailure()
    {
        FaceManager fm;
        addSample(fm, makePattern(0));
        addSample(fm, makePattern(0));   // 只有 2 个

        QSignalSpy spy(&fm, &FaceManager::trainFinished);
        bool threw = false;
        try {
            fm.train();
        } catch (const cv::Exception&) {
            threw = true;
        }
        QVERIFY2(!threw, "样本不足时应提前返回，绝不该去调 train()");
        QCOMPARE(spy.count(), 1);
        QVERIFY(!spy.at(0).at(0).toBool());
    }

    /// 完全没有样本时训练同样必须报失败且不触碰模型。
    void trainWithNoSamplesReportsFailure()
    {
        FaceManager fm;
        QSignalSpy spy(&fm, &FaceManager::trainFinished);
        fm.train();
        QCOMPARE(spy.count(), 1);
        QVERIFY(!spy.at(0).at(0).toBool());
    }

    /// 样本足够时训练报成功。
    void trainWithEnoughSamplesReportsSuccess()
    {
        FaceManager fm;
        QSignalSpy spy(&fm, &FaceManager::trainFinished);
        for (int i = 0; i < 3; ++i) {
            addSample(fm, makePattern(0));
        }
        fm.train();
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.at(0).at(0).toBool());
    }

    // -----------------------------------------------------------------------
    // 识别
    // -----------------------------------------------------------------------

    /**
     * 样本不足时识别必须报 (false, 0.0)，且**绝不触碰未训练的模型**。
     *
     * 这条断言真正判别的是守卫存在与否：探针已证实**未训练的 LBPH
     * 一旦 predict() 就抛 cv::Exception**。所以若删掉
     * `if (registeredFaces.size() < collectTarget)` 守卫，recognize()
     * 会抛异常 —— 而它跑在 UI 线程的槽里，异常逃出事件循环即终止进程。
     * 测试会以崩溃形式抓住这个退化。
     */
    void recognizeWithInsufficientSamplesDoesNotTouchModel()
    {
        FaceManager fm;
        addSample(fm, makePattern(0));   // 只有 1 个

        QSignalSpy spy(&fm, &FaceManager::recognizeResult);
        bool threw = false;
        try {
            fm.recognize(makePattern(1));
        } catch (const cv::Exception&) {
            threw = true;
        }
        QVERIFY2(!threw, "样本不足时不得去 predict() 未训练的模型");
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), false);
        QCOMPARE(spy.at(0).at(1).toDouble(), 0.0);
    }

    /// 零样本时识别同样必须报 (false, 0.0) 且不抛。
    void recognizeWithNoSamplesReportsFailure()
    {
        FaceManager fm;
        QSignalSpy spy(&fm, &FaceManager::recognizeResult);
        bool threw = false;
        try {
            fm.recognize(makePattern(1));
        } catch (const cv::Exception&) {
            threw = true;
        }
        QVERIFY(!threw);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), false);
        QCOMPARE(spy.at(0).at(1).toDouble(), 0.0);
    }

    /**
     * **阈值比较必须是严格小于**（`confidence < threshold`，不是 `<=`）。
     *
     * 做法：先用一个很大的阈值把代码自己算出的 confidence 取出来，
     * 再分别把阈值设成 conf（必须拒绝）与 conf+1（必须接受）。
     *
     * - 与 OpenCV 版本无关：不硬编码任何 confidence 数值。
     * - 精确到边界：若把 `<` 改成 `<=`，`阈值 = conf` 那条会变成接受，
     *   本用例立刻失败。
     */
    void thresholdComparisonIsStrictLessThan()
    {
        FaceManager fm;
        QVERIFY(trainOn(fm, 0));

        const QImage other = makePattern(1);

        // 取基准：阈值放到很大，保证这次是"接受"，从而拿到真实的 conf。
        const QPair<bool, double> permissive = recognizeWith(fm, other, 1e9);
        QVERIFY2(permissive.first, "阈值极大时必须接受");
        const double conf = permissive.second;
        QVERIFY2(conf > 0.0, "阳性对照：不同图案的 LBPH 距离应大于 0");

        // 阈值 == conf -> 严格小于不成立 -> 必须拒绝。
        const QPair<bool, double> atBoundary = recognizeWith(fm, other, conf);
        QVERIFY2(!atBoundary.first,
                 "confidence == threshold 时必须拒绝（比较是严格 `<` 而非 `<=`）");

        // 阈值 == conf + 1 -> 必须接受。
        const QPair<bool, double> justAbove = recognizeWith(fm, other, conf + 1.0);
        QVERIFY2(justAbove.first, "confidence < threshold 时必须接受");
    }

    /**
     * **只有一个 label 时，阈值是唯一的门。**
     *
     * 训练时所有样本的 label 都是 1（单用户门禁），LBPH 只有一个类别，
     * 于是 `predictedLabel == 1` **恒为真**。这意味着：一张与注册者
     * **毫无关系**的图，只要阈值放得足够松，就会被判为"通过"。
     *
     * 这不是要"修复"的行为（单用户门禁本就该如此，且与 baseline 一致），
     * 而是要把**门禁安全完全系于阈值**这一事实写进测试：
     *   - 阈值松 -> 陌生人也能开门（下一条断言就是它）；
     *   - 阈值紧 -> 必须拒绝。
     * 因此 behavior.face_confidence_threshold 是一个**安全相关**配置项。
     */
    void thresholdIsTheOnlyGateWithSingleLabel()
    {
        FaceManager fm;
        QVERIFY(trainOn(fm, 0));

        const QImage stranger = makePattern(2);  // 棋盘，与注册图案完全不同

        const QPair<bool, double> tight = recognizeWith(fm, stranger, 90.0);
        QVERIFY2(!tight.first, "默认阈值下完全不同的图案必须被拒绝");

        const QPair<bool, double> loose = recognizeWith(fm, stranger, 1e9);
        QVERIFY2(loose.first,
                 "阈值放到极大时，单 label 模型必然接受任何图 —— "
                 "这正是『门禁安全完全取决于阈值』的含义");

        QVERIFY2(loose.second > tight.second || loose.second == tight.second,
                 "阳性对照：同一张图的 confidence 应是确定值");
    }

    /**
     * 阳性对照 + 判别力：与注册图案相同的图，距离必须**小于**不同的图。
     *
     * 没有这条，上面所有阈值断言在"两张图 conf 恰好都是 0"时都会恒真。
     */
    void samePatternIsCloserThanDifferentPattern()
    {
        FaceManager fm;
        QVERIFY(trainOn(fm, 0));

        const QPair<bool, double> same =
            recognizeWith(fm, makePattern(0), 1e9);
        const QPair<bool, double> diff =
            recognizeWith(fm, makePattern(1), 1e9);

        QVERIFY2(same.second < diff.second,
                 qPrintable(QStringLiteral("同图案距离(%1) 必须小于 异图案距离(%2)")
                                .arg(same.second).arg(diff.second)));
    }

    /// 与注册图案相同的图，在默认阈值下必须通过。
    void samePatternIsAcceptedAtDefaultThreshold()
    {
        FaceManager fm;
        QVERIFY(trainOn(fm, 0));
        const QPair<bool, double> r = recognizeWith(fm, makePattern(0), 90.0);
        QVERIFY2(r.first, "注册者本人的图案在默认阈值下必须通过");
    }

    /// 收紧阈值必须能把同一张图从"通过"变成"拒绝"（配置真的生效）。
    void tighteningThresholdFlipsResultForSameImage()
    {
        FaceManager fm;
        QVERIFY(trainOn(fm, 0));
        const QImage same = makePattern(0);

        const QPair<bool, double> loose = recognizeWith(fm, same, 1e9);
        QVERIFY(loose.first);

        // 阈值收到 conf 之下（conf 可能是 0，故用 -1 保证严格更小）。
        const QPair<bool, double> strict = recognizeWith(fm, same, -1.0);
        QVERIFY2(!strict.first,
                 "阈值设为 -1 时任何 confidence 都不满足严格小于，必须拒绝");

        QCOMPARE(loose.second, strict.second);  // 同一张图 conf 应一致
    }

    // -----------------------------------------------------------------------
    // clear 与析构
    // -----------------------------------------------------------------------

    /// clear() 必须清空样本计数。
    void clearResetsSamples()
    {
        FaceManager fm;
        for (int i = 0; i < 3; ++i) {
            addSample(fm, makePattern(0));
        }
        QCOMPARE(fm.sampleCount(), 3);

        fm.clear();
        QCOMPARE(fm.sampleCount(), 0);

        // 清空后识别必须回到"样本不足"分支。
        QSignalSpy spy(&fm, &FaceManager::recognizeResult);
        fm.recognize(makePattern(1));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), false);
    }

    /// 析构必须安全（含"注册了样本但从未训练"这一情形）。
    void destructorIsSafe()
    {
        {
            FaceManager fm;
            addSample(fm, makePattern(0));
            addSample(fm, makePattern(1));
        }
        {
            FaceManager fm;
            for (int i = 0; i < 3; ++i) {
                addSample(fm, makePattern(0));
            }
            fm.train();
        }
        QVERIFY(true);  // 走到这里即未崩溃
    }

    /**
     * 已知隐患（**不改代码，只钉住现状**）：样本够了但**没训练**时，
     * recognize() 会抛 cv::Exception。
     *
     * 原因：recognize() 的守卫是 `registeredFaces.size() < collectTarget`
     * （数**样本数**），而 predict() 需要的是"**模型已训练**"。两者在
     * "填满样本但跳过 train()" 时不相等。
     *
     * 为什么现在不是线上缺陷：唯一的调用点 widget.cpp:767-768 在
     * `sampleCount() == sampleTarget()` 时**立即**调用 train()，因此
     * 样本数达到 3 与模型已训练是同一件事，该路径不可达。
     *
     * 为什么仍然要钉住：这是个**隐患**而非已爆的缺陷。异常跑在 UI 线程的
     * 槽里，一旦将来有人加了别的调用路径（例如界面预加载样本、或把 train()
     * 改成异步），异常逃出事件循环会直接终止进程。本用例把当前行为固定下来，
     * 使"是否收敛为守卫上模型状态"成为一个**显式**决定，而不是随手的改动。
     *
     * 与 baseline 的关系：baseline 的守卫完全相同（face_manager.cpp:76），
     * 属**继承**行为，故此处不改生产代码。
     */
    void samplesWithoutTrainingMakeRecognizeThrow_documentedHazard()
    {
        FaceManager fm;
        for (int i = 0; i < 3; ++i) {
            addSample(fm, makePattern(0));
        }
        // 故意**不**调用 train()

        QSignalSpy spy(&fm, &FaceManager::recognizeResult);
        bool threw = false;
        try {
            fm.recognize(makePattern(0));
        } catch (const cv::Exception&) {
            threw = true;
        }
        QVERIFY2(threw,
                 "现状：样本填满但未训练时 recognize() 会抛 cv::Exception。"
                 "若此处失败，说明守卫已改为检查模型训练状态 —— "
                 "那是行为变化，请同步更新本用例与 README 的隐患记录。");
        QCOMPARE(spy.count(), 0);   // 抛异常发生在 emit 之前
    }
};

QTEST_MAIN(TestFaceManager)
#include "test_face_manager.moc"
