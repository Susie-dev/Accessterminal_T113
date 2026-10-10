/**
 * @file fake_fingerprint_device.cpp
 * @brief `HailinFingerprint` 的链接期替身实现（见头文件说明）。
 *
 * 关键约束：**buildPacket() 必须是真实算法**。报文字节是唯一需要逐字节
 * 守住的契约，如果连这里都换成"简化版"，测试就退化成"替身和自己比"。
 * 因此下面的实现是从 src/hal/fingerprint_hailin.cpp 逐字节照搬的
 * （包头 0xEF01 + 地址 0xFFFFFFFF + 标识 0x01 + 大端长度 + payload +
 * 大端校验和，校验和自下标 6 起算）。
 *
 * 另有两个刻意的行为约定，用来暴露"只在真实硬件上才会发生"的情形：
 *   - readPacket() 的出队顺序严格 FIFO；
 *   - 队列为空时返回空 QByteArray（而非阻塞等待），使超时路径可确定性复现。
 */
#include <QMutex>
#include <QMutexLocker>
#include <QSet>

#include "hal/fingerprint_hailin.h"

#include "fake_fingerprint_device.h"

namespace {

QMutex g_mutex;

QList<QByteArray> g_sent;        ///< 下发的完整帧
QList<QByteArray> g_pending;     ///< 待取走的应答帧
QList<int> g_readTimeouts;       ///< readPacket 的 timeout 参数序列
int g_readCount = 0;
int g_buildCount = 0;
int g_sendFailFirst = 0;

/// 哪些替身实例处于"已打开"状态。
///
/// 用外部集合而不是给 HailinFingerprint 加一个 m_open 成员：后者属于
/// **改动生产头文件**，而本替身的设计前提就是"生产代码零改动"。
QSet<const HailinFingerprint*> g_openDevices;

/// 与真实 buildPacket() 完全一致（下标 6 起累加，不含包头与设备地址）。
QByteArray buildFrame(const QByteArray& payload)
{
    QByteArray pkt;
    pkt.append((char)0xEF);
    pkt.append((char)0x01);
    pkt.append((char)0xFF);
    pkt.append((char)0xFF);
    pkt.append((char)0xFF);
    pkt.append((char)0xFF);
    pkt.append((char)0x01); // PKT_CMD
    const quint16 pktLen = static_cast<quint16>(payload.size() + 2);
    pkt.append((char)(pktLen >> 8));
    pkt.append((char)(pktLen & 0xFF));
    pkt.append(payload);
    quint16 sum = 0;
    for (int i = 6; i < pkt.size(); ++i) {
        sum = static_cast<quint16>(sum + static_cast<quint8>(pkt.at(i)));
    }
    pkt.append((char)(sum >> 8));
    pkt.append((char)(sum & 0xFF));
    return pkt;
}

} // namespace

namespace fakefp {

void reset()
{
    QMutexLocker locker(&g_mutex);
    g_sent.clear();
    g_pending.clear();
    g_readTimeouts.clear();
    g_readCount = 0;
    g_buildCount = 0;
    g_sendFailFirst = 0;
}

QList<QByteArray> sent()
{
    QMutexLocker locker(&g_mutex);
    return g_sent;
}

int sentCount()
{
    QMutexLocker locker(&g_mutex);
    return g_sent.size();
}

void setSendFailFirst(int n)
{
    QMutexLocker locker(&g_mutex);
    g_sendFailFirst = n;
}

QByteArray sentPayload(int index)
{
    QMutexLocker locker(&g_mutex);
    if (index < 0 || index >= g_sent.size()) {
        return QByteArray();
    }
    const QByteArray& f = g_sent.at(index);
    if (f.size() < 11) {
        return QByteArray();
    }
    // 去掉 9 字节包头与 2 字节校验和。
    return f.mid(9, f.size() - 11);
}

void pushResponse(const QByteArray& fullFrame)
{
    QMutexLocker locker(&g_mutex);
    g_pending.append(fullFrame);
}

void pushAck(const QByteArray& payload)
{
    QMutexLocker locker(&g_mutex);
    // 应答帧的标识字节是 0x07（与真实模块一致）。
    QByteArray pkt;
    pkt.append((char)0xEF);
    pkt.append((char)0x01);
    pkt.append((char)0xFF);
    pkt.append((char)0xFF);
    pkt.append((char)0xFF);
    pkt.append((char)0xFF);
    pkt.append((char)0x07); // PKT_ACK
    const quint16 pktLen = static_cast<quint16>(payload.size() + 2);
    pkt.append((char)(pktLen >> 8));
    pkt.append((char)(pktLen & 0xFF));
    pkt.append(payload);
    quint16 sum = 0;
    for (int i = 6; i < pkt.size(); ++i) {
        sum = static_cast<quint16>(sum + static_cast<quint8>(pkt.at(i)));
    }
    pkt.append((char)(sum >> 8));
    pkt.append((char)(sum & 0xFF));
    g_pending.append(pkt);
}

void pushConfirm(quint8 confirm)
{
    QByteArray p;
    p.append((char)confirm);
    pushAck(p);
}

void pushSearchResult(quint8 confirm, quint16 id, quint16 score, quint8 param)
{
    // payload 落点：frame[9]=payload[0] ...
    // 故 order 必须是 confirm, param, id_hi, id_lo, score_hi, score_lo，
    // 才能让调用方读到的 frame[11..12]=id、frame[13..14]=score。
    QByteArray p;
    p.append((char)confirm);            // -> frame[9]
    p.append((char)param);              // -> frame[10]
    p.append((char)(id >> 8));          // -> frame[11]
    p.append((char)(id & 0xFF));        // -> frame[12]
    p.append((char)(score >> 8));       // -> frame[13]
    p.append((char)(score & 0xFF));     // -> frame[14]
    pushAck(p);
}

void pushConfirmParams(quint8 confirm, quint8 p1, quint8 p2)
{
    QByteArray p;
    p.append((char)confirm);
    p.append((char)p1);
    p.append((char)p2);
    pushAck(p);
}

void pushShortFrames(int n)
{
    // 11 字节：9 字节头部（长度字段 = 2，即"只有校验和"）+ 2 字节校验和。
    // 解析器认可它，但它连 resp[9] 都没有。
    for (int i = 0; i < n; ++i) {
        QByteArray p; // 空 payload
        pushAck(p);
    }
}

void pushFrameWithTag(quint8 tag, const QByteArray& payload)
{
    QMutexLocker locker(&g_mutex);
    QByteArray pkt;
    pkt.append((char)0xEF);
    pkt.append((char)0x01);
    pkt.append((char)0xFF);
    pkt.append((char)0xFF);
    pkt.append((char)0xFF);
    pkt.append((char)0xFF);
    pkt.append((char)tag); // 任意包标识
    const quint16 pktLen = static_cast<quint16>(payload.size() + 2);
    pkt.append((char)(pktLen >> 8));
    pkt.append((char)(pktLen & 0xFF));
    pkt.append(payload);
    quint16 sum = 0;
    for (int i = 6; i < pkt.size(); ++i) {
        sum = static_cast<quint16>(sum + static_cast<quint8>(pkt.at(i)));
    }
    pkt.append((char)(sum >> 8));
    pkt.append((char)(sum & 0xFF));
    g_pending.append(pkt);
}

int pendingCount()
{
    QMutexLocker locker(&g_mutex);
    return g_pending.size();
}

QByteArray takeRawResponse()
{
    QMutexLocker locker(&g_mutex);
    if (g_pending.isEmpty()) {
        return QByteArray();
    }
    return g_pending.takeFirst();
}

int readCount()
{
    QMutexLocker locker(&g_mutex);
    return g_readCount;
}

QList<int> readTimeouts()
{
    QMutexLocker locker(&g_mutex);
    return g_readTimeouts;
}

int buildCount()
{
    QMutexLocker locker(&g_mutex);
    return g_buildCount;
}

} // namespace fakefp

// ===========================================================================
// HailinFingerprint 成员定义（替身）
// ===========================================================================

HailinFingerprint::HailinFingerprint(const QString& portName, QObject* parent)
    : QObject(parent), m_portName(portName)
{
}

HailinFingerprint::~HailinFingerprint() {}

bool HailinFingerprint::open()
{
    QMutexLocker locker(&g_mutex);
    g_openDevices.insert(this);
    return true;
}

void HailinFingerprint::close()
{
    QMutexLocker locker(&g_mutex);
    g_openDevices.remove(this);
}

bool HailinFingerprint::isOpen() const
{
    QMutexLocker locker(&g_mutex);
    return g_openDevices.contains(this);
}

QByteArray HailinFingerprint::buildPacket(const QByteArray& payload)
{
    {
        QMutexLocker locker(&g_mutex);
        ++g_buildCount;
    }
    return buildFrame(payload);
}

bool HailinFingerprint::sendPacket(const QByteArray& pkt)
{
    QMutexLocker locker(&g_mutex);
    if (g_sendFailFirst > 0) {
        --g_sendFailFirst;
        return false;
    }
    g_sent.append(pkt);
    return true;
}

QByteArray HailinFingerprint::readPacket(int timeoutMs)
{
    {
        QMutexLocker locker(&g_mutex);
        ++g_readCount;
        g_readTimeouts.append(timeoutMs);
        if (!g_pending.isEmpty()) {
            return g_pending.takeFirst();
        }
    }
    // 队列为空：真实实现会阻塞到超时并返回空。这里直接返回空，
    // 使"模块不应答"这一路径可确定性复现，且不拖慢测试。
    return QByteArray();
}

quint16 HailinFingerprint::calcChecksum(const QByteArray& pkt)
{
    quint16 sum = 0;
    for (int i = 6; i < pkt.size(); ++i) {
        sum = static_cast<quint16>(sum + static_cast<quint8>(pkt.at(i)));
    }
    return sum;
}

void HailinFingerprint::debug(const QString& msg)
{
    emit debugInfo(msg);
}

// 其余公开流程（autoIdentify/autoEnroll/cancel/stepRegister/stepIdentify）
// 在管理器的测试里不被调用；给出定义以满足链接。
bool HailinFingerprint::autoIdentify(quint8, int) { return false; }
bool HailinFingerprint::autoEnroll(quint16, quint8, quint8, int) { return false; }
bool HailinFingerprint::cancel() { return false; }
void HailinFingerprint::stepRegister(quint16, int) {}
void HailinFingerprint::stepIdentify(int, int) {}
