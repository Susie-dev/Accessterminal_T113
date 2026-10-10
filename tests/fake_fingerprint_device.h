/**
 * @file fake_fingerprint_device.h
 * @brief FingerprintManager 测试用的可注入指纹设备替身。
 *
 * 为什么是"链接期替身"而不是继承覆盖
 * ----------------------------------
 * `FingerprintManager` 只持有 `HailinFingerprint*`，而 `HailinFingerprint` 的
 * `buildPacket/sendPacket/readPacket` **都不是 virtual**，无法通过继承覆盖。
 * 因此本文件提供同名成员函数的另一份定义，链接时**不链接真实的
 * fingerprint_hailin.cpp**，从而：
 *
 *   - 生产代码零改动（不加任何测试钩子）；
 *   - `buildPacket()` 仍走真实实现（见 fake_fingerprint_device.cpp，它直接
 *     拷贝了真实算法），因为**报文字节是唯一需要逐字节守住的东西**；
 *   - `sendPacket()` 变成可记录的（真实实现要开串口）；
 *   - `readPacket()` 变成可编程的应答队列（真实实现要等串口超时）。
 *
 * 与 `getFrame()` 替身（tests/fake_v4l2_camera.h）同一思路。
 */
#ifndef FAKE_FINGERPRINT_DEVICE_H
#define FAKE_FINGERPRINT_DEVICE_H

#include <QByteArray>
#include <QList>

namespace fakefp {

/// 清空记录与队列。
void reset();

// ---- 下发方向（sendPacket 记录）----

/// 记录到的每一个下发报文（完整帧，含包头与校验和）。
QList<QByteArray> sent();

/// 下发报文条数。
int sentCount();

/// 让接下来的 n 次 sendPacket 返回失败（模拟串口写不完整/超时）。
void setSendFailFirst(int n);

/// 第 i 条下发报文的 payload（去掉 9 字节包头与 2 字节校验和）。
/// 下标越界返回空。
QByteArray sentPayload(int index);

// ---- 应答方向（readPacket 出队）----

/// 压入一个**完整帧**作为下一次 readPacket 的返回值。
void pushResponse(const QByteArray& fullFrame);

/**
 * @brief 依据 payload 构造一个应答帧并压入队列。
 *
 * 自动按真实算法补齐包头、长度与校验和。注意 payload 的第 0 字节落在
 * 帧下标 9，因此"确认码"就是 payload[0]（与真实应答一致）。
 */
void pushAck(const QByteArray& payload);

/// 便捷：构造只含确认码的应答帧（payload = [confirm]）。
void pushConfirm(quint8 confirm);

/**
 * @brief 构造一个**任意包标识**的帧（tag 落在外显下标 6）。
 *
 * 真实模块的应答包标识是 0x07（PKT_ACK），命令包是 0x01（PKT_CMD）。
 * 本函数用于验证"接收方是否校验了包标识"这一契约：
 * 一个 tag=0x01（命令）且 byte[9]=0x00 的帧**不是**成功应答，
 * 若不校验 tag 就会被误判为"手指已按下"。
 */
void pushFrameWithTag(quint8 tag, const QByteArray& payload);

/**
 * @brief 构造**搜索应答**帧：payload = [confirm, param, id_hi, id_lo, score_hi, score_lo]。
 *
 * 布局依据（与 baseline 逐字节一致，勿改）：
 *   frame[9]=confirm  frame[10]=param  frame[11..12]=id  frame[13..14]=score
 *
 * 为什么必须带上 param 字节：`FingerprintManager` 读的是 **frame[11..14]**，
 * 若 payload 里省掉 param，id 就会整体前移一位，读出来的值完全错误
 * （我第一版就漏了 param，测试实测读到 id=1792 而不是 7）。
 * 这条布局由 baseline `HailinFingerprint.cpp` 的 autoIdentify() 佐证：
 * 它读 confirm=resp[9]、param=resp[10]、id=resp[11..12]、score=resp[13..14]。
 */
void pushSearchResult(quint8 confirm, quint16 id, quint16 score,
                      quint8 param = 0x00);

/// 便捷：构造带任意确认码与两个参数字节的应答帧（payload = [c, p1, p2]）。
void pushConfirmParams(quint8 confirm, quint8 p1, quint8 p2);

/**
 * @brief 压入 n 个"过短但合法"的帧（11 字节，仅包头+长度+校验和）。
 *
 * 这是边界核心：解析器只保证长度字段自洽，最短合法帧就是 11 字节。
 * 若被测代码在读取 resp[9] 之前只判断 isEmpty()，就会越界。
 */
void pushShortFrames(int n);

/// 尚未被取走的应答帧数。
int pendingCount();

/**
 * @brief 直接取出一帧应答（不走 readPacket），用于断言帧的**原始字节布局**。
 *
 * 为什么要这个口子：`readPacket()` 返回的是同一份字节，通过它也能断言；
 * 但这个函数让"布局"这件事在不启动轮询、不等待定时器的情况下就能验证，
 * 使布局断言与调度逻辑解耦。队列为空返回空 QByteArray。
 */
QByteArray takeRawResponse();

/// readPacket 被调用的次数。
int readCount();

/// readPacket 收到的超时参数序列（用于断言每一步给够/给对了超时）。
QList<int> readTimeouts();

/// buildPacket 被调用的次数。
int buildCount();

} // namespace fakefp

#endif // FAKE_FINGERPRINT_DEVICE_H
