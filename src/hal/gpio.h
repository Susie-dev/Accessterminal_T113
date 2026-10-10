/**
 * @file gpio.h
 * @brief libgpiod 的极简 RAII 封装。
 *
 * 目的：把 gpiod 的 C 接口与资源生命周期收敛到一处，
 * 使 UI 层不再直接依赖 libgpiod，也不再手写 open/get/release/close。
 */
#ifndef ACCESSTERMINAL_HAL_GPIO_H
#define ACCESSTERMINAL_HAL_GPIO_H

#include <QString>

// 必须在使用 gpiod_chip/gpiod_line 之前引入 libgpiod 的声明。
// 否则在 namespace accessterminal 内书写 `struct gpiod_chip* m_chip;`
// 会被当作 elaborated-type-specifier，在**本命名空间**里声明出一个全新的
// 不完整类型 accessterminal::gpiod_chip，导致 gpiod_* 函数调用时实参
// 类型不匹配而编译失败（已由主机端真实编译验证捕获）。
#include <gpiod.h>

namespace accessterminal {

/**
 * @brief 单条 GPIO 线的输入/输出封装。
 *
 * 构造即尝试占用该线；失败时 isValid() 为 false，调用方需自行降级处理
 * （硬件不可用不应导致程序崩溃）。
 */
class GpioLine
{
public:
    GpioLine() = default;
    ~GpioLine();

    // 资源独占，禁止拷贝；允许移动。
    GpioLine(const GpioLine&) = delete;
    GpioLine& operator=(const GpioLine&) = delete;
    GpioLine(GpioLine&& other) noexcept;
    GpioLine& operator=(GpioLine&& other) noexcept;

    /**
     * @brief 以输出模式占用指定线。
     * @param chipName gpiochip 名称，如 "gpiochip0"。
     * @param lineOffset 线号，如 T113 的 PE5 = 133。
     * @param consumer 占用者名称，便于用 gpioinfo 排查。
     * @param initialValue 初始电平，0 或 1。
     * @return 成功返回 true。
     */
    bool openOutput(const QString& chipName, unsigned int lineOffset,
                    const QString& consumer, int initialValue = 0);

    /**
     * @brief 以输入模式占用指定线。
     * @return 成功返回 true。
     */
    bool openInput(const QString& chipName, unsigned int lineOffset,
                   const QString& consumer);

    /// 主动释放占用的线。
    void close();

    /// 是否已成功占用。
    bool isValid() const { return m_line != nullptr; }

    /**
     * @brief 读取电平。
     * @return 高电平返回 1，低电平返回 0，读取失败返回 -1。
     */
    int value() const;

    /**
     * @brief 设置输出电平。
     * @param v 非 0 视为高电平。
     * @return 成功返回 true。
     */
    bool setValue(int v);

private:
    struct gpiod_chip* m_chip = nullptr;
    struct gpiod_line* m_line = nullptr;
};

} // namespace accessterminal

#endif // ACCESSTERMINAL_HAL_GPIO_H
