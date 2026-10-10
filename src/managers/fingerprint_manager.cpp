// 引入项目内部头文件 "fingerprint_manager.h"。
#include "managers/fingerprint_manager.h"
// 引入项目内部头文件 "HailinFingerprint.h"。
#include "hal/fingerprint_hailin.h"
// 引入系统或第三方头文件 <QTimer>。
#include <QTimer>
// 引入系统或第三方头文件 <QDateTime>。
#include <QDateTime>
// 引入系统或第三方头文件 <QDebug>。
#include <QDebug>
// 引入系统或第三方头文件 <QSharedPointer>。
#include <QSharedPointer>

// 定义类 FingerprintManager 的成员函数 FingerprintManager。
FingerprintManager::FingerprintManager(QObject* parent)
    // 继续当前代码块的参数、表达式或结构定义。
    : QObject(parent)
// 进入新的代码块作用域。
{
// 结束当前代码块作用域。
}

// 定义类 FingerprintManager 的成员函数 ~FingerprintManager。
FingerprintManager::~FingerprintManager()
{
    // 必须显式停表：轮询回调里 readPacket() 是跑在 GUI 线程上的阻塞读，
    // 而定时器以 this 为 parent，会在 QObject 析构过程中被销毁——此时
    // 排队的 timeout 事件可能仍在事件队列里，回调再去访问已在拆解的
    // 成员（fingerprint / m_pollTimer）就是 use-after-free。
    stopPoll();
}

// 定义或声明函数 FingerprintManager::stopPoll。
void FingerprintManager::stopPoll()
{
    if (m_pollTimer) {
        m_pollTimer->stop();
        m_pollTimer->deleteLater();
        m_pollTimer = nullptr;
    }
    // 注册轮询同样自带阻塞读，也必须能被停掉。
    if (m_enrollTimer) {
        m_enrollTimer->stop();
        m_enrollTimer->deleteLater();
        m_enrollTimer = nullptr;
        m_enrolling = false;
    }
}

// 定义或声明函数 FingerprintManager::setSuspended。
void FingerprintManager::setSuspended(bool suspended)
{
    if (m_suspended == suspended) return;
    m_suspended = suspended;
    if (m_suspended) {
        // 立刻停止抢读，并解除“等待手指”状态，使恢复后能重新进入。
        stopPoll();
        m_waitingFinger = false;
        return;
    }
    // 恢复常驻识别。用 0ms 延时而不是直接调用：本函数通常是在注册流程
    // 的收尾路径里被调用的，直接递归进入 startIdentify() 会在仍有
    // 串口事务在栈上时插队下发新命令。
    QTimer::singleShot(0, this, [this]() {
        if (!m_suspended) startIdentify();
    });
}

// 定义或声明函数 FingerprintManager::setDevice。
void FingerprintManager::setDevice(HailinFingerprint* device)
// 进入新的代码块作用域。
{
    // 执行本行语句完成对应变量、对象或业务状态处理。
    fingerprint = device;
// 结束当前代码块作用域。
}

// 定义或声明函数 FingerprintManager::startIdentify。
void FingerprintManager::startIdentify()
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if (!fingerprint) return;
    // 注册流程进行中：不得抢串口（见 setSuspended 的说明）。
    if (m_suspended) return;
    // 只允许一个识别流程
    if (m_waitingFinger) return;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    m_waitingFinger = true;

    // 只在开始时提示一次
    // 发出 Qt 信号 identifyResult 通知其他对象。
    emit identifyResult(false, -1, 0, "请按下指纹进行识别"); // UI可用此信号做提示音

    // 复用同一个成员定时器：重复进入本函数时不要叠加新定时器。
    stopPoll();
    // 创建或使用 Qt 定时器对象。
    m_pollTimer = new QTimer(this);
    // 把句柄按值捕获进 lambda，回调里操作的就是本次这一只定时器。
    QTimer* const timer = m_pollTimer;
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(timer, &QTimer::timeout, this, [this, timer]() {
        // 注册流程可能在本回调排队后被启动（setSuspended 会 stopPoll，
        // 但已进入事件队列的 timeout 仍会执行一次）。此时必须立刻让路，
        // 否则会与注册流程抢同一个串口的应答。
        if (m_suspended) {
            stopPoll();
            m_waitingFinger = false;
            return;
        }
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray payload;
        // 继续当前代码块的参数、表达式或结构定义。
        payload.append((char)0x01); // CMD_GET_IMAGE
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray pkt = fingerprint->buildPacket(payload);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        fingerprint->sendPacket(pkt);
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray resp = fingerprint->readPacket(500);
        // 根据条件判断是否执行该分支。
        if (resp.size() >= 12 && (quint8)resp[9] == 0x00) {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            timer->stop();
            // 执行本行语句完成对应变量、对象或业务状态处理。
            timer->deleteLater();
            // 成员句柄同步清空，保持“句柄有效 <=> 定时器在跑”这一不变式。
            if (m_pollTimer == timer) m_pollTimer = nullptr;
            // 生成特征
            // 创建或使用字节数组保存协议或音视频数据。
            QByteArray payload2;
            // 继续当前代码块的参数、表达式或结构定义。
            payload2.append((char)0x02); // CMD_GEN_CHAR
            // 继续当前代码块的参数、表达式或结构定义。
            payload2.append((char)1);    // BufferID
            // 创建或使用字节数组保存协议或音视频数据。
            QByteArray pkt2 = fingerprint->buildPacket(payload2);
            // 执行本行语句完成对应变量、对象或业务状态处理。
            fingerprint->sendPacket(pkt2);
            // 创建或使用字节数组保存协议或音视频数据。
            QByteArray resp2 = fingerprint->readPacket(2000);
            // 根据条件判断是否执行该分支。
            if (resp2.size() < 12 || (quint8)resp2[9] != 0x00) {
                // 发出 Qt 信号 identifyResult 通知其他对象。
                emit identifyResult(false, -1, 0, "生成特征失败");
                // 执行本行语句完成对应变量、对象或业务状态处理。
                m_waitingFinger = false;
                // 返回当前函数的执行结果或结束函数。
                return;
            // 结束当前代码块作用域。
            }
            // 搜索指纹
            // 创建或使用字节数组保存协议或音视频数据。
            QByteArray payload3;
            // 继续当前代码块的参数、表达式或结构定义。
            payload3.append((char)0x04); // CMD_SEARCH
            // 继续当前代码块的参数、表达式或结构定义。
            payload3.append((char)1);    // BufferID
            // 继续当前代码块的参数、表达式或结构定义。
            payload3.append((char)0x00); // PageID high
            // 继续当前代码块的参数、表达式或结构定义。
            payload3.append((char)0x01); // PageID low
            // 继续当前代码块的参数、表达式或结构定义。
            payload3.append((char)0x00); // Num high
            // 继续当前代码块的参数、表达式或结构定义。
            payload3.append((char)0x01); // Num low
            // 创建或使用字节数组保存协议或音视频数据。
            QByteArray pkt3 = fingerprint->buildPacket(payload3);
            // 执行本行语句完成对应变量、对象或业务状态处理。
            fingerprint->sendPacket(pkt3);
            // 创建或使用字节数组保存协议或音视频数据。
            QByteArray resp3 = fingerprint->readPacket(2000);
            // 根据条件判断是否执行该分支。
            if (resp3.size() >= 16 && (quint8)resp3[9] == 0x00) {
                // 执行本行语句完成对应变量、对象或业务状态处理。
                int id = ((quint8)resp3[11] << 8) | (quint8)resp3[12];
                // 执行本行语句完成对应变量、对象或业务状态处理。
                int score = ((quint8)resp3[13] << 8) | (quint8)resp3[14];
                // 输出调试日志。
                qDebug() << "指纹识别成功";
                // 发出 Qt 信号 identifyResult 通知其他对象。
                emit identifyResult(true, id, score, "指纹识别成功");
            // 结束当前代码块作用域。
            } else {
                // 输出调试日志。
                qDebug() << "指纹识别失败";
                // 发出 Qt 信号 identifyResult 通知其他对象。
                emit identifyResult(false, -1, 0, "指纹识别失败");
            // 结束当前代码块作用域。
            }
            // 执行本行语句完成对应变量、对象或业务状态处理。
            m_waitingFinger = false;
        // 结束当前代码块作用域。
        }
        // 否则继续等待
    // 结束当前代码块作用域。
    });
    // 执行本行语句完成对应变量、对象或业务状态处理。
    timer->start(500);
// 结束当前代码块作用域。
}


// 定义或声明函数 FingerprintManager::startEnroll。
void FingerprintManager::startEnroll(int id)
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if (!fingerprint) return;
    // 注册流程进行中：不得抢串口（见 setSuspended 的说明）。
    if (m_suspended) return;
    // 根据条件判断是否执行该分支。
    if (m_enrolling) return;
    // 先清理上一轮的定时器，再置守卫。
    // 顺序很关键：stopPoll() 会把 m_enrolling 复位为 false，若放在置位之后
    // 调用，刚设好的重入保护就被自己抹掉了。
    stopPoll();
    // 执行本行语句完成对应变量、对象或业务状态处理。
    m_enrolling = true;

    // 继续当前代码块的参数、表达式或结构定义。
    // 用 QSharedPointer 而不是裸堆指针：lambda 需要修改它，但裸指针在
    // “对象被销毁”等提前返回路径上会泄漏（baseline 只在成功路径 delete）。
    // 共享指针由 lambda 按值捕获，任何退出路径都会自动释放。
    QSharedPointer<int> currentStep(new int(1));
    // 发出 Qt 信号 enrollResult 通知其他对象。
    emit enrollResult(false, id, "请按下手指进行第1次采集");

    // 创建或使用 Qt 定时器对象。
    m_enrollTimer = new QTimer(this);
    // 把句柄按值捕获进 lambda，回调里操作的就是本次这一只定时器。
    QTimer* const timer = m_enrollTimer;
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(timer, &QTimer::timeout, this, [this, timer, id, currentStep]() {
        // 与识别轮询同理：注册可能被挂起（对象析构/外部暂停），
        // 已排队的 timeout 仍会执行一次，必须先让路。
        if (m_suspended) {
            stopPoll();
            m_enrolling = false;
            return;
        }
        // 1. 采集图像
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray payload;
        // 继续当前代码块的参数、表达式或结构定义。
        payload.append((char)0x01); // CMD_GET_IMAGE
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray pkt = fingerprint->buildPacket(payload);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        fingerprint->sendPacket(pkt);
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray resp = fingerprint->readPacket(500);
        // 根据条件判断是否执行该分支。
        if (resp.size() >= 12 && (quint8)resp[9] == 0x00) {
            // 2. 生成特征
            // 创建或使用字节数组保存协议或音视频数据。
            QByteArray payload2;
            // 继续当前代码块的参数、表达式或结构定义。
            payload2.append((char)0x02); // CMD_GEN_CHAR
            // 继续当前代码块的参数、表达式或结构定义。
            payload2.append((char)(*currentStep)); // BufferID: 1,2,3
            // 创建或使用字节数组保存协议或音视频数据。
            QByteArray pkt2 = fingerprint->buildPacket(payload2);
            // 执行本行语句完成对应变量、对象或业务状态处理。
            fingerprint->sendPacket(pkt2);
            // 创建或使用字节数组保存协议或音视频数据。
            QByteArray resp2 = fingerprint->readPacket(2000);
            // 根据条件判断是否执行该分支。
            if (resp2.size() < 12 || (quint8)resp2[9] != 0x00) {
                // 发出 Qt 信号 enrollResult 通知其他对象。
                emit enrollResult(false, id, QString("第%1次生成特征失败").arg(*currentStep));
                // 执行本行语句完成对应变量、对象或业务状态处理。
                m_enrolling = false;
                // 执行本行语句完成对应变量、对象或业务状态处理。
                timer->stop();
                // 执行本行语句完成对应变量、对象或业务状态处理。
                timer->deleteLater();
                if (m_enrollTimer == timer) m_enrollTimer = nullptr;
                // 返回当前函数的执行结果或结束函数。
                return;
            // 结束当前代码块作用域。
            }
            // 发出 Qt 信号 enrollResult 通知其他对象。
            emit enrollResult(true, id, QString("第%1次采集成功").arg(*currentStep));
            // 执行本行语句完成对应变量、对象或业务状态处理。
            (*currentStep)++;
            // 根据条件判断是否执行该分支。
            if (*currentStep > 3) {
                // 合并特征
                // 创建或使用字节数组保存协议或音视频数据。
                QByteArray payload3;
                // 继续当前代码块的参数、表达式或结构定义。
                payload3.append((char)0x05); // CMD_REG_MODEL
                // 创建或使用字节数组保存协议或音视频数据。
                QByteArray pkt3 = fingerprint->buildPacket(payload3);
                // 执行本行语句完成对应变量、对象或业务状态处理。
                fingerprint->sendPacket(pkt3);
                // 创建或使用字节数组保存协议或音视频数据。
                QByteArray resp3 = fingerprint->readPacket(2000);
                // 根据条件判断是否执行该分支。
                if (resp3.size() < 12 || (quint8)resp3[9] != 0x00) {
                    // 发出 Qt 信号 enrollResult 通知其他对象。
                    emit enrollResult(false, id, "合并特征失败");
                    // 执行本行语句完成对应变量、对象或业务状态处理。
                    m_enrolling = false;
                    // 执行本行语句完成对应变量、对象或业务状态处理。
                    timer->stop();
                    // 执行本行语句完成对应变量、对象或业务状态处理。
                    timer->deleteLater();
                    if (m_enrollTimer == timer) m_enrollTimer = nullptr;
                    // 返回当前函数的执行结果或结束函数。
                    return;
                // 结束当前代码块作用域。
                }
                // 存储模板
                // 创建或使用字节数组保存协议或音视频数据。
                QByteArray payload4;
                // 继续当前代码块的参数、表达式或结构定义。
                payload4.append((char)0x06); // CMD_STORE_CHAR
                // 继续当前代码块的参数、表达式或结构定义。
                payload4.append((char)1); // BufferID
                // 执行本行语句完成对应变量、对象或业务状态处理。
                payload4.append((char)(id >> 8));
                // 执行本行语句完成对应变量、对象或业务状态处理。
                payload4.append((char)(id & 0xFF));
                // 创建或使用字节数组保存协议或音视频数据。
                QByteArray pkt4 = fingerprint->buildPacket(payload4);
                // 执行本行语句完成对应变量、对象或业务状态处理。
                fingerprint->sendPacket(pkt4);
                // 创建或使用字节数组保存协议或音视频数据。
                QByteArray resp4 = fingerprint->readPacket(2000);
                // 根据条件判断是否执行该分支。
                if (resp4.size() < 12 || (quint8)resp4[9] != 0x00) {
                    // 发出 Qt 信号 enrollResult 通知其他对象。
                    emit enrollResult(false, id, "存储模板失败");
                // 结束当前代码块作用域。
                } else {
                    // 发出 Qt 信号 enrollResult 通知其他对象。
                    emit enrollResult(true, id, "注册成功");
                // 结束当前代码块作用域。
                }
                // 执行本行语句完成对应变量、对象或业务状态处理。
                m_enrolling = false;
                // 执行本行语句完成对应变量、对象或业务状态处理。
                timer->stop();
                // 执行本行语句完成对应变量、对象或业务状态处理。
                timer->deleteLater();
                if (m_enrollTimer == timer) m_enrollTimer = nullptr;
                // 返回当前函数的执行结果或结束函数。
                return;
            // 结束当前代码块作用域。
            } else {
                // 发出 Qt 信号 enrollResult 通知其他对象。
                emit enrollResult(false, id, QString("请抬手后再按下，进行第%1次采集").arg(*currentStep));
            // 结束当前代码块作用域。
            }
        // 结束当前代码块作用域。
        }
        // 否则继续等待
    // 结束当前代码块作用域。
    });
    // 执行本行语句完成对应变量、对象或业务状态处理。
    timer->start(500);
// 结束当前代码块作用域。
}
