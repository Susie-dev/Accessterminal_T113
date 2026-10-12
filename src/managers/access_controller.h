/**
 * @file access_controller.h
 * @brief 门禁业务编排中枢：**唯一**的放行入口。
 *
 * 存在的理由（借鉴相邻仓库 Accessterminal 的 AccessController）
 * ------------------------------------------------------------
 * 重构前"谁能进门"这件事散在 `Widget` 的四个 lambda/槽里（密码、指纹、
 * 人脸、远程 unlock），每一处各自播音、各自弹窗、各自决定要不要开门。
 * 任何一处改动都可能牵连界面，也没有任何地方能回答"这一秒到底放行了几次"。
 *
 * 本类把这件事收敛到一处：
 *   * 四条认证路径最终都必须调用 `request()`；
 *   * 它统一维护"放行静默期"（可配置，见下），避免门口站着的人被 3Hz 的
 *     人脸识别反复开门；
 *   * 它把结果以语义化信号抛出去，UI 只负责显示；
 *   * 真正的电气动作交给 `DoorActuator`（本类不碰 GPIO，规则 `[actuator]`
 *     守着这一点）。
 *
 * 静默期默认 **0 = 不抑制**（保持现状）
 * ------------------------------------
 * 静默期一旦大于 0 就**改变现场行为**（同一段时间内第二次认证不会再次驱动
 * 门锁）。按本工程"行为变更必须显式决策"的口径，默认值刻意取 0，由装配点
 * 显式决定；能力本身有测试覆盖（见 tests/test_access_controller.cpp），
 * 不是悬空的开关。
 */
#ifndef ACCESSTERMINAL_MANAGERS_ACCESS_CONTROLLER_H
#define ACCESSTERMINAL_MANAGERS_ACCESS_CONTROLLER_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>

namespace accessterminal {

class DoorActuator;

/**
 * @brief 认证方式。新增一种开门方式时，这里加一个枚举值，
 *        并在 `methodText()` 与装配点各补一处（编译期会提醒）。
 */
enum class AccessMethod {
    Password,     ///< 触摸键盘密码
    Fingerprint,  ///< 指纹模块识别
    Face,         ///< 人脸识别
    Remote        ///< 室内机下发的 unlock 信令
};

/// 认证方式的数量（新增枚举值时同步加一；`methodIndex()` 会做边界检查）。
const int kAccessMethodCount = 4;

/**
 * @brief 把 `AccessMethod` 映射到 `[0, kAccessMethodCount)` 的下标。
 *
 * 越界（枚举被扩展却忘了改 `kAccessMethodCount`）时返回 0 并**不崩溃** ——
 * 配置/状态数组的越界写是这类做法最常见的自伤方式。
 */
int methodIndex(AccessMethod method);

/**
 * @brief 放行决策中枢：唯一的"谁能进门"出口。
 */
class AccessController : public QObject
{
    Q_OBJECT
public:
    explicit AccessController(QObject* parent = nullptr);

    /**
     * @brief 装配开锁执行器（不接管所有权）。
     *
     * 传 nullptr 时 `request()` 仍然会记账与发信号，但不会有任何电气动作
     * （用于"只统计不动作"的测试与降级）。
     */
    void setActuator(DoorActuator* actuator);

    /// 放行静默期（毫秒）。0 = 不抑制（默认）；出口夹取到 [0, 60000]。
    void setSilentPeriodMs(int ms);
    int silentPeriodMs() const { return m_silentPeriodMs; }

    /// 当前是否处于静默期。
    bool isInSilentPeriod() const;
    /// 静默期剩余毫秒；不在静默期返回 0。
    qint64 silentRemainingMs() const;

    // ---- 连续命中平滑（防假阳性，方案 P4）----
    //
    // 为什么需要：本工程已实测确认，LBPH 在单 label 下 `predictedLabel == 1`
    // **恒为真**，距离阈值是**唯一的门**。一次凑巧落进阈值的误识别就足以开门。
    // 要求"窗口内连续命中 N 次"把单帧误判的概率变成需要连续 N 次都误判，
    // 显著降低假阳性开门。
    //
    // **按认证方式独立配置，不是全局开关。** 这是刻意的：如果做成全局，
    // 运维为了让人脸更严而设 requiredStreak=2，会**顺带**让"密码输一次不开门"
    // —— 一个静默改变其它三条认证路径的误配。按方式arm 之后，装配点只给
    // `Face` 设值，其余三条恒为 1（默认不改变任何行为）。
    //
    // 默认 `required = 1` = **不启用**，与重构前逐字节一致。

    /**
     * @brief 设置某种认证方式所需的"连续命中"次数与统计窗口。
     *
     * @param method   认证方式（只影响这一种）。
     * @param required 需要的连续命中次数。1 = 不启用（默认）。
     *                 出口夹取到 [1, 10]：超过 10 次在门口站着的人早就走了，
     *                 且会让"到底要试几次"变得不可预期。
     * @param windowMs 统计窗口（毫秒）。**从第一次命中开始计时**，不是从最近
     *                 一次 —— 这样"慢速滴灌"式的命中永远攒不满，见下方说明。
     *                 出口夹取到 [1, 60000]。
     *
     * 窗口从**第一次**命中起算，是防假阳性的关键：若从最近一次起算，
     * 只要命中间隔小于窗口就能无限累积，一个人（或一次持续误检）站在那里
     * 迟早会攒够 N 次 —— 门等于没锁。从第一次起算给出硬截止，
     * 超时即从头再来。判别力由 tools/verify_face_streak_rule.py 钉住。
     */
    void setRequiredStreak(AccessMethod method, int required, int windowMs);

    /// 该方式当前需要的连续命中次数（1 = 不启用）。
    int requiredStreak(AccessMethod method) const;

    /// 该方式当前统计窗口（毫秒）。
    int streakWindowMs(AccessMethod method) const;

    /// 该方式当前已累计的连续命中次数（尚未放行）。
    int pendingStreak(AccessMethod method) const;

    /// 清空全部方式的连续命中状态（不改阈值）。
    void resetStreaks();

    /**
     * @brief 请求放行 —— 四条认证路径的**唯一**入口。
     * @param method 认证方式。
     * @param source 来源描述（用于日志与信号；一般为方式的中文名）。
     * @return true 表示请求被受理（未处于静默期、且连续命中已满足）；
     *         false 表示被静默期抑制、或在攒连续命中、或未装配执行器之外的
     *         其它不满足条件。
     *         注意：返回值**不代表门开了** —— 门是否真的动作由 DoorActuator
     *         的 `pulsed(..., realActuator)` 说明。
     */
    bool request(AccessMethod method, const QString& source);

    /// 已受理的放行次数。
    int grantedCount() const { return m_granted; }
    /// 被静默期抑制的次数。
    int suppressedCount() const { return m_suppressed; }
    /// 因"连续命中未达标"而暂缓的次数（与静默期分开记账，便于现场区分）。
    int pendingCount() const { return m_pending; }

    /// 认证方式的可读名字（日志与信号用）。
    static QString methodText(AccessMethod method);

signals:
    /// 请求被受理（静默期之外）。
    void granted(int method, const QString& source);
    /// 请求被静默期抑制，附带剩余毫秒数。
    void suppressed(int method, const QString& source, qint64 remainingMs);
    /**
     * @brief 连续命中进度：本次**未**放行，已累计 current/required。
     *        与 `suppressed` 分开，便于界面区分"刚看到人"与"被静默期挡住"。
     */
    void streakProgress(int method, const QString& source, int current, int required);
    /// 执行器已动作；realActuator=false 表示板端未接执行器（模拟开锁）。
    void actuated(const QString& source, bool realActuator);

private:
    /// 一种认证方式的连续命中状态。
    struct StreakState {
        int required = 1;   ///< 1 = 不启用
        int windowMs = 0;
        int pending = 0;    ///< 已累计次数（未放行）
        bool hasFirst = false;
        QElapsedTimer sinceFirst;   ///< 单调时钟：不受系统时间调整影响
    };

    DoorActuator* m_actuator = nullptr;
    QElapsedTimer m_sinceGrant;   ///< 单调时钟：不受系统时间调整影响
    bool m_hasGranted = false;
    int m_silentPeriodMs = 0;
    int m_granted = 0;
    int m_suppressed = 0;
    int m_pending = 0;
    StreakState m_streak[kAccessMethodCount];
};

} // namespace accessterminal

#endif // ACCESSTERMINAL_MANAGERS_ACCESS_CONTROLLER_H
