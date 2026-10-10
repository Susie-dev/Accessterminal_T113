/**
 * @file fingerprint_frame_parser.h
 * @brief 指纹模块串口帧解析器（纯逻辑，不依赖串口硬件）。
 *
 * 解决的真实缺陷
 * --------------
 * 重构前的 `HailinFingerprint::readPacket()` 用一个**局部** `QByteArray buf`
 * 累积串口数据，解析出第一个完整包后直接 `return buf.left(totalLen)`：
 *
 *     QByteArray HailinFingerprint::readPacket(int timeoutMs) {
 *         QByteArray buf;                       // ← 局部变量，函数返回即丢弃
 *         ...
 *         while (timer.elapsed() < timeoutMs) {
 *             if (m_serial.waitForReadyRead(100)) {
 *                 buf += m_serial.readAll();
 *                 if (buf.size() >= 12) {
 *                     quint16 pktLen = ((quint8)buf[7] << 8) | (quint8)buf[8];
 *                     int totalLen = 9 + pktLen;
 *                     if (buf.size() >= totalLen) {
 *                         QByteArray pkt = buf.left(totalLen);
 *                         return pkt;           // ← 剩余字节随 buf 一起丢弃
 *                     }
 *                 }
 *             }
 *         }
 *         return QByteArray();
 *     }
 *
 * 这会产生两类丢包，且都发生在真实硬件常见的时序下：
 *
 * 1. **一次 readAll() 收到多个包**时，只返回第一个，其余字节永久丢失。
 *    注册/识别流程是「发一帧、收一帧」的严格交替，但这些应答由模块连续
 *    发出；串口按 100ms 轮询读取，很容易一次读到多帧。丢失的应答会让
 *    后续 `readPacket()` 一直等到超时，表现为“注册随机失败/只能成功一次”。
 *
 * 2. **超时返回时**，已收到的半包也被丢弃。下一次调用重新从零开始累积，
 *    于是被截断的那一帧永远无法拼齐。
 *
 * 另外原实现没有任何包头同步：若串口噪声让字节流错位，`buf[7]/buf[8]`
 * 读到的是噪声，算出的 `totalLen` 是随机的，解析会一直错位下去。
 *
 * 本解析器把“累积 + 定帧”这部分纯逻辑独立出来：跨调用保存缓冲区、按
 * `0xEF 0x01` 包头重新同步、且一次喂入可以产出**任意多个**完整帧。
 * 与 `core/net/protocol.h` 里的 `StreamDecoder` 同构，便于单元测试。
 *
 * 协议帧格式（与 `HailinFingerprint::buildPacket()` 一致）
 * -------------------------------------------------------
 *   [0..1]   包头      0xEF 0x01
 *   [2..5]   设备地址  默认 0xFFFFFFFF（广播）
 *   [6]      包标识    0x01=命令 / 0x07=应答
 *   [7..8]   长度      大端，= payload 字节数 + 2（含校验和）
 *   [9..]    payload
 *   [末 2]   校验和    大端，= 自下标 6 起累加到 payload 末尾（不含校验和自身）
 *
 * 注意：校验和沿用 `buildPacket()` 的既有约定（从下标 6 起算、不含包头与
 * 设备地址），因此 `checksumOf()` 传入**去掉末 2 字节**的帧即为期望值。
 */

#ifndef FINGERPRINT_FRAME_PARSER_H
#define FINGERPRINT_FRAME_PARSER_H

#include <QByteArray>
#include <QList>

namespace accessterminal {

/**
 * @brief 把串口字节流切分为完整的协议帧。
 *
 * 使用方式（跨多次 readyRead 复用同一实例）：
 *
 *     const QList<QByteArray> frames = parser.feed(m_serial.readAll());
 *     for (const QByteArray& f : frames) { ... }
 *
 * `feed()` 返回本次新解析出的**全部**完整帧；不完整的数据留在内部缓冲区，
 * 等下次喂入时继续拼接。会话结束时调用 `reset()`。
 */
class FingerprintFrameParser
{
public:
    /// 固定头部字节数：包头 2 + 地址 4 + 标识 1 + 长度 2。
    static const int kHeaderBytes = 9;
    /// 尾部校验和字节数。
    static const int kChecksumBytes = 2;
    /// 内部缓冲上限。合法帧最大也就是这个量级，超过即判定为噪声并重新同步。
    static const int kMaxBufferedBytes = 4096;

    /// 清空缓冲（串口重开/关闭时必须调用，避免上一会话的残留字节串到下一会话）。
    void reset() { m_buffer.clear(); }

    /**
     * @brief 喂入新收到的字节，返回本次能切出的所有完整帧。
     * @param data 新数据；可为空，此时仅尝试把缓冲区里已完整的帧取出。
     */
    QList<QByteArray> feed(const QByteArray& data)
    {
        if (!data.isEmpty()) {
            m_buffer += data;
        }

        QList<QByteArray> frames;
        while (true) {
            QByteArray frame;
            if (!takeOne(frame)) {
                break;
            }
            frames.append(frame);
        }

        // 兜底：若缓冲区超出上限却仍切不出帧，说明是噪声或被截断的伪长度，
        // 此时只保留最后 1 字节（可能是下一个包头的 0xEF）后重新同步，
        // 防止内存无限增长。
        if (m_buffer.size() > kMaxBufferedBytes) {
            m_buffer = m_buffer.right(1);
        }
        return frames;
    }

    /// 当前未切出完整帧的残留字节数。
    int bufferedBytes() const { return m_buffer.size(); }
    /// 缓冲区中是否留有半包。
    bool hasPartial() const { return !m_buffer.isEmpty(); }

    /**
     * @brief 计算校验和：自下标 6 起累加至末尾。
     * @param frameWithoutChecksum 去掉尾部 2 字节校验和的帧。
     */
    static quint16 checksumOf(const QByteArray& frameWithoutChecksum)
    {
        quint16 sum = 0;
        for (int i = 6; i < frameWithoutChecksum.size(); ++i) {
            sum += static_cast<quint8>(frameWithoutChecksum.at(i));
        }
        return sum;
    }

    /// 校验一个**完整帧**的尾部校验和是否正确。
    static bool checksumMatches(const QByteArray& frame)
    {
        if (frame.size() < kHeaderBytes + kChecksumBytes) {
            return false;
        }
        const QByteArray body = frame.left(frame.size() - kChecksumBytes);
        const quint16 expected = checksumOf(body);
        const quint16 actual =
            (static_cast<quint16>(static_cast<quint8>(frame.at(frame.size() - 2))) << 8) |
            static_cast<quint8>(frame.at(frame.size() - 1));
        return expected == actual;
    }

private:
    /// 从缓冲区头部尝试取出一帧；取出成功返回 true。取不出则原样保留。
    bool takeOne(QByteArray& out)
    {
        while (true) {
            const int start = findHeader();
            if (start < 0) {
                // 整个缓冲区都没有包头：只在末尾留下 1 字节，因为它可能是
                // 被拆开的包头的第一个字节（0xEF）。
                if (m_buffer.size() > 1) {
                    m_buffer = m_buffer.right(1);
                }
                return false;
            }
            if (start > 0) {
                m_buffer.remove(0, start); // 丢弃包头之前的噪声
            }
            if (m_buffer.size() < kHeaderBytes) {
                return false; // 还没攒够长度字段
            }

            const quint16 payloadAndChecksum =
                (static_cast<quint16>(static_cast<quint8>(m_buffer.at(7))) << 8) |
                static_cast<quint8>(m_buffer.at(8));
            const int totalLen = kHeaderBytes + payloadAndChecksum;

            if (payloadAndChecksum < kChecksumBytes || totalLen > kMaxBufferedBytes) {
                // 长度字段不合理（连校验和都放不下，或超出上限）：说明这个
                // 包头是噪声伪装的，跳过后继续向后重新同步。
                m_buffer.remove(0, 2);
                continue;
            }
            if (m_buffer.size() < totalLen) {
                return false; // 半包，等更多数据
            }

            out = m_buffer.left(totalLen);
            m_buffer.remove(0, totalLen);
            return true;
        }
    }

    /// 查找包头 0xEF 0x01 的下标；找不到返回 -1。
    int findHeader() const
    {
        for (int i = 0; i + 1 < m_buffer.size(); ++i) {
            if (static_cast<quint8>(m_buffer.at(i)) == 0xEF &&
                static_cast<quint8>(m_buffer.at(i + 1)) == 0x01) {
                return i;
            }
        }
        return -1;
    }

    QByteArray m_buffer;
};

} // namespace accessterminal

#endif // FINGERPRINT_FRAME_PARSER_H
