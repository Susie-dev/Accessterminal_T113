/**
 * @file test_face_quality.cpp
 * @brief FaceQuality（人脸质量门）的行为断言 —— 方案 P4 的防假阳性基础。
 *
 * 为什么必须有这一组
 * ------------------
 * 本工程已实测确认：LBPH 在单 label 下 `predictedLabel == 1` **恒为真**，
 * 于是距离阈值是**唯一的门**（见 tests/test_face_manager.cpp 的结论）。这意味着
 * 一张模糊、过曝或过暗的图一旦凑巧落进阈值，**门就开了** —— 是安全问题。
 * 质量门是挡在识别之前的第二道判断，它自己错了不会有人发现（只会表现为
 * "偶尔误开"或"老是识别不了"），因此必须逐项钉住。
 *
 * 本目标**不依赖 OpenCV**（与方案文档原计划不同）
 * ----------------------------------------------
 * 方案原写"按本项目惯例缺 OpenCV 时跳过测试"。实现时发现清晰度用四邻域
 * 卷积只需十几行纯 C++，于是改成纯 Qt 实现 —— 于是本目标在**任何**环境
 * 都真的跑。理由正是 README §78：会静默跳过、或安静地少跑一条的检查，
 * 比没有检查更糟。因此这里**没有** find_package(OpenCV) 分支。
 *
 * 断言策略
 * --------
 *   - 正向能力（能分辨清晰/模糊、亮/暗、大/小脸）用**构造出来的确定性图像**：
 *     均匀图 -> 清晰度应为 0；棋盘图 -> 清晰度应显著 > 0。不依赖任何
 *     "经验阈值"，只依赖数学事实。
 *   - 「默认不限制」是**保持现状的承诺**，必须有断言（否则质量门一上线
 *     就可能把正常用户挡在门外）。
 *   - **行首必须用 bytesPerLine() 而非 width()** —— QImage 扫描线按 4 字节
 *     对齐，宽度非 4 的倍数时两者不等。这条有专门用例：构造 641 宽（填充
 *     3 字节）的图，把填充字节涂成哨兵值 255，内容全是 100；用 width() 的
 *     实现会读到哨兵值，指标立刻偏离。640 宽恰好对齐，随手测的尺寸
 *     发现不了这个 bug。
 */
#include <QtTest/QtTest>

#include <QImage>

#include "managers/face_quality.h"

using accessterminal::FaceQuality;
using accessterminal::FaceQualityReport;
using accessterminal::FaceQualityThresholds;

namespace {

/// 均匀灰度图（清晰度应为 0，对比度应为 0）。
QImage uniformImage(int w, int h, int value)
{
    QImage img(w, h, QImage::Format_Grayscale8);
    img.fill(static_cast<uint>(value));
    return img;
}

/// 棋盘图：黑白交替，Laplacian 响应大 -> 清晰度显著大于 0。
QImage checkerImage(int w, int h, int cell = 4)
{
    QImage img(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar* row = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            const bool on = ((x / cell) + (y / cell)) % 2 == 0;
            row[x] = on ? 0 : 255;
        }
    }
    return img;
}

} // namespace

class TestFaceQuality : public QObject
{
    Q_OBJECT

private slots:

    // ---- 1. 默认阈值必须**不限制任何东西**（保持现状的承诺）----

    void defaultThresholdsAcceptEverything()
    {
        const FaceQualityThresholds d;
        QCOMPARE(d.minSharpness, 0.0);
        QCOMPARE(d.minBrightness, 0.0);
        QCOMPARE(d.maxBrightness, 255.0);
        QCOMPARE(d.minContrast, 0.0);
        QCOMPARE(d.minAreaRatio, 0.0);

        // 一张全黑、模糊、只占很小区域的图，在默认阈值下也必须通过 ——
        // 否则"不配置时行为逐字节不变"这句话就是假的。
        const QImage tiny = uniformImage(8, 8, 0);
        const QRect face(0, 0, 4, 4);
        QVERIFY2(FaceQuality::passes(tiny, face),
                 "默认阈值下任何合法输入都必须通过（不配置 = 保持现状）");

        const FaceQualityReport r = FaceQuality::evaluate(tiny, face);
        QCOMPARE(r.reason, static_cast<int>(FaceQualityReport::Ok));
        QVERIFY(r.acceptable);
    }

    // ---- 2. 清晰度：能区分清晰与模糊（数学事实，不依赖经验阈值）----

    void sharpnessSeparatesCheckerboardFromUniform()
    {
        const QImage checker = checkerImage(64, 64);
        const QImage flat = uniformImage(64, 64, 128);
        const QRect whole(0, 0, 64, 64);

        const FaceQualityReport rc = FaceQuality::evaluate(checker, whole);
        const FaceQualityReport rf = FaceQuality::evaluate(flat, whole);

        QCOMPARE(rf.sharpness, 0.0);            // 均匀图 Laplacian 恒为 0
        QVERIFY2(rc.sharpness > 0.0,
                 qPrintable(QStringLiteral("棋盘图清晰度应 > 0，实测 %1")
                                .arg(rc.sharpness)));
        QVERIFY2(rc.sharpness > rf.sharpness,
                 "棋盘图的清晰度必须高于均匀图");

        // 正向能力：阈值设在两者之间必须能分开。
        FaceQualityThresholds t;
        t.minSharpness = rc.sharpness / 2.0;
        QVERIFY2(FaceQuality::passes(checker, whole, t), "清晰图应通过");
        QVERIFY2(!FaceQuality::passes(flat, whole, t), "模糊图应被拒");
        QCOMPARE(FaceQuality::evaluate(flat, whole, t).reason,
                 static_cast<int>(FaceQualityReport::TooBlurry));
    }

    // ---- 3. 模糊程度单调：越糊清晰度越低 ----

    void blurrierMeansLowerSharpness()
    {
        const QRect whole(0, 0, 64, 64);
        const double s4 = FaceQuality::evaluate(checkerImage(64, 64, 2), whole).sharpness;
        const double s8 = FaceQuality::evaluate(checkerImage(64, 64, 4), whole).sharpness;
        const double s16 = FaceQuality::evaluate(checkerImage(64, 64, 16), whole).sharpness;
        QVERIFY2(s4 > s8, qPrintable(QStringLiteral("%1 应 > %2").arg(s4).arg(s8)));
        QVERIFY2(s8 > s16, qPrintable(QStringLiteral("%1 应 > %2").arg(s8).arg(s16)));
    }

    // ---- 4. 亮度：均值正确，且过暗/过亮分别判对原因 ----

    void brightnessIsMeasuredAndGated()
    {
        const QRect whole(0, 0, 32, 32);

        const FaceQualityReport dark = FaceQuality::evaluate(uniformImage(32, 32, 10), whole);
        QCOMPARE(dark.brightness, 10.0);

        const FaceQualityReport bright = FaceQuality::evaluate(uniformImage(32, 32, 250), whole);
        QCOMPARE(bright.brightness, 250.0);

        FaceQualityThresholds t;
        t.minBrightness = 40.0;
        t.maxBrightness = 220.0;

        QCOMPARE(FaceQuality::evaluate(uniformImage(32, 32, 10), whole, t).reason,
                 static_cast<int>(FaceQualityReport::TooDark));
        QCOMPARE(FaceQuality::evaluate(uniformImage(32, 32, 250), whole, t).reason,
                 static_cast<int>(FaceQualityReport::TooBright));
        QVERIFY2(FaceQuality::passes(uniformImage(32, 32, 128), whole, t),
                 "中间亮度必须通过");
    }

    // ---- 5. 对比度：标准差正确，低对比度被拒 ----

    void contrastIsMeasuredAndGated()
    {
        const QRect whole(0, 0, 32, 32);

        // 均匀图标准差为 0。
        QCOMPARE(FaceQuality::evaluate(uniformImage(32, 32, 77), whole).contrast, 0.0);

        // 一半 0、一半 255 的图：均值 127.5，总体标准差也是 127.5。
        QImage half(32, 32, QImage::Format_Grayscale8);
        for (int y = 0; y < 32; ++y) {
            uchar* row = half.scanLine(y);
            for (int x = 0; x < 32; ++x) {
                row[x] = (y < 16) ? 0 : 255;
            }
        }
        const FaceQualityReport rh = FaceQuality::evaluate(half, whole);
        QCOMPARE(rh.brightness, 127.5);
        QVERIFY2(qAbs(rh.contrast - 127.5) < 0.001,
                 qPrintable(QStringLiteral("对比度应为 127.5，实测 %1").arg(rh.contrast)));

        FaceQualityThresholds t;
        t.minContrast = 10.0;
        QCOMPARE(FaceQuality::evaluate(uniformImage(32, 32, 128), whole, t).reason,
                 static_cast<int>(FaceQualityReport::TooFlat));
        QVERIFY2(FaceQuality::passes(half, whole, t), "高对比度图应通过");
    }

    // ---- 6. 面积占比：越界框按图像求交，占比正确 ----

    void areaRatioUsesIntersection()
    {
        const QSize img(100, 100);

        QCOMPARE(FaceQuality::clampedRegion(img, QRect(0, 0, 50, 50)),
                 QRect(0, 0, 50, 50));
        // 越界部分被裁掉：从 (90,90) 起 30x30 -> 实际 10x10
        QCOMPARE(FaceQuality::clampedRegion(img, QRect(90, 90, 30, 30)),
                 QRect(90, 90, 10, 10));
        // 完全在图像外 -> 空
        QVERIFY(FaceQuality::clampedRegion(img, QRect(200, 200, 10, 10)).isEmpty());
        // 负起点
        QCOMPARE(FaceQuality::clampedRegion(img, QRect(-10, -10, 30, 30)),
                 QRect(0, 0, 20, 20));

        const FaceQualityReport r =
            FaceQuality::evaluate(uniformImage(100, 100, 128), QRect(0, 0, 50, 50));
        QVERIFY2(qAbs(r.areaRatio - 0.25) < 1e-9,
                 qPrintable(QStringLiteral("占比应为 0.25，实测 %1").arg(r.areaRatio)));
    }

    void smallFaceIsRejectedByAreaRatio()
    {
        const QImage img = uniformImage(100, 100, 128);
        FaceQualityThresholds t;
        t.minAreaRatio = 0.10;

        QVERIFY2(FaceQuality::passes(img, QRect(0, 0, 40, 40), t), "16% 应通过");
        QCOMPARE(FaceQuality::evaluate(img, QRect(0, 0, 20, 20), t).reason,
                 static_cast<int>(FaceQualityReport::TooSmall));
    }

    // ---- 7. 越界框不得崩溃，且原因明确 ----

    void outOfBoundsRectIsSafe()
    {
        const QImage img = uniformImage(64, 64, 128);

        const FaceQualityReport r =
            FaceQuality::evaluate(img, QRect(500, 500, 10, 10));
        QVERIFY2(!r.acceptable, "完全在图像外的框不得判为通过");
        QCOMPARE(r.reason, static_cast<int>(FaceQualityReport::TooSmall));

        // 只读评估，图像内容不得被改动。
        QVERIFY(FaceQuality::clampedRegion(QSize(64, 64), QRect(60, 60, 100, 100))
                == QRect(60, 60, 4, 4));
    }

    // ---- 8. 空输入一律安全（不抛异常、不除零）----

    void emptyInputsAreSafe()
    {
        const QImage null;
        const FaceQualityReport r = FaceQuality::evaluate(null, QRect(0, 0, 10, 10));
        QVERIFY(!r.acceptable);
        QCOMPARE(r.reason, static_cast<int>(FaceQualityReport::EmptyInput));

        QCOMPARE(FaceQuality::evaluate(QImage(), QRect()).reason,
                 static_cast<int>(FaceQualityReport::EmptyInput));

        // 合法图配空框 / 零尺寸框
        const QImage ok = uniformImage(16, 16, 128);
        QCOMPARE(FaceQuality::evaluate(ok, QRect(0, 0, 0, 0)).reason,
                 static_cast<int>(FaceQualityReport::TooSmall));
        QCOMPARE(FaceQuality::evaluate(ok, QRect(4, 4, -2, 5)).reason,
                 static_cast<int>(FaceQualityReport::TooSmall));

        // 零尺寸图像
        QCOMPARE(FaceQuality::evaluate(QImage(0, 0, QImage::Format_Grayscale8),
                                       QRect(0, 0, 5, 5)).reason,
                 static_cast<int>(FaceQualityReport::EmptyInput));
    }

    // ---- 9. 只读：评估不得改动入参图像 ----

    void evaluateDoesNotMutateInput()
    {
        const QImage img = checkerImage(32, 32);
        const QImage before = img.copy();
        const QRect whole(0, 0, 32, 32);

        FaceQuality::evaluate(img, whole);
        QCOMPARE(img, before);

        // 彩色图走转换路径，同样不得改动原图。
        //
        // 这里**不写魔法数字**：Qt 的灰度换算系数属实现细节（实测 Qt::red
        // 在 Qt 5.15 上换成 87，红/绿/蓝权重并非我最初假设的 76）。改为与
        // `QImage::convertToFormat()` 的**独立结果**比对 —— 那正是本类声明的
        // 契约（"与 FaceManager 的预处理一致"），且不依赖版本常量。
        QImage rgb(32, 32, QImage::Format_RGB32);
        rgb.fill(Qt::red);
        const QImage rgbBefore = rgb.copy();
        const double expected =
            FaceQuality::evaluate(rgb.convertToFormat(QImage::Format_Grayscale8),
                                  whole).brightness;
        QVERIFY2(qAbs(FaceQuality::evaluate(rgb, whole).brightness - expected) < 1e-9,
                 "彩色图必须与显式转灰度的结果一致");
        QCOMPARE(rgb, rgbBefore);   // 入参不得被改动
    }

    // ---- 10. 彩色图与灰度图结论一致（预处理口径与 FaceManager 相同）----

    void colorImageIsConvertedLikeFaceManager()
    {
        const QRect whole(0, 0, 32, 32);
        const QImage gray = uniformImage(32, 32, 100);

        QImage rgb(32, 32, QImage::Format_RGB32);
        rgb.fill(QColor(100, 100, 100));

        const double a = FaceQuality::evaluate(gray, whole).brightness;
        const double b = FaceQuality::evaluate(rgb, whole).brightness;
        QVERIFY2(qAbs(a - b) < 1.0,
                 qPrintable(QStringLiteral("灰度 %1 与等效彩色 %2 应一致").arg(a).arg(b)));
    }

    // ---- 11. **对齐陷阱**：行首必须用 bytesPerLine()，不能用 width() ----

    void unalignedWidthDoesNotLeakRowPadding()
    {
        // 宽度 641 -> QImage 扫描线按 4 字节对齐 -> bytesPerLine = 644，
        // 每行末尾有 3 个**不属于图像内容**的填充字节。
        // 640 宽恰好对齐，所以这个 bug 在随手取整尺寸下完全看不出来。
        const int w = 641;
        const int h = 16;
        QImage img(w, h, QImage::Format_Grayscale8);
        QVERIFY2(img.bytesPerLine() > w,
                 "本用例的前提是填充存在：bytesPerLine() 必须大于 width()");

        // 内容全部 100，填充字节全部涂成哨兵值 255。
        for (int y = 0; y < h; ++y) {
            uchar* row = img.scanLine(y);
            for (int x = 0; x < w; ++x) {
                row[x] = 100;
            }
            for (int x = w; x < img.bytesPerLine(); ++x) {
                row[x] = 255;   // 哨兵：正确实现永远不该读到它
            }
        }

        const FaceQualityReport r =
            FaceQuality::evaluate(img, QRect(0, 0, w, h));

        // 正确实现只看内容像素：均值恰为 100、对比度为 0、清晰度为 0。
        QVERIFY2(qAbs(r.brightness - 100.0) < 1e-9,
                 qPrintable(QStringLiteral(
                     "亮度应为 100，实测 %1 —— 若为 100.x/101 说明行首用了 "
                     "width() 而非 bytesPerLine()，读到了行尾填充")
                     .arg(r.brightness, 0, 'f', 6)));
        QVERIFY2(r.contrast < 1e-9,
                 qPrintable(QStringLiteral("对比度应为 0，实测 %1").arg(r.contrast)));
        QVERIFY2(r.sharpness < 1e-9,
                 qPrintable(QStringLiteral("清晰度应为 0，实测 %1").arg(r.sharpness)));

        // 阳性对照：同一张图在非对齐宽度下，清晰度门仍然能正确判定棋盘图，
        // 说明上面的"恰好为 0"不是因为根本没算。
        QImage checker(w, h, QImage::Format_Grayscale8);
        for (int y = 0; y < h; ++y) {
            uchar* row = checker.scanLine(y);
            for (int x = 0; x < w; ++x) {
                row[x] = ((x / 4) + (y / 4)) % 2 == 0 ? 0 : 255;
            }
            for (int x = w; x < checker.bytesPerLine(); ++x) {
                row[x] = 255;
            }
        }
        QVERIFY2(FaceQuality::evaluate(checker, QRect(0, 0, w, h)).sharpness > 0.0,
                 "非对齐宽度下棋盘图仍必须有清晰度（正向对照）");
    }

    // ---- 12. 非对齐宽度下限定区域也必须正确 ----

    void unalignedWidthWithSubRegion()
    {
        const int w = 643;   // 643 % 4 == 3 -> 1 个填充字节
        const int h = 20;
        QImage img(w, h, QImage::Format_Grayscale8);
        QVERIFY(img.bytesPerLine() > w);
        for (int y = 0; y < h; ++y) {
            uchar* row = img.scanLine(y);
            for (int x = 0; x < w; ++x) {
                row[x] = 60;
            }
            for (int x = w; x < img.bytesPerLine(); ++x) {
                row[x] = 0;   // 与内容不同即可
            }
        }
        const FaceQualityReport r =
            FaceQuality::evaluate(img, QRect(5, 5, 30, 10));
        QVERIFY2(qAbs(r.brightness - 60.0) < 1e-9,
                 qPrintable(QStringLiteral("子区域亮度应为 60，实测 %1")
                                .arg(r.brightness, 0, 'f', 6)));
        QVERIFY(r.sharpness < 1e-9);
    }

    // ---- 13. 原因文本完备（枚举扩展后不会落空）----

    void reasonTextIsComplete()
    {
        using accessterminal::faceQualityReasonText;
        QVERIFY(QString::fromUtf8(faceQualityReasonText(FaceQualityReport::Ok))
                != QString());
        QVERIFY(QString::fromUtf8(faceQualityReasonText(FaceQualityReport::EmptyInput))
                != QString());
        QVERIFY(QString::fromUtf8(faceQualityReasonText(FaceQualityReport::TooSmall))
                != QString());
        QVERIFY(QString::fromUtf8(faceQualityReasonText(FaceQualityReport::TooDark))
                != QString());
        QVERIFY(QString::fromUtf8(faceQualityReasonText(FaceQualityReport::TooBright))
                != QString());
        QVERIFY(QString::fromUtf8(faceQualityReasonText(FaceQualityReport::TooFlat))
                != QString());
        QVERIFY(QString::fromUtf8(faceQualityReasonText(FaceQualityReport::TooBlurry))
                != QString());

        // 每个枚举值都有**互不相同**的文本，否则日志分不清哪种拒绝。
        const char* texts[] = {
            faceQualityReasonText(FaceQualityReport::Ok),
            faceQualityReasonText(FaceQualityReport::EmptyInput),
            faceQualityReasonText(FaceQualityReport::TooSmall),
            faceQualityReasonText(FaceQualityReport::TooDark),
            faceQualityReasonText(FaceQualityReport::TooBright),
            faceQualityReasonText(FaceQualityReport::TooFlat),
            faceQualityReasonText(FaceQualityReport::TooBlurry),
        };
        for (int i = 0; i < 7; ++i) {
            for (int j = i + 1; j < 7; ++j) {
                QVERIFY2(QString::fromUtf8(texts[i]) != QString::fromUtf8(texts[j]),
                         "不同原因的文本必须互不相同");
            }
        }
    }

    // ---- 14. describe() 不落空且含指标（现场标定要用）----

    void describeContainsMetrics()
    {
        const QImage img = uniformImage(32, 32, 200);
        const FaceQualityReport r = FaceQuality::evaluate(img, QRect(0, 0, 32, 32));
        const QString s = r.describe();
        QVERIFY(!s.isEmpty());
        QVERIFY2(s.contains(QStringLiteral("清晰度")),
                 qPrintable(QStringLiteral("应含指标，实测: %1").arg(s)));
        QVERIFY(s.contains(QStringLiteral("亮度")));
        QVERIFY(s.contains(QStringLiteral("对比度")));
        QVERIFY(s.contains(QStringLiteral("占比")));
    }

    // ---- 15. 所有门一起开：能同时正确判定四项 ----

    void allGatesWorkTogether()
    {
        const QImage img = checkerImage(64, 64, 4);
        const QRect whole(0, 0, 64, 64);

        FaceQualityThresholds t;
        t.minBrightness = 40.0;
        t.maxBrightness = 220.0;
        t.minContrast = 5.0;
        t.minAreaRatio = 0.10;
        t.minSharpness = 10.0;

        // 棋盘图：均值 127.5（在区间内）、对比度 127.5、占比 1.0、清晰度高
        QVERIFY2(FaceQuality::passes(img, whole, t),
                 "四项都达标的图必须通过");
    }
};

QTEST_MAIN(TestFaceQuality)
#include "test_face_quality.moc"
