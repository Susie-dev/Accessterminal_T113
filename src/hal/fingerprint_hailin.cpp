// 引入项目内部头文件 "HailinFingerprint.h"。
#include "hal/fingerprint_hailin.h"
// 引入系统或第三方头文件 <QDebug>。
#include <QDebug>
// 引入系统或第三方头文件 <QThread>。
#include <QThread>

// 定义类 HailinFingerprint 的成员函数 HailinFingerprint。
HailinFingerprint::HailinFingerprint(const QString& portName, QObject* parent)
    // 继续当前代码块的参数、表达式或结构定义。
    : QObject(parent), m_portName(portName)
// 进入新的代码块作用域。
{
    // 执行本行语句完成对应变量、对象或业务状态处理。
    m_serial.setPortName(m_portName);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    m_serial.setBaudRate(QSerialPort::Baud57600);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    m_serial.setDataBits(QSerialPort::Data8);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    m_serial.setParity(QSerialPort::NoParity);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    m_serial.setStopBits(QSerialPort::OneStop);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    m_serial.setFlowControl(QSerialPort::NoFlowControl);
// 结束当前代码块作用域。
}

// 定义类 HailinFingerprint 的成员函数 ~HailinFingerprint。
HailinFingerprint::~HailinFingerprint() {
    // 关闭文件描述符或 socket。
    close();
// 结束当前代码块作用域。
}

// 定义或声明函数 HailinFingerprint::open。
bool HailinFingerprint::open() {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QMutexLocker locker(&m_mutex);
    // 根据条件判断是否执行该分支。
    if (m_serial.isOpen()) return true;
    // 根据条件判断是否执行该分支。
    if (!m_serial.open(QIODevice::ReadWrite)) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug(QString("[指纹] 打开串口失败: %1").arg(m_serial.errorString()));
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    debug(QString("[指纹] 串口打开成功: %1").arg(m_portName));
    // 新会话开始：清空解析状态与驱动侧残留字节。上电/重连时模块可能已在
    // 发送上一轮的应答，若不清空，这些旧字节会与本次请求的应答混在一起，
    // 让“发一帧收一帧”的时序错位。
    // 执行本行语句完成对应变量、对象或业务状态处理。
    m_parser.reset();
    // 执行本行语句完成对应变量、对象或业务状态处理。
    m_pendingFrames.clear();
    // 执行本行语句完成对应变量、对象或业务状态处理。
    m_serial.clear();
    // 返回当前函数的执行结果或结束函数。
    return true;
// 结束当前代码块作用域。
}

// 定义或声明函数 HailinFingerprint::close。
void HailinFingerprint::close() {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QMutexLocker locker(&m_mutex);
    // 根据条件判断是否执行该分支。
    if (m_serial.isOpen()) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        m_serial.close();
        // 关闭时清空解析状态：半包与排队帧都属于上一个会话，若留到下次
        // 打开，会把旧字节拼到新会话的数据前，导致解析持续错位。
        // 执行本行语句完成对应变量、对象或业务状态处理。
        m_parser.reset();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        m_pendingFrames.clear();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 串口已关闭");
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// 定义或声明函数 HailinFingerprint::isOpen。
bool HailinFingerprint::isOpen() const {
    // 返回当前函数的执行结果或结束函数。
    return m_serial.isOpen();
// 结束当前代码块作用域。
}

// 自动识别（1:N）
// 定义或声明函数 HailinFingerprint::autoIdentify。
bool HailinFingerprint::autoIdentify(quint8 scoreLevel, int timeoutMs) {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QMutexLocker locker(&m_mutex);
    // 根据条件判断是否执行该分支。
    if (!m_serial.isOpen()) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 串口未打开");
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray payload;
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append(0x32); // 指令码
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append(scoreLevel); // 分数等级
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)0xFF); // ID高
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)0xFF); // ID低
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)0x00); // 参数
    // 包长度=8=指令码+分数等级+ID高+ID低+参数+校验和(2)+包长度(2)
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray pkt = buildPacket(payload);
    // 根据条件判断是否执行该分支。
    if (!sendPacket(pkt)) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 自动识别指令发送失败");
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray resp = readPacket(timeoutMs);
    // 根据条件判断是否执行该分支。
    if (resp.isEmpty()) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 自动识别无应答");
        // 发出 Qt 信号 identifyResult 通知其他对象。
        emit identifyResult(false, -1, 0, "无应答");
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 长度校验：下面要读到 resp[14]（score 低字节），因此至少需要 15 字节。
    //
    // 为什么必须显式判断：enable 定帧的 FingerprintFrameParser 只保证
    // **长度字段自洽**，最短合法帧是 9(包头+地址+标识+长度) + 2(校验和)
    // = 11 字节。也就是说 resp 完全可能是 11~14 字节的合法帧，此时
    // resp[11]~resp[14] 越界。QByteArray::operator[] 越界是未定义行为
    // （不像 QString 那样返回空引用），必须挡住。
    //
    // 旧的 `resp.isEmpty()` 只挡住了 0 字节，挡不住这种“短但合法”的帧。
    // stepIdentify() 用 `resp.size() < 16` 做同类校验（同为读取到
    // resp[14]，那里多留了 1 字节余量）；此处按实际需要取 15。
    // 根据条件判断是否执行该分支。
    if (resp.size() < 15) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug(QString("[指纹] 自动识别应答过短(%1字节)，忽略").arg(resp.size()));
        // 发出 Qt 信号 identifyResult 通知其他对象。
        emit identifyResult(false, -1, 0, "应答过短");
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 解析应答
    // 根据条件判断是否执行该分支。
    if ((quint8)resp[6] != PKT_ACK) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 应答包标识错误");
        // 发出 Qt 信号 identifyResult 通知其他对象。
        emit identifyResult(false, -1, 0, "应答包标识错误");
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint8 confirm = (quint8)resp[9];
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint8 param = (quint8)resp[10];
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint16 id = ((quint8)resp[11] << 8) | (quint8)resp[12];
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint16 score = ((quint8)resp[13] << 8) | (quint8)resp[14];
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QString msg = QString("确认码=%1, param=%2, id=%3, score=%4").arg(confirm,2,16,QChar('0')).arg(param,2,16,QChar('0')).arg(id).arg(score);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    debug("[指纹] 自动识别应答: " + msg);
    // 根据条件判断是否执行该分支。
    if (confirm == 0x00 && param == 0x05) {
        // 发出 Qt 信号 identifyResult 通知其他对象。
        emit identifyResult(true, id, score, "识别成功");
        // 返回当前函数的执行结果或结束函数。
        return true;
    // 结束当前代码块作用域。
    } else {
        // 发出 Qt 信号 identifyResult 通知其他对象。
        emit identifyResult(false, -1, 0, msg);
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// 自动注册
// 执行本行语句完成对应变量、对象或业务状态处理。
enum { ENROLL_PARAM_DEFAULT = 0x00 };
// 定义或声明函数 HailinFingerprint::autoEnroll。
bool HailinFingerprint::autoEnroll(quint16 id, quint8 enrollTimes, quint8 param, int timeoutMs) {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QMutexLocker locker(&m_mutex);
    // 根据条件判断是否执行该分支。
    if (!m_serial.isOpen()) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 串口未打开");
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray payload;
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append(0x31); // 指令码
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)(id >> 8)); // ID高
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)(id & 0xFF)); // ID低
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append(enrollTimes); // 录入次数
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append(param); // 参数
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray pkt = buildPacket(payload);
    // 根据条件判断是否执行该分支。
    if (!sendPacket(pkt)) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 自动注册指令发送失败");
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 注册流程有多步应答，需循环读取
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int elapsed = 0;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int step = 0;
    // 开始循环处理满足条件的数据或状态。
    while (elapsed < timeoutMs) {
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray resp = readPacket(2000);
        // 根据条件判断是否执行该分支。
        if (resp.isEmpty()) {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            elapsed += 2000;
            // 跳过本轮循环剩余逻辑并进入下一轮。
            continue;
        // 结束当前代码块作用域。
        }
        // 根据条件判断是否执行该分支。
        if ((quint8)resp[6] != PKT_ACK) {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            debug("[指纹] 注册应答包标识错误");
            // 发出 Qt 信号 enrollResult 通知其他对象。
            emit enrollResult(false, id, "应答包标识错误");
            // 返回当前函数的执行结果或结束函数。
            return false;
        // 结束当前代码块作用域。
        }
        // 长度校验：下面要读 resp[9]/resp[10]/resp[11]，至少需要 12 字节。
        // 解析器只保证「长度字段自洽」，最短合法帧是 9 + 2(校验和) = 11 字节，
        // 此时 resp[11] 越界。stepRegister()/stepIdentify() 早已用
        // `resp.size() < 12` 做同样校验，此处原本漏掉了，保持一致补上。
        // 注意 QByteArray::operator[] 越界是未定义行为（非 QString 那种
        // 返回空引用），必须显式判断。
        // 根据条件判断是否执行该分支。
        if (resp.size() < 12) {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            debug("[指纹] 注册应答过短，忽略");
            // 必须推进 elapsed：下面的 continue 会跳过循环末尾的
            // elapsed += 2000，若模块持续吐短帧，超时判断就永远不会成立。
            // 执行本行语句完成对应变量、对象或业务状态处理。
            elapsed += 2000;
            // 跳过本轮循环剩余逻辑并进入下一轮。
            continue;
        // 结束当前代码块作用域。
        }
        // 执行本行语句完成对应变量、对象或业务状态处理。
        quint8 confirm = (quint8)resp[9];
        // 执行本行语句完成对应变量、对象或业务状态处理。
        quint8 param1 = (quint8)resp[10];
        // 执行本行语句完成对应变量、对象或业务状态处理。
        quint8 param2 = (quint8)resp[11];
        // 执行本行语句完成对应变量、对象或业务状态处理。
        QString msg = QString("step=%1, 确认码=%2, param1=%3, param2=%4").arg(step++).arg(confirm,2,16,QChar('0')).arg(param1,2,16,QChar('0')).arg(param2,2,16,QChar('0'));
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 自动注册应答: " + msg);
        // 根据条件判断是否执行该分支。
        if (confirm == 0x00 && param1 == 0x06) { // 存储模板成功
            // 发出 Qt 信号 enrollResult 通知其他对象。
            emit enrollResult(true, id, "注册成功");
            // 返回当前函数的执行结果或结束函数。
            return true;
        // 结束当前代码块作用域。
        } else if (confirm == 0x01 || confirm == 0x0A || confirm == 0x0B || confirm == 0x1F || confirm == 0x22 || confirm == 0x25 || confirm == 0x26 || confirm == 0x27) {
            // 发出 Qt 信号 enrollResult 通知其他对象。
            emit enrollResult(false, id, msg);
            // 返回当前函数的执行结果或结束函数。
            return false;
        // 结束当前代码块作用域。
        }
        // 其它步骤继续等待
        // 执行本行语句完成对应变量、对象或业务状态处理。
        elapsed += 2000;
    // 结束当前代码块作用域。
    }
    // 发出 Qt 信号 enrollResult 通知其他对象。
    emit enrollResult(false, id, "注册超时");
    // 返回当前函数的执行结果或结束函数。
    return false;
// 结束当前代码块作用域。
}

// 取消当前操作
// 定义或声明函数 HailinFingerprint::cancel。
bool HailinFingerprint::cancel() {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QMutexLocker locker(&m_mutex);
    // 根据条件判断是否执行该分支。
    if (!m_serial.isOpen()) return false;
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray payload;
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append(0x30); // 指令码
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray pkt = buildPacket(payload);
    // 根据条件判断是否执行该分支。
    if (!sendPacket(pkt)) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 取消指令发送失败");
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray resp = readPacket(1000);
    // 根据条件判断是否执行该分支。
    if (resp.isEmpty()) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 取消无应答");
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 长度校验：下面要读 resp[9]，至少需要 10 字节（最短合法帧为 11 字节，
    // 但这里按实际访问下标判断，留出最小必要长度）。理由同上：
    // QByteArray::operator[] 越界是未定义行为。
    // 根据条件判断是否执行该分支。
    if (resp.size() < 10) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug(QString("[指纹] 取消应答过短(%1字节)，忽略").arg(resp.size()));
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint8 confirm = (quint8)resp[9];
    // 执行本行语句完成对应变量、对象或业务状态处理。
    debug(QString("[指纹] 取消应答: 确认码=%1").arg(confirm,2,16,QChar('0')));
    // 返回当前函数的执行结果或结束函数。
    return confirm == 0x00;
// 结束当前代码块作用域。
}

// 定义或声明函数 HailinFingerprint::buildPacket。
QByteArray HailinFingerprint::buildPacket(const QByteArray& payload) {
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray pkt;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)0xEF);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)0x01);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)0xFF);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)0xFF);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)0xFF);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)0xFF);
    // 继续当前代码块的参数、表达式或结构定义。
    pkt.append(PKT_CMD); // 包标识
    // 继续当前代码块的参数、表达式或结构定义。
    quint16 pktLen = payload.size() + 2; // 包长度=payload+校验和
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)(pktLen >> 8));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)(pktLen & 0xFF));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append(payload);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint16 sum = 0;
    // 校验和从包标识字段开始累加到 payload 末尾，不包含 0xEF01 包头和设备地址。
    // 开始循环处理满足条件的数据或状态。
    for (int i = 6; i < pkt.size(); ++i) sum += (quint8)pkt[i];
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)(sum >> 8));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt.append((char)(sum & 0xFF));
    // 返回当前函数的执行结果或结束函数。
    return pkt;
// 结束当前代码块作用域。
}

// 定义或声明函数 HailinFingerprint::sendPacket。
bool HailinFingerprint::sendPacket(const QByteArray& pkt) {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    qint64 written = m_serial.write(pkt);
    // 根据条件判断是否执行该分支。
    if (written != pkt.size()) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 串口写入不完整");
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if (!m_serial.waitForBytesWritten(200)) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        debug("[指纹] 串口写入超时");
        // 返回当前函数的执行结果或结束函数。
        return false;
    // 结束当前代码块作用域。
    }
    //debug("[指纹] 发送: " + pkt.toHex(' '));
    // 返回当前函数的执行结果或结束函数。
    return true;
// 结束当前代码块作用域。
}

// 定义或声明函数 HailinFingerprint::readPacket。
QByteArray HailinFingerprint::readPacket(int timeoutMs) {
    // 先把上次残留的完整帧消费掉。一次 feed() 可能切出多帧，而本函数每次
    // 只返回一帧，多出来的帧存在 m_pendingFrames 里，不能丢——否则表现为
    // “注册随机失败”：下一次调用会一直等一帧永远不来的应答而超时。
    if (!m_pendingFrames.isEmpty()) {
        return m_pendingFrames.takeFirst();
    }

    // 执行本行语句完成对应变量、对象或业务状态处理。
    QElapsedTimer timer;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    timer.start();
    // 开始循环处理满足条件的数据或状态。
    while (timer.elapsed() < timeoutMs) {
        // 根据条件判断是否执行该分支。
        if (m_serial.waitForReadyRead(100)) {
            // 交给解析器定帧：它跨调用保存半包，且一次喂入可切出多帧。
            // 原实现用局部 buf 累积并 `return buf.left(totalLen)`，会把
            // 同一次 readAll() 里的后续帧和超时时的半包一起丢弃。
            const QList<QByteArray> frames = m_parser.feed(m_serial.readAll());
            if (frames.isEmpty()) {
                // 本次没凑出完整帧，继续等（半包已留在解析器内部）。
                continue;
            }
            // 第一帧返回给调用方，其余排队供后续 readPacket() 取用。
            for (int i = 1; i < frames.size(); ++i) {
                m_pendingFrames.append(frames.at(i));
            }
            // 返回当前函数的执行结果或结束函数。
            return frames.at(0);
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    }
    // 超时：不返回半包（半包仍留在解析器中，下次喂入可继续拼），
    // 但若此时已有完整帧排队，仍应返回它而不是空。
    if (!m_pendingFrames.isEmpty()) {
        return m_pendingFrames.takeFirst();
    }
    // 返回当前函数的执行结果或结束函数。
    return QByteArray();
// 结束当前代码块作用域。
}

// 定义或声明函数 HailinFingerprint::calcChecksum。
quint16 HailinFingerprint::calcChecksum(const QByteArray& pkt) {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint16 sum = 0;
    // 开始循环处理满足条件的数据或状态。
    for (int i = 6; i < pkt.size(); ++i) sum += (quint8)pkt[i];
    // 返回当前函数的执行结果或结束函数。
    return sum;
// 结束当前代码块作用域。
}

// 定义或声明函数 HailinFingerprint::debug。
void HailinFingerprint::debug(const QString& msg) {
    // 输出调试日志。
    qDebug() << msg;
    // 发出 Qt 信号 debugInfo 通知其他对象。
    emit debugInfo(msg);
// 结束当前代码块作用域。
}

// 定义或声明函数 HailinFingerprint::stepRegister。
void HailinFingerprint::stepRegister(quint16 id, int collectTimes) {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QMutexLocker locker(&m_mutex);
    // 根据条件判断是否执行该分支。
    if (!m_serial.isOpen()) {
        // 发出 Qt 信号 stepRegisterStatus 通知其他对象。
        emit stepRegisterStatus("串口未打开", 0, false);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int step = 0;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int bufferId = 1;
    // 1. 多次采集图像和生成特征
    // 开始循环处理满足条件的数据或状态。
    for (; bufferId <= collectTimes; ++bufferId) {
        // 获取图像
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray payload;
        // 继续当前代码块的参数、表达式或结构定义。
        payload.append((char)0x01); // CMD_GET_IMAGE
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray pkt = buildPacket(payload);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        sendPacket(pkt);
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray resp = readPacket(2000);
        // 根据条件判断是否执行该分支。
        if (resp.size() < 12 || (quint8)resp[9] != 0x00) {
            // 发出 Qt 信号 stepRegisterStatus 通知其他对象。
            emit stepRegisterStatus(QString("第%1次采集图像失败").arg(bufferId), step++, false);
            // 返回当前函数的执行结果或结束函数。
            return;
        // 结束当前代码块作用域。
        }
        // 发出 Qt 信号 stepRegisterStatus 通知其他对象。
        emit stepRegisterStatus(QString("第%1次采集图像成功").arg(bufferId), step++, true);
        // 生成特征
        // 执行本行语句完成对应变量、对象或业务状态处理。
        payload.clear();
        // 继续当前代码块的参数、表达式或结构定义。
        payload.append((char)0x02); // CMD_GEN_CHAR
        // 继续当前代码块的参数、表达式或结构定义。
        payload.append((char)bufferId); // BufferID
        // 执行本行语句完成对应变量、对象或业务状态处理。
        pkt = buildPacket(payload);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        sendPacket(pkt);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        resp = readPacket(2000);
        // 根据条件判断是否执行该分支。
        if (resp.size() < 12 || (quint8)resp[9] != 0x00) {
            // 发出 Qt 信号 stepRegisterStatus 通知其他对象。
            emit stepRegisterStatus(QString("第%1次生成特征失败").arg(bufferId), step++, false);
            // 返回当前函数的执行结果或结束函数。
            return;
        // 结束当前代码块作用域。
        }
        // 发出 Qt 信号 stepRegisterStatus 通知其他对象。
        emit stepRegisterStatus(QString("第%1次生成特征成功").arg(bufferId), step++, true);
    // 结束当前代码块作用域。
    }
    // 2. 合并特征
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray payload;
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)0x05); // CMD_REG_MODEL
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray pkt = buildPacket(payload);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    sendPacket(pkt);
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray resp = readPacket(2000);
    // 根据条件判断是否执行该分支。
    if (resp.size() < 12 || (quint8)resp[9] != 0x00) {
        // 发出 Qt 信号 stepRegisterStatus 通知其他对象。
        emit stepRegisterStatus("合并特征失败", step++, false);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 发出 Qt 信号 stepRegisterStatus 通知其他对象。
    emit stepRegisterStatus("合并特征成功", step++, true);
    // 3. 存储模板
    // 执行本行语句完成对应变量、对象或业务状态处理。
    payload.clear();
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)0x06); // CMD_STORE_CHAR
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)1); // BufferID
    // 执行本行语句完成对应变量、对象或业务状态处理。
    payload.append((char)(id >> 8));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    payload.append((char)(id & 0xFF));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt = buildPacket(payload);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    sendPacket(pkt);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    resp = readPacket(2000);
    // 根据条件判断是否执行该分支。
    if (resp.size() < 12 || (quint8)resp[9] != 0x00) {
        // 发出 Qt 信号 stepRegisterStatus 通知其他对象。
        emit stepRegisterStatus("存储模板失败", step++, false);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 发出 Qt 信号 stepRegisterStatus 通知其他对象。
    emit stepRegisterStatus("存储模板成功", step++, true);
// 结束当前代码块作用域。
}

// 定义或声明函数 HailinFingerprint::stepIdentify。
void HailinFingerprint::stepIdentify(int startPage, int pageNum) {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QMutexLocker locker(&m_mutex);
    // 根据条件判断是否执行该分支。
    if (!m_serial.isOpen()) {
        // 发出 Qt 信号 stepIdentifyStatus 通知其他对象。
        emit stepIdentifyStatus("串口未打开", false, -1, 0);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 1. 获取图像
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray payload;
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)0x01); // CMD_GET_IMAGE
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray pkt = buildPacket(payload);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    sendPacket(pkt);
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray resp = readPacket(2000);
    // 根据条件判断是否执行该分支。
    if (resp.size() < 12 || (quint8)resp[9] != 0x00) {
        // 发出 Qt 信号 stepIdentifyStatus 通知其他对象。
        emit stepIdentifyStatus("采集图像失败", false, -1, 0);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 发出 Qt 信号 stepIdentifyStatus 通知其他对象。
    emit stepIdentifyStatus("采集图像成功", true, -1, 0);
    // 2. 生成特征
    // 执行本行语句完成对应变量、对象或业务状态处理。
    payload.clear();
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)0x02); // CMD_GEN_CHAR
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)1); // BufferID
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt = buildPacket(payload);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    sendPacket(pkt);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    resp = readPacket(2000);
    // 根据条件判断是否执行该分支。
    if (resp.size() < 12 || (quint8)resp[9] != 0x00) {
        // 发出 Qt 信号 stepIdentifyStatus 通知其他对象。
        emit stepIdentifyStatus("生成特征失败", false, -1, 0);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 发出 Qt 信号 stepIdentifyStatus 通知其他对象。
    emit stepIdentifyStatus("生成特征成功", true, -1, 0);
    // 3. 搜索指纹
    // 执行本行语句完成对应变量、对象或业务状态处理。
    payload.clear();
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)0x04); // CMD_SEARCH
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)1); // BufferID
    // 执行本行语句完成对应变量、对象或业务状态处理。
    payload.append((char)(startPage >> 8));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    payload.append((char)(startPage & 0xFF));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    payload.append((char)(pageNum >> 8));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    payload.append((char)(pageNum & 0xFF));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt = buildPacket(payload);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    sendPacket(pkt);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    resp = readPacket(2000);
    // 根据条件判断是否执行该分支。
    if (resp.size() < 16) {
        // 发出 Qt 信号 stepIdentifyStatus 通知其他对象。
        emit stepIdentifyStatus("搜索指纹失败", false, -1, 0);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint8 confirm = (quint8)resp[9];
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint16 id = ((quint8)resp[11] << 8) | (quint8)resp[12];
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint16 score = ((quint8)resp[13] << 8) | (quint8)resp[14];
    // 根据条件判断是否执行该分支。
    if (confirm == 0x00) {
        // 发出 Qt 信号 stepIdentifyStatus 通知其他对象。
        emit stepIdentifyStatus("识别成功", true, id, score);
    // 结束当前代码块作用域。
    } else {
        // 发出 Qt 信号 stepIdentifyStatus 通知其他对象。
        emit stepIdentifyStatus(QString("识别失败, 确认码=%1").arg(confirm,2,16,QChar('0')), false, -1, 0);
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}
