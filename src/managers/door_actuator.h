/**
 * @file door_actuator.h
 * @brief 开锁执行器：只负责"驱动门锁"这一件事，**不掺补光**。
 *
 * 为什么需要单独一个类
 * --------------------
 * 重构前（以及第 40 轮之前）本项目里**没有"开锁"这条代码路径**：
 * `Widget::m_auxLine`（PE5，线号 133）只有 5 处使用，全是补光
 * （采样第 1 张开、第 3 张关、识别结果回来关、挂断关、检出人但无脸时开）；
 * 认证成功侧只播音/弹窗，从不驱动任何输出。
 *
 * 而 app_config.h 写的是"开锁继电器与补光灯共用 PE5"。两种解释都有问题：
 *   - 若 PE5 是继电器，则现状是"采集人脸时给锁上电、识别成功时把锁关掉"；
 *   - 若 PE5 只是灯，则认证成功根本不开门。
 * 更根本的问题是：**代码里"补光"与"开锁"共用一条线、一个变量，没有任何
 * 语义区分**，48 项静态检查与 71 条约定里也没有一条守着它。
 *
 * 本类把"执行器"独立出来：它只有一个语义动作 `unlock()`，且**必须**由
 * 认证成功路径调用。板端尚未接入门锁执行器时（`setLine(nullptr)`），它
 * 走"模拟开锁"：打印明确日志 + 发出 `realActuator=false` 的信号，绝不假装
 * 门开了。这与相邻仓库 Accessterminal 的做法一致（那里 `door.relayLine`
 * 默认 -1，日志里标记"模拟开锁（板端未接门锁执行器）"）。
 *
 * 两条硬性设计约束（都有测试守着）
 * --------------------------------
 *   1. **非阻塞**：`unlock()` 立即返回，由单次定时器在 durationMs 后拉低。
 *      旧工程用同步 `QThread::msleep(1000)` 把 GUI 冻住 1 秒，本类不允许
 *      重演（测试断言 `unlock()` 返回耗时 < 50ms）。
 *   2. **不重复驱动**：脉冲进行中再次 `unlock()` 只记日志，不产生第二次物理
 *      脉冲（电气上重复驱动继电器没有意义，且会让锁"抖动"）。
 *      注意与认证层的"放行静默期"分工不同：那里管的是"同一个人别被反复
 *      开门"，这里管的是"同一秒别把继电器来回打"。
 *
 * 依赖注入与所有权：本类**不创建也不释放** `GpioLine`，只持有一个裸指针
 * （与 `Widget` 装配其它模块的方式一致：`main.cpp`/`Widget` 管生命周期）。
 */
#ifndef ACCESSTERMINAL_MANAGERS_DOOR_ACTUATOR_H
#define ACCESSTERMINAL_MANAGERS_DOOR_ACTUATOR_H

#include <QObject>
#include <QString>

class QTimer;

namespace accessterminal {

class GpioLine;

/**
 * @brief 门锁执行器：非阻塞脉冲 + 真实/模拟开锁可观测。
 */
class DoorActuator : public QObject
{
    Q_OBJECT
public:
    explicit DoorActuator(QObject* parent = nullptr);
    ~DoorActuator() override;

    /**
     * @brief 装配门锁所在的那条 GPIO 线。
     *
     * 传 nullptr 表示**板端未接门锁执行器**，此时 `unlock()` 走模拟开锁。
     * 传 PE5（补光灯那条线）是**错误用法**：本类与补光无关，规则
     * `[actuator]` 会拦住把补光标识符写进本类的做法。
     */
    void setLine(GpioLine* line);

    /// 脉冲宽度（毫秒）。出口做范围兜底，非法值夹取而不是崩。
    void setDurationMs(int ms);

    /// 当前脉冲宽度（已兜底）。
    int durationMs() const { return m_durationMs; }

    /// 是否已接上真实执行器（线存在且占用成功）。
    bool isWired() const;

    /// 已产生的**物理**脉冲次数（模拟开锁不计入）。便于测试与日志核对。
    int pulseCount() const { return m_pulseCount; }

    /// 当前是否有脉冲在途。
    bool isPulsing() const;

    /**
     * @brief 请求开锁。非阻塞：立即返回，durationMs 后由定时器拉低。
     * @param source 来源描述（"密码"/"指纹"/"人脸"/"远程"…），只用于日志与信号。
     * @return 本次是否真的产生了一次**物理**脉冲。
     *         模拟开锁、重复请求、写失败都返回 false。
     */
    bool unlock(const QString& source);

signals:
    /**
     * @brief 收到开锁请求时发出（无论真实还是模拟），用于界面与日志。
     * @param realActuator false 表示板端未接执行器 / 写失败，门**没有**真的开。
     */
    void pulsed(const QString& source, int durationMs, bool realActuator);

    /// 脉冲结束、输出已拉低时发出。
    void released(const QString& source);

private:
    void releaseOutput();

    GpioLine* m_line = nullptr;   ///< 不持有所有权
    QTimer* m_releaseTimer = nullptr;
    int m_durationMs = 1000;
    int m_pulseCount = 0;
    QString m_activeSource;
};

} // namespace accessterminal

#endif // ACCESSTERMINAL_MANAGERS_DOOR_ACTUATOR_H
