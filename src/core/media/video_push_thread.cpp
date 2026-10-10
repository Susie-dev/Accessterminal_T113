// 引入项目内部头文件 "camera_push_thread.h"。
#include "core/media/video_push_thread.h"
// 引入系统或第三方头文件 <QDebug>。
#include <QDebug>
// 引入系统或第三方头文件 <arpa/inet.h>。
#include <arpa/inet.h>
// 引入系统或第三方头文件 <unistd.h>。
#include <unistd.h>
// 引入系统或第三方头文件 <pthread.h>。
#include <pthread.h>
// 引入系统或第三方头文件 <cstring>。
#include <cstring>
// 引入系统或第三方头文件 <QBuffer>。
#include <QBuffer>
// 引入系统或第三方头文件 <QImage>。
#include <QImage>

// 定义类 CameraPushThread 的成员函数 CameraPushThread。
//
// running 初值为 true（构造即“待运行”），且 **run() 内不再重置它**。
// 这一点与同工程的 AudioSenderThread / AudioReceiverThread 保持一致，
// 用来消除一个真实的“丢停止”竞态：
//
//   if (m_cameraPushThread) { stop(); wait(); delete; }   // 先清理旧的
//   m_cameraPushThread = new CameraPushThread(...);
//   m_cameraPushThread->start();                          // 再启动新的
//
// 线程对象创建与 start() 之间、以及 start() 到 run() 真正开始执行之间，
// 存在时间窗口。若此期间有人调用 stop()（例如用户在这几十微秒内挂断），
// running 会被置为 false；而旧代码在 run() 开头无条件写 running = true，
// **会把这次停止请求覆盖掉**，线程随即进入 while(running) 长时间推流。
// 后果：挂断流程里的 wait(3000) 超时、delete 掉一个仍在运行的 QThread
// （Qt 会打印 QThread: Destroyed while thread is still running 并终止进程），
// 同时该线程还持有已关闭的 socket 继续 sendto。
//
// 改为初值 true、run() 不重置后，stop() 的语义变为“不可逆地置 false”，
// 任何时刻调用都不会被后续的 run() 抹掉——这正是 stop() 应有的语义。
CameraPushThread::CameraPushThread(V4L2Camera* cam, const QString& ip, quint16 port, QObject* parent)
    // 继续当前代码块的参数、表达式或结构定义。
    : QThread(parent), camera(cam), serverIp(ip), serverPort(port), running(true)
// 进入新的代码块作用域。
{}

// 定义或声明函数 CameraPushThread::stop。
void CameraPushThread::stop() {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    running = false;
// 结束当前代码块作用域。
}

// 定义或声明函数 CameraPushThread::run。
void CameraPushThread::run() {
    // 输出调试日志。
    qDebug() << "[视频推送] 线程启动 -> 目标服务器:" << serverIp << ":" << serverPort;
    // 注意：这里**不能**写 running = true。
    // running 的初值已在构造函数中置为 true，run() 内再置 true 会覆盖掉
    // 在 start() 与 run() 之间到达的 stop() 请求（详见构造函数上方的说明）。
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    // 根据条件判断是否执行该分支。
    if (sock < 0) {
        // 输出警告日志。
        qWarning() << "[视频推送] 创建UDP socket失败";
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 声明或创建 C 结构体变量。
    struct sockaddr_in serv_addr;
    // 清空结构体或缓冲区内容。
    memset(&serv_addr, 0, sizeof(serv_addr));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    serv_addr.sin_family = AF_INET;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    serv_addr.sin_port = htons(serverPort);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    inet_pton(AF_INET, serverIp.toStdString().c_str(), &serv_addr.sin_addr);
    // 输出调试日志。
    qDebug() << "[视频推送] UDP连接已建立，开始推送视频流...";

    // 执行本行语句完成对应变量、对象或业务状态处理。
    int frame_count = 0;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int success_count = 0;
    // 开始循环处理满足条件的数据或状态。
    while (running) {
        // 创建或处理图像帧数据。
        QImage frame = camera ? camera->getFrame() : QImage();
        // 根据条件判断是否执行该分支。
        if (!frame.isNull()) {
            // 创建或使用字节数组保存协议或音视频数据。
            QByteArray jpegData;
            // 执行本行语句完成对应变量、对象或业务状态处理。
            QBuffer buffer(&jpegData);
            // 执行本行语句完成对应变量、对象或业务状态处理。
            buffer.open(QIODevice::WriteOnly);
            // 通话视频走单向 UDP 推流，质量 75 在清晰度和带宽/编码耗时之间做折中。
            // 继续当前代码块的参数、表达式或结构定义。
            frame.save(&buffer, "JPEG", 75); // 75为压缩质量
            // 执行本行语句完成对应变量、对象或业务状态处理。
            // sockaddr_in* -> sockaddr* 是 POSIX socket API 规定的固定形态，
            // 用 reinterpret_cast 表达（与 widget.cpp 中绑定端口的写法保持一致）。
            int ret = sendto(sock, jpegData.data(), jpegData.size(), 0,
                             reinterpret_cast<struct sockaddr*>(&serv_addr),
                             sizeof(serv_addr));
            // 根据条件判断是否执行该分支。
            if (ret < 0) {
                // 输出警告日志。
                qWarning() << "[视频推送] 发送失败:" << strerror(errno);
            // 结束当前代码块作用域。
            } else {
                // 执行本行语句完成对应变量、对象或业务状态处理。
                success_count++;
                // 执行本行语句完成对应变量、对象或业务状态处理。
                frame_count++;
                // 根据条件判断是否执行该分支。
                if (frame_count % 100 == 0) {
                    // 输出调试日志。
                    qDebug() << "[视频推送] 已成功推送" << success_count << "帧，当前帧大小:" << jpegData.size() << "字节";
                // 结束当前代码块作用域。
                }
            // 结束当前代码块作用域。
            }
        // 结束当前代码块作用域。
        } else {
            // 声明静态变量或静态常量，保持跨调用状态。
            static int no_data_count = 0;
            // 根据条件判断是否执行该分支。
            if (no_data_count < 5) {
                // 输出调试日志。
                qDebug() << "[视频推送] 等待视频数据...";
                // 执行本行语句完成对应变量、对象或业务状态处理。
                no_data_count++;
            // 结束当前代码块作用域。
            }
        // 结束当前代码块作用域。
        }
        // sleep分片，及时响应stop
        // 开始循环处理满足条件的数据或状态。
        for (int i = 0; i < 8 && running; ++i) {
            // 继续当前代码块的参数、表达式或结构定义。
            usleep(5000); // 8*5ms=40ms
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    }
    // 输出调试日志。
    qDebug() << "[视频推送] 线程结束，共推送" << success_count << "帧";
    // 关闭文件描述符或 socket。
    close(sock);
// 结束当前代码块作用域。
} 
