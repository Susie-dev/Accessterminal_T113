/**
 * @file door_actuator.cpp
 * @brief DoorActuator 的实现：非阻塞脉冲开锁。
 */
#include "managers/door_actuator.h"

#include "hal/gpio.h"

#include <QDebug>
#include <QTimer>

namespace accessterminal {

namespace {

/// 默认脉冲宽度：与重构前"开锁 1 秒"的现场语义一致。
const int kDefaultDurationMs = 1000;
/// 出口兜底范围：小于 1ms 等于没驱动，比 10s 更长没有物理意义且会锁死继电器。
const int kMinDurationMs = 1;
const int kMaxDurationMs = 10000;

int clampDuration(int ms)
{
    if (ms < kMinDurationMs) {
        return kMinDurationMs;
    }
    if (ms > kMaxDurationMs) {
        return kMaxDurationMs;
    }
    return ms;
}

} // namespace

DoorActuator::DoorActuator(QObject* parent)
    : QObject(parent)
    , m_releaseTimer(new QTimer(this))
{
    // 单次定时器：拉高之后由它负责拉低，因此 unlock() 不需要阻塞。
    m_releaseTimer->setSingleShot(true);
    connect(m_releaseTimer, &QTimer::timeout, this, &DoorActuator::releaseOutput);
}

DoorActuator::~DoorActuator() = default;

void DoorActuator::setLine(GpioLine* line)
{
    m_line = line;
}

void DoorActuator::setDurationMs(int ms)
{
    const int clamped = clampDuration(ms);
    if (clamped != ms) {
        qWarning() << "[开锁] 脉冲宽度" << ms << "ms 超出合法范围，夹取为" << clamped << "ms";
    }
    m_durationMs = clamped;
}

bool DoorActuator::isWired() const
{
    return m_line != nullptr && m_line->isValid();
}

bool DoorActuator::isPulsing() const
{
    return m_releaseTimer->isActive();
}

bool DoorActuator::unlock(const QString& source)
{
    // 情况一：板端未接执行器 —— 模拟开锁。
    // 日志必须写明"模拟"，否则现场会把提示音当成"门开了"（这是重构前
    // 最容易骗过验收的一类假象）。
    if (!isWired()) {
        qInfo() << "[开锁] 模拟开锁（板端未接门锁执行器），来源:" << source;
        emit pulsed(source, m_durationMs, false);
        return false;
    }

    // 情况二：已有脉冲在途 —— 不重复驱动。
    if (m_releaseTimer->isActive()) {
        qInfo() << "[开锁] 脉冲进行中，忽略本次重复请求，来源:" << source;
        return false;
    }

    // 情况三：真实驱动。写失败时**不**谎报成功。
    if (!m_line->setValue(1)) {
        qWarning() << "[开锁] 驱动门锁失败（写 GPIO 失败），来源:" << source;
        emit pulsed(source, m_durationMs, false);
        return false;
    }

    ++m_pulseCount;
    m_activeSource = source;
    m_releaseTimer->start(m_durationMs);
    qInfo() << "[开锁] 已驱动门锁" << m_durationMs << "ms，来源:" << source;
    emit pulsed(source, m_durationMs, true);
    return true;
}

void DoorActuator::releaseOutput()
{
    const QString source = m_activeSource;
    m_activeSource.clear();

    if (m_line != nullptr && m_line->isValid() && !m_line->setValue(0)) {
        // 拉低失败要说出来：线会一直保持高电平（锁一直通电）。
        qWarning() << "[开锁] 释放门锁失败（写 GPIO 失败），来源:" << source;
    }
    emit released(source);
}

} // namespace accessterminal
