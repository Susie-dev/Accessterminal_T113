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

    /**
     * @brief 请求放行 —— 四条认证路径的**唯一**入口。
     * @param method 认证方式。
     * @param source 来源描述（用于日志与信号；一般为方式的中文名）。
     * @return true 表示请求被受理（未处于静默期）；false 表示被静默期抑制。
     *         注意：返回值**不代表门开了** —— 门是否真的动作由 DoorActuator
     *         的 `pulsed(..., realActuator)` 说明。
     */
    bool request(AccessMethod method, const QString& source);

    /// 已受理的放行次数。
    int grantedCount() const { return m_granted; }
    /// 被静默期抑制的次数。
    int suppressedCount() const { return m_suppressed; }

    /// 认证方式的可读名字（日志与信号用）。
    static QString methodText(AccessMethod method);

signals:
    /// 请求被受理（静默期之外）。
    void granted(int method, const QString& source);
    /// 请求被静默期抑制，附带剩余毫秒数。
    void suppressed(int method, const QString& source, qint64 remainingMs);
    /// 执行器已动作；realActuator=false 表示板端未接执行器（模拟开锁）。
    void actuated(const QString& source, bool realActuator);

private:
    DoorActuator* m_actuator = nullptr;
    QElapsedTimer m_sinceGrant;   ///< 单调时钟：不受系统时间调整影响
    bool m_hasGranted = false;
    int m_silentPeriodMs = 0;
    int m_granted = 0;
    int m_suppressed = 0;
};

} // namespace accessterminal

#endif // ACCESSTERMINAL_MANAGERS_ACCESS_CONTROLLER_H
