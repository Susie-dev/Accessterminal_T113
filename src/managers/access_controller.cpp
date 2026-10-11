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

} // namespace

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

bool AccessController::request(AccessMethod method, const QString& source)
{
    const qint64 remaining = silentRemainingMs();
    if (remaining > 0) {
        ++m_suppressed;
        qInfo() << "[放行] 处于静默期，忽略本次" << methodText(method)
                << "请求，剩余" << remaining << "ms，来源:" << source;
        emit suppressed(static_cast<int>(method), source, remaining);
        return false;
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
