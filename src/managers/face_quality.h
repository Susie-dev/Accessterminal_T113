/**
 * @file face_quality.h
 * @brief 人脸图像质量门：在"要不要拿这一帧去识别"之前先判断它值不值得识别。
 *
 * 为什么需要它（防假阳性，方案 P4）
 * ---------------------------------
 * LBPH 的距离阈值是**唯一的门**（见 tests/test_face_manager.cpp 实测结论：
 * 单 label 时 `predictedLabel == 1` 恒为真）。于是现场会发生两件坏事：
 *
 *   1. **假阳性开门**：一张模糊/过曝/过暗的图，其 LBPH 距离可能凑巧落进阈值
 *      之内 —— 门开了，而门外站着的人并不是注册者。这是**安全**问题，
 *      不是体验问题。
 *   2. **无谓开销**：明显不可用的帧（人脸只占 20 个像素、整幅全黑）照样
 *      跑完整的 predict()，在 T113 上纯属浪费，还挤占了本该给清晰帧的窗口。
 *
 * 本类只做**纯像素统计**，不依赖 OpenCV
 * -------------------------------------
 * 方案文档原计划"按本项目惯例，缺 OpenCV 时跳过测试"。实际实现时发现
 * 清晰度（Laplacian 方差）用四邻域卷积只需十几行纯 C++，于是**改成不依赖
 * OpenCV**：这样 `t113_test_face_quality` 在任何环境都真的跑，规则不会
 * 因为"本机没装 face 模块"而静默失效（那正是 §78 说的"安静地少跑一条"）。
 *
 * 默认阈值 = **全部不限制**，因此不配置时行为逐字节不变
 * --------------------------------------------------------
 * 与本工程其它"能力类"改动同一纪律（`AccessController::silentPeriodMs`
 * 默认 0、`CredentialStore::maxAttempts` 默认 0）：
 * 质量门默认放过一切（`minSharpness = 0` 等），由装配点按现场标定显式收紧。
 * 这样重构不会悄悄改变"谁进得来"。
 *
 * 边界与数值口径（每条都有测试）
 * ------------------------------
 *   - **必须用 `bytesPerLine()` 定位行首，不能用 `width()`**：QImage 每行
 *     按 4 字节对齐，当图宽不是 4 的倍数时两者不等。用 width() 会读到上一行
 *     的尾部填充，指标整体偏移 —— 且只在特定尺寸下发作，极易漏网。
 *   - 人脸框允许越界：与 `QImage::copy()` 的语义一致地与图像求交；
 *     完全不相交时返回 `TooSmall` 而不是崩溃。
 *   - 空图 / 零尺寸 / 空框一律安全返回，不抛异常、不除零。
 *   - 方差用**总体方差**（除以 N），N = 有效像素数；N == 0 时返回 0。
 */
#ifndef ACCESSTERMINAL_MANAGERS_FACE_QUALITY_H
#define ACCESSTERMINAL_MANAGERS_FACE_QUALITY_H

#include <QImage>
#include <QRect>

namespace accessterminal {

/**
 * @brief 质量门的判定阈值。**默认全部不限制**（见文件头说明）。
 *
 * 全部用"下限/上限"表达，含义直白：
 *   - `minSharpness`：Laplacian 方差下限，越大要求越清晰；
 *   - `minBrightness` / `maxBrightness`：灰度均值允许区间；
 *   - `minContrast`：灰度标准差下限；
 *   - `minAreaRatio`：人脸框面积占整幅比例下限（0~1）。
 *
 * 某个字段取默认值（0 或 255）即表示**该项不参与判定**。
 */
struct FaceQualityThresholds {
    double minSharpness = 0.0;
    double minBrightness = 0.0;
    double maxBrightness = 255.0;
    double minContrast = 0.0;
    double minAreaRatio = 0.0;
};

/**
 * @brief 一次质量评估的结果。
 *
 * `acceptable` 是**唯一的结论**；其余字段只用于日志与现场标定
 * （现场想知道"为什么这一帧被拒"，而不是只知道被拒了）。
 */
struct FaceQualityReport {
    /// 不通过的原因。`Ok` 表示通过。
    enum Reason {
        Ok = 0,      ///< 通过
        EmptyInput,  ///< 空图 / 零尺寸 / 空框（不可评估，视为不通过）
        TooSmall,    ///< 人脸框与图像不相交，或占比低于 minAreaRatio
        TooDark,     ///< 亮度均值低于 minBrightness
        TooBright,   ///< 亮度均值高于 maxBrightness
        TooFlat,     ///< 对比度（标准差）低于 minContrast
        TooBlurry    ///< 清晰度（Laplacian 方差）低于 minSharpness
    };

    double sharpness = 0.0;   ///< Laplacian 响应的总体方差（越大越清晰）
    double brightness = 0.0;  ///< 灰度均值 [0, 255]
    double contrast = 0.0;    ///< 灰度标准差 [0, ~128]
    double areaRatio = 0.0;   ///< 人脸框面积 / 整幅面积，已夹取到 [0, 1]
    bool acceptable = false;  ///< 是否通过全部已启用的判定项
    Reason reason = EmptyInput;

    /// 便于日志：结论 + 四个指标的一行摘要。
    QString describe() const;
};

/// 质量门原因的可读文本（中文，用于日志与信号）。
const char* faceQualityReasonText(FaceQualityReport::Reason reason);

/**
 * @brief 图像质量评估器。**无状态**，全部是静态纯函数。
 */
class FaceQuality
{
public:
    /**
     * @brief 评估一帧中某个人脸区域的质量。
     *
     * @param image    整幅图像。非 Grayscale8 格式会被转换（**不修改入参**）。
     * @param faceRect 人脸框（相对整幅图像）。允许越界，按图像边界求交。
     * @param t        阈值；默认表示"不限制"。
     * @return 评估结果。任何非法输入都返回 `acceptable == false` 且带原因，
     *         **不会抛异常、不会崩溃、不会除零**。
     */
    static FaceQualityReport evaluate(const QImage& image,
                                      const QRect& faceRect,
                                      const FaceQualityThresholds& t =
                                          FaceQualityThresholds());

    /**
     * @brief 仅判断是否通过（等价于 `evaluate(...).acceptable`）。
     *        便于装配点写成 `if (!FaceQuality::passes(...)) return;`。
     */
    static bool passes(const QImage& image, const QRect& faceRect,
                       const FaceQualityThresholds& t = FaceQualityThresholds());

    /**
     * @brief 与图像求交后的实际采样区域（供测试与日志核对）。
     *        完全不相交时返回空 QRect。
     */
    static QRect clampedRegion(const QSize& imageSize, const QRect& faceRect);
};

} // namespace accessterminal

#endif // ACCESSTERMINAL_MANAGERS_FACE_QUALITY_H
