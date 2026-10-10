// 开始头文件防重复包含保护 CAMERA_PUSH_THREAD_H。
#ifndef CAMERA_PUSH_THREAD_H
// 定义预处理宏或常量 CAMERA_PUSH_THREAD_H。
#define CAMERA_PUSH_THREAD_H

// 引入系统或第三方头文件 <QThread>。
#include <QThread>
// 引入系统或第三方头文件 <QString>。
#include <QString>
// 引入系统或第三方头文件 <atomic>。
#include <atomic>
// 引入项目内部头文件 "camera/v4l2camera.h"。
#include "core/vision/v4l2_camera.h"
// 声明类 CameraPushThread。
class CameraPushThread : public QThread
// 进入新的代码块作用域。
{
    // 继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
// 声明类成员的 public 访问权限区域。
public:
    // 执行本行语句完成对应变量、对象或业务状态处理。
    CameraPushThread(V4L2Camera* camera, const QString& ip, quint16 port, QObject* parent = nullptr);
    // 定义或声明函数 stop。
    void stop();
    // 定义或声明函数 isRunning。
    bool isRunning() const { return running; }
// 声明类成员的 protected 访问权限区域。
protected:
    // 定义或声明函数 run。
    void run() override;
// 声明类成员的 private 访问权限区域。
private:
    // 执行本行语句完成对应变量、对象或业务状态处理。
    V4L2Camera* camera;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QString serverIp;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint16 serverPort;
    // 停止标志。初值为 true，表示“构造完成即可运行”；run() 内**不得**
    // 再置 true，否则会覆盖掉在 start() 与 run() 之间到达的 stop() 请求。
    // 详见 video_push_thread.cpp 中构造函数的说明，以及 tools/verify.py
    // 的 check_no_running_reset_in_run。
    std::atomic<bool> running{true};
// 结束当前代码块作用域。
};

// 结束当前预处理条件块。
#endif // CAMERA_PUSH_THREAD_H 
