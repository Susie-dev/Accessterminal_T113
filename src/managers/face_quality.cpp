/**
 * @file face_quality.cpp
 * @brief 人脸图像质量门的实现：纯像素统计，不依赖 OpenCV。
 */
#include "managers/face_quality.h"

#include <QString>

#include <cmath>

namespace accessterminal {

namespace {

/// 把任意格式的图像转成 8 位灰度。**不做原地修改**（QImage 是隐式共享的）。
QImage toGray(const QImage& image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return QImage();
    }
    if (image.format() == QImage::Format_Grayscale8) {
        return image;   // 已经是目标格式，零拷贝（隐式共享）
    }
    // 统一转换路径：与 FaceManager 的预处理保持一致（LBPH 用灰度纹理）。
    return image.convertToFormat(QImage::Format_Grayscale8);
}

} // namespace

const char* faceQualityReasonText(FaceQualityReport::Reason reason)
{
    switch (reason) {
    case FaceQualityReport::Ok:         return "通过";
    case FaceQualityReport::EmptyInput: return "输入为空";
    case FaceQualityReport::TooSmall:   return "人脸区域过小";
    case FaceQualityReport::TooDark:    return "亮度过低";
    case FaceQualityReport::TooBright:  return "亮度过高";
    case FaceQualityReport::TooFlat:    return "对比度过低";
    case FaceQualityReport::TooBlurry:  return "清晰度过低";
    }
    // 枚举被扩展却没在这里补分支时，返回一个明确的值而不是落空。
    return "未知原因";
}

QString FaceQualityReport::describe() const
{
    return QStringLiteral("%1 (清晰度=%2 亮度=%3 对比度=%4 占比=%5)")
        .arg(QString::fromUtf8(faceQualityReasonText(reason)))
        .arg(sharpness, 0, 'f', 2)
        .arg(brightness, 0, 'f', 2)
        .arg(contrast, 0, 'f', 2)
        .arg(areaRatio, 0, 'f', 4);
}

QRect FaceQuality::clampedRegion(const QSize& imageSize, const QRect& faceRect)
{
    if (imageSize.width() <= 0 || imageSize.height() <= 0) {
        return QRect();
    }
    if (faceRect.width() <= 0 || faceRect.height() <= 0) {
        return QRect();
    }
    return faceRect.intersected(QRect(0, 0, imageSize.width(), imageSize.height()));
}

FaceQualityReport FaceQuality::evaluate(const QImage& image,
                                        const QRect& faceRect,
                                        const FaceQualityThresholds& t)
{
    FaceQualityReport r;

    const QImage gray = toGray(image);
    if (gray.isNull()) {
        r.reason = FaceQualityReport::EmptyInput;
        r.acceptable = false;
        return r;
    }

    const int imgW = gray.width();
    const int imgH = gray.height();
    const QRect region = clampedRegion(QSize(imgW, imgH), faceRect);
    if (region.isEmpty()) {
        // 框与图像完全不相交、或框本身为空。无法评估 -> 不通过（原因明确）。
        r.reason = FaceQualityReport::TooSmall;
        r.acceptable = false;
        return r;
    }

    r.areaRatio = (static_cast<double>(region.width()) * region.height())
                  / (static_cast<double>(imgW) * imgH);
    if (r.areaRatio > 1.0) {
        r.areaRatio = 1.0;   // 求交之后不可能超过 1，纯属防御
    }

    // 只需要看占比时先短路：占比不达标就没必要逐像素统计。
    if (r.areaRatio < t.minAreaRatio) {
        r.reason = FaceQualityReport::TooSmall;
        r.acceptable = false;
        return r;
    }

    const int x0 = region.x();
    const int y0 = region.y();
    const int x1 = region.x() + region.width();    // 右开区间
    const int y1 = region.y() + region.height();

    // 行首必须用 bytesPerLine()，**不能**用 width()。
    //
    // QImage 的扫描线按 4 字节对齐：宽度不是 4 的倍数时
    // bytesPerLine() > width()，行尾有填充字节。若用 width() 定位下一行，
    // 就会从**错误的位置**继续读，整幅指标发生偏移 —— 并且只在特定宽度下
    // 发作（640x480 恰好对齐，所以随手测的尺寸发现不了）。
    // tests/test_face_quality.cpp 里有专门一条用 641 等非对齐宽度钉住这点。
    const int stride = gray.bytesPerLine();
    const uchar* const bits = gray.constBits();

    // ---- 亮度均值与对比度 ----
    double sum = 0.0;
    double sumSq = 0.0;
    const qint64 n = static_cast<qint64>(region.width()) * region.height();
    for (int y = y0; y < y1; ++y) {
        const uchar* row = bits + static_cast<qint64>(y) * stride;
        for (int x = x0; x < x1; ++x) {
            const double v = row[x];
            sum += v;
            sumSq += v * v;
        }
    }
    if (n <= 0) {
        // 理论上到不了（region 非空）。防御：不除零。
        r.reason = FaceQualityReport::EmptyInput;
        r.acceptable = false;
        return r;
    }
    const double mean = sum / static_cast<double>(n);
    r.brightness = mean;

    // 总体方差 = E[x^2] - E[x]^2。浮点误差可能让它变成极小的负数，故夹取。
    double var = sumSq / static_cast<double>(n) - mean * mean;
    if (var < 0.0) {
        var = 0.0;
    }
    r.contrast = std::sqrt(var);

    // ---- 清晰度：Laplacian 响应的方差 ----
    //
    // 用四邻域卷积核 4*中心 - 上 - 下 - 左 - 右（离散 Laplacian 的一种）。
    // 只需遍历**内部**像素（边界像素凑不齐四个邻居），因此采样区域小于
    // 3x3 时一个样本都取不到，此时清晰度记为 0 并由更上层的阈值决定是否拒绝。
    double lapSum = 0.0;
    double lapSumSq = 0.0;
    qint64 lapN = 0;
    for (int y = y0 + 1; y < y1 - 1; ++y) {
        const uchar* row = bits + static_cast<qint64>(y) * stride;
        const uchar* prev = row - stride;
        const uchar* next = row + stride;
        for (int x = x0 + 1; x < x1 - 1; ++x) {
            const int lap = 4 * static_cast<int>(row[x])
                            - static_cast<int>(prev[x])
                            - static_cast<int>(next[x])
                            - static_cast<int>(row[x - 1])
                            - static_cast<int>(row[x + 1]);
            const double d = static_cast<double>(lap);
            lapSum += d;
            lapSumSq += d * d;
            ++lapN;
        }
    }
    if (lapN > 0) {
        const double lapMean = lapSum / static_cast<double>(lapN);
        double lapVar = lapSumSq / static_cast<double>(lapN) - lapMean * lapMean;
        if (lapVar < 0.0) {
            lapVar = 0.0;
        }
        r.sharpness = lapVar;
    } else {
        r.sharpness = 0.0;
    }

    // ---- 逐项判定（顺序即"最该先修的问题"的优先级）----
    if (r.brightness < t.minBrightness) {
        r.reason = FaceQualityReport::TooDark;
    } else if (r.brightness > t.maxBrightness) {
        r.reason = FaceQualityReport::TooBright;
    } else if (r.contrast < t.minContrast) {
        r.reason = FaceQualityReport::TooFlat;
    } else if (r.sharpness < t.minSharpness) {
        r.reason = FaceQualityReport::TooBlurry;
    } else {
        r.reason = FaceQualityReport::Ok;
        r.acceptable = true;
    }
    return r;
}

bool FaceQuality::passes(const QImage& image, const QRect& faceRect,
                         const FaceQualityThresholds& t)
{
    return evaluate(image, faceRect, t).acceptable;
}

} // namespace accessterminal
