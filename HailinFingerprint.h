// 开始头文件防重复包含保护 HAILINFINGERPRINT_H。
#ifndef HAILINFINGERPRINT_H
// 定义预处理宏或常量 HAILINFINGERPRINT_H。
#define HAILINFINGERPRINT_H

// 引入系统或第三方头文件 <QObject>。
#include <QObject>
// 引入系统或第三方头文件 <QSerialPort>。
#include <QSerialPort>
// 引入系统或第三方头文件 <QMutex>。
#include <QMutex>
// 引入系统或第三方头文件 <QTimer>。
#include <QTimer>
// 引入系统或第三方头文件 <QElapsedTimer>。
#include <QElapsedTimer>

// 声明类 HailinFingerprint。
class HailinFingerprint : public QObject {
    // 继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
// 声明类成员的 public 访问权限区域。
public:
    // 声明显式构造函数 HailinFingerprint。
    explicit HailinFingerprint(const QString& portName = "/dev/ttyS4", QObject* parent = nullptr);
    // 声明或定义析构函数 HailinFingerprint。
    ~HailinFingerprint();

    // 定义或声明函数 open。
    bool open();
    // 定义或声明函数 close。
    void close();
    // 定义或声明函数 isOpen。
    bool isOpen() const;

    // 自动识别（1:N）
    // 定义或声明函数 autoIdentify。
    bool autoIdentify(quint8 scoreLevel = 2, int timeoutMs = 2000);
    // 自动注册
    // 定义或声明函数 autoEnroll。
    bool autoEnroll(quint16 id, quint8 enrollTimes = 2, quint8 param = 0, int timeoutMs = 10000);
    // 取消当前操作
    // 定义或声明函数 cancel。
    bool cancel();

    // 分步注册和识别
    // 定义或声明函数 stepRegister。
    void stepRegister(quint16 id = 1, int collectTimes = 3);
    // 定义或声明函数 stepIdentify。
    void stepIdentify(int startPage = 1, int pageNum = 1);

    // 线程安全
    // 定义或声明函数 mutex。
    QMutex* mutex() { return &m_mutex; }

    // 指纹模块串口协议封装：包头 + 地址 + 包标识 + 长度 + payload + 校验和。
    // 定义或声明函数 buildPacket。
    QByteArray buildPacket(const QByteArray& payload);
    // 定义或声明函数 sendPacket。
    bool sendPacket(const QByteArray& pkt);
    // 定义或声明函数 readPacket。
    QByteArray readPacket(int timeoutMs = 1000);

// 声明 Qt 信号槽成员区域。
signals:
    // 定义或声明函数 identifyResult。
    void identifyResult(bool success, int id, int score, const QString& msg);
    // 定义或声明函数 enrollResult。
    void enrollResult(bool success, int id, const QString& msg);
    // 定义或声明函数 debugInfo。
    void debugInfo(const QString& info);
    // 定义或声明函数 stepRegisterStatus。
    void stepRegisterStatus(const QString& msg, int step, bool ok);
    // 定义或声明函数 stepIdentifyStatus。
    void stepIdentifyStatus(const QString& msg, bool ok, int id, int score);

// 声明类成员的 private 访问权限区域。
private:
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QSerialPort m_serial;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QString m_portName;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QMutex m_mutex;

    // 定义或声明函数 calcChecksum。
    quint16 calcChecksum(const QByteArray& pkt);
    // 定义或声明函数 debug。
    void debug(const QString& msg);

    // 协议相关：默认广播地址 0xFFFFFFFF，命令包 0x01，应答包 0x07。
    // 声明静态变量或静态常量，保持跨调用状态。
    static constexpr quint32 DEVICE_ADDR = 0xFFFFFFFF;
    // 声明静态变量或静态常量，保持跨调用状态。
    static constexpr quint8 PKT_CMD = 0x01;
    // 声明静态变量或静态常量，保持跨调用状态。
    static constexpr quint8 PKT_ACK = 0x07;
// 结束当前代码块作用域。
};

// 结束当前预处理条件块。
#endif // HAILINFINGERPRINT_H
