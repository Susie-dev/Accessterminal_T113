/**
 * @file access_controller.cpp
 * @brief AccessController 的实现：放行记账 + 静默期 + 转发给执行器。
 */
#include "managers/access_controller.h"

#include "managers/door_actuator.h"

#include <QDebug>

namespace accessterminal {

namespace {

/// 静默期上限：比一分钟更长没有意义（门口的人早走了），且能挡住误配。
const int kMaxSilentPeriodMs = 60000;

/// 连续命中次数上限：超过 10 次门口的人早走了，且"到底要试几次"不可预期。
const int kMaxRequiredStreak = 10;

/// 连续命中窗口上限，与静默期同量级。
const int kMaxStreakWindowMs = 60000;

int clampSilentPeriod(int ms)
{
    if (ms < 0) {
        return 0;
    }
    if (ms > kMaxSilentPeriodMs) {
        return kMaxSilentPeriodMs;
    }
    return ms;
}

int clampRequiredStreak(int n)
{
    if (n < 1) {
        return 1;
    }
    if (n > kMaxRequiredStreak) {
        return kMaxRequiredStreak;
    }
    return n;
}

int clampStreakWindow(int ms)
{
    if (ms < 1) {
        return 1;
    }
    if (ms > kMaxStreakWindowMs) {
        return kMaxStreakWindowMs;
    }
    return ms;
}

} // namespace

int methodIndex(AccessMethod method)
{
    const int i = static_cast<int>(method);
    if (i < 0 || i >= kAccessMethodCount) {
        return 0;   // 越界不崩：枚举被扩展却忘了改 kAccessMethodCount 时兜底
    }
    return i;
}

AccessController::AccessController(QObject* parent)
    : QObject(parent)
{
}

void AccessController::setActuator(DoorActuator* actuator)
{
    if (m_actuator == actuator) {
        return;
    }
    if (m_actuator != nullptr) {
        disconnect(m_actuator, nullptr, this, nullptr);
    }
    m_actuator = actuator;
    if (m_actuator != nullptr) {
        // 把执行器的"真实/模拟"结果转发出去，UI 与日志只需要看本类。
        connect(m_actuator, &DoorActuator::pulsed, this,
                [this](const QString& source, int /*durationMs*/, bool realActuator) {
                    emit actuated(source, realActuator);
                });
    }
}

void AccessController::setSilentPeriodMs(int ms)
{
    const int clamped = clampSilentPeriod(ms);
    if (clamped != ms) {
        qWarning() << "[放行] 静默期" << ms << "ms 超出合法范围，夹取为" << clamped << "ms";
    }
    m_silentPeriodMs = clamped;
}

bool AccessController::isInSilentPeriod() const
{
    return silentRemainingMs() > 0;
}

qint64 AccessController::silentRemainingMs() const
{
    if (!m_hasGranted || m_silentPeriodMs <= 0 || !m_sinceGrant.isValid()) {
        return 0;
    }
    const qint64 elapsed = m_sinceGrant.elapsed();
    if (elapsed >= m_silentPeriodMs) {
        return 0;
    }
    return static_cast<qint64>(m_silentPeriodMs) - elapsed;
}

void AccessController::setRequiredStreak(AccessMethod method, int required,
                                         int windowMs)
{
    const int cReq = clampRequiredStreak(required);
    const int cWin = clampStreakWindow(windowMs);
    if (cReq != required) {
        qWarning() << "[放行] 连续命中次数" << required << "超出合法范围，夹取为"
                   << cReq << "（方式:" << methodText(method) << "）";
    }
    if (cWin != windowMs) {
        qWarning() << "[放行] 连续命中窗口" << windowMs << "ms 超出合法范围，夹取为"
                   << cWin << "ms（方式:" << methodText(method) << "）";
    }
    StreakState& s = m_streak[methodIndex(method)];
    s.required = cReq;
    s.windowMs = cWin;
    // 阈值变了就把已有进度清掉：否则"先攒了 3 次、再把阈值从 5 改成 2"
    // 会**立刻**放行一次，而那一次并不是在 2 次窗口内攒出来的。
    s.pending = 0;
    s.hasFirst = false;
}

int AccessController::requiredStreak(AccessMethod method) const
{
    return m_streak[methodIndex(method)].required;
}

int AccessController::streakWindowMs(AccessMethod method) const
{
    return m_streak[methodIndex(method)].windowMs;
}

int AccessController::pendingStreak(AccessMethod method) const
{
    return m_streak[methodIndex(method)].pending;
}

void AccessController::resetStreaks()
{
    for (int i = 0; i < kAccessMethodCount; ++i) {
        m_streak[i].pending = 0;
        m_streak[i].hasFirst = false;
    }
}

bool AccessController::request(AccessMethod method, const QString& source)
{
    // 顺序很重要：先判静默期（既有行为，优先保持），再判连续命中。
    // 若把两者调换，"静默期内的人脸命中"会被算进连续计数，使静默期
    // 实际上缩短了人脸所需的观察时间 —— 两个独立机制互相污染。
    const qint64 remaining = silentRemainingMs();
    if (remaining > 0) {
        ++m_suppressed;
        qInfo() << "[放行] 处于静默期，忽略本次" << methodText(method)
                << "请求，剩余" << remaining << "ms，来源:" << source;
        emit suppressed(static_cast<int>(method), source, remaining);
        return false;
    }

    // ---- 连续命中平滑：required == 1 时整段短路，行为与重构前完全一致 ----
    StreakState& s = m_streak[methodIndex(method)];
    if (s.required > 1) {
        const bool expired = s.hasFirst && s.sinceFirst.isValid()
                             && s.sinceFirst.elapsed() >= s.windowMs;
        if (!s.hasFirst || expired) {
            // 首次命中，或窗口已过 -> 从这一次**重新**开始计时与计数。
            s.sinceFirst.start();
            s.hasFirst = true;
            s.pending = 1;
        } else {
            ++s.pending;
        }

        if (s.pending < s.required) {
            // 还没攒够：暂缓放行。**不**动 m_sinceGrant（未放行就没有静默期）。
            ++m_pending;
            qInfo() << "[放行] 连续命中未达标，" << methodText(method) << "已"
                    << s.pending << "/" << s.required << "，来源:" << source;
            emit streakProgress(static_cast<int>(method), source, s.pending,
                                s.required);
            return false;
        }

        // 攒够了：清空进度（下一次要重新攒），然后照常放行。
        s.pending = 0;
        s.hasFirst = false;
    }

    ++m_granted;
    m_sinceGrant.start();
    m_hasGranted = true;
    qInfo() << "[放行] 认证通过，方式:" << methodText(method) << "来源:" << source;
    emit granted(static_cast<int>(method), source);

    if (m_actuator != nullptr) {
        // 电气动作交给执行器（它自己决定真实驱动还是模拟开锁）。
        m_actuator->unlock(source);
    } else {
        qWarning() << "[放行] 未装配开锁执行器，门锁不会有任何动作，来源:" << source;
    }
    return true;
}

QString AccessController::methodText(AccessMethod method)
{
    switch (method) {
    case AccessMethod::Password:    return QStringLiteral("密码");
    case AccessMethod::Fingerprint: return QStringLiteral("指纹");
    case AccessMethod::Face:        return QStringLiteral("人脸");
    case AccessMethod::Remote:      return QStringLiteral("远程");
    }
    return QStringLiteral("未知");
}

} // namespace accessterminal
