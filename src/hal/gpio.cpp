/**
 * @file gpio.cpp
 * @brief GpioLine 实现。
 */
#include "hal/gpio.h"

#include <QDebug>

#include <gpiod.h>
#include <utility>

namespace accessterminal {

GpioLine::~GpioLine()
{
    close();
}

GpioLine::GpioLine(GpioLine&& other) noexcept
    : m_chip(other.m_chip), m_line(other.m_line)
{
    other.m_chip = nullptr;
    other.m_line = nullptr;
}

GpioLine& GpioLine::operator=(GpioLine&& other) noexcept
{
    if (this != &other) {
        close();
        m_chip = other.m_chip;
        m_line = other.m_line;
        other.m_chip = nullptr;
        other.m_line = nullptr;
    }
    return *this;
}

bool GpioLine::openOutput(const QString& chipName, unsigned int lineOffset,
                          const QString& consumer, int initialValue)
{
    close();

    m_chip = gpiod_chip_open_by_name(chipName.toUtf8().constData());
    if (!m_chip) {
        qWarning() << "[GPIO] 无法打开" << chipName;
        return false;
    }

    m_line = gpiod_chip_get_line(m_chip, lineOffset);
    if (!m_line) {
        qWarning() << "[GPIO] 无法获取线" << lineOffset << "@" << chipName;
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
        return false;
    }

    if (gpiod_line_request_output(m_line, consumer.toUtf8().constData(),
                                 initialValue) < 0) {
        qWarning() << "[GPIO] 请求输出失败:" << lineOffset << "@" << chipName;
        gpiod_line_release(m_line);
        m_line = nullptr;
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
        return false;
    }
    return true;
}

bool GpioLine::openInput(const QString& chipName, unsigned int lineOffset,
                         const QString& consumer)
{
    close();

    m_chip = gpiod_chip_open_by_name(chipName.toUtf8().constData());
    if (!m_chip) {
        qWarning() << "[GPIO] 无法打开" << chipName;
        return false;
    }

    m_line = gpiod_chip_get_line(m_chip, lineOffset);
    if (!m_line) {
        qWarning() << "[GPIO] 无法获取线" << lineOffset << "@" << chipName;
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
        return false;
    }

    if (gpiod_line_request_input(m_line, consumer.toUtf8().constData()) < 0) {
        qWarning() << "[GPIO] 请求输入失败:" << lineOffset << "@" << chipName;
        gpiod_line_release(m_line);
        m_line = nullptr;
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
        return false;
    }
    return true;
}

void GpioLine::close()
{
    // 注意释放顺序：先释放线，再关闭 chip。
    if (m_line) {
        gpiod_line_release(m_line);
        m_line = nullptr;
    }
    if (m_chip) {
        gpiod_chip_close(m_chip);
        m_chip = nullptr;
    }
}

int GpioLine::value() const
{
    if (!m_line) {
        return -1;
    }
    return gpiod_line_get_value(m_line);
}

bool GpioLine::setValue(int v)
{
    if (!m_line) {
        return false;
    }
    return gpiod_line_set_value(m_line, v) >= 0;
}

} // namespace accessterminal
