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

// 定义类 FingerprintManager 的成员函数 FingerprintManager。
FingerprintManager::FingerprintManager(QObject* parent)
    // 继续当前代码块的参数、表达式或结构定义。
    : QObject(parent)
// 进入新的代码块作用域。
{
// 结束当前代码块作用域。
}

// 定义类 FingerprintManager 的成员函数 ~FingerprintManager。
FingerprintManager::~FingerprintManager() {}

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
    // 只允许一个识别流程
    // 声明静态变量或静态常量，保持跨调用状态。
    static bool waitingFinger = false;
    // 根据条件判断是否执行该分支。
    if (waitingFinger) return;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    waitingFinger = true;

    // 只在开始时提示一次
    // 发出 Qt 信号 identifyResult 通知其他对象。
    emit identifyResult(false, -1, 0, "请按下指纹进行识别"); // UI可用此信号做提示音

    // 创建或使用 Qt 定时器对象。
    QTimer* timer = new QTimer(this);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(timer, &QTimer::timeout, this, [=]() mutable {
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
                waitingFinger = false;
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
            waitingFinger = false;
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
    // 声明静态变量或静态常量，保持跨调用状态。
    static bool enrolling = false;
    // 根据条件判断是否执行该分支。
    if (enrolling) return;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    enrolling = true;

    // 继续当前代码块的参数、表达式或结构定义。
    int* currentStep = new int(1); // 用堆变量保证lambda可修改
    // 发出 Qt 信号 enrollResult 通知其他对象。
    emit enrollResult(false, id, "请按下手指进行第1次采集");

    // 创建或使用 Qt 定时器对象。
    QTimer* timer = new QTimer(this);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(timer, &QTimer::timeout, this, [=]() mutable {
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
                enrolling = false;
                // 执行本行语句完成对应变量、对象或业务状态处理。
                timer->stop();
                // 执行本行语句完成对应变量、对象或业务状态处理。
                timer->deleteLater();
                // 释放动态创建的对象资源。
                delete currentStep;
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
                    enrolling = false;
                    // 执行本行语句完成对应变量、对象或业务状态处理。
                    timer->stop();
                    // 执行本行语句完成对应变量、对象或业务状态处理。
                    timer->deleteLater();
                    // 释放动态创建的对象资源。
                    delete currentStep;
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
                enrolling = false;
                // 执行本行语句完成对应变量、对象或业务状态处理。
                timer->stop();
                // 执行本行语句完成对应变量、对象或业务状态处理。
                timer->deleteLater();
                // 释放动态创建的对象资源。
                delete currentStep;
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
