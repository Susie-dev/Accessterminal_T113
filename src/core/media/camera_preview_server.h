// 逐行注释：声明该头文件在一次编译中只包含一次。
#pragma once
// 逐行注释：引入系统或第三方头文件 <QObject>。
#include <QObject>
// 逐行注释：引入系统或第三方头文件 <QUdpSocket>。
#include <QUdpSocket>
// 逐行注释：引入系统或第三方头文件 <QTimer>。
#include <QTimer>
// 逐行注释：引入项目内部头文件 "camera/v4l2camera.h"。
#include "core/vision/v4l2_camera.h"
// 逐行注释：声明类 CameraUdpServer。
class CameraUdpServer : public QObject
// 逐行注释：进入新的代码块作用域。
{
    // 逐行注释：继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
// 逐行注释：声明类成员的 public 访问权限区域。
public:
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    CameraUdpServer(V4L2Camera* camera, quint16 port, QObject* parent = nullptr);

// 逐行注释：声明 Qt 信号槽成员区域。
private slots:
    // 逐行注释：定义或声明函数 onReadyRead。
    void onReadyRead();
    // 逐行注释：定义或声明函数 sendFrame。
    void sendFrame();

// 逐行注释：声明类成员的 private 访问权限区域。
private:
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    QUdpSocket* udpSocket;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    V4L2Camera* camera;
    // 逐行注释：创建或使用 Qt 定时器对象。
    QTimer* sendTimer;
    // 逐行注释：创建或设置网络主机地址。
    QHostAddress lastClientAddr;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    quint16 lastClientPort = 0;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    bool sending = false;
// 逐行注释：结束当前代码块作用域。
};
