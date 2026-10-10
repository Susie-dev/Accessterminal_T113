// 引入项目内部头文件 "camera_push_thread.h"。
#include "camera_push_thread.h"
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
CameraPushThread::CameraPushThread(V4L2Camera* cam, const QString& ip, quint16 port, QObject* parent)
    // 继续当前代码块的参数、表达式或结构定义。
    : QThread(parent), camera(cam), serverIp(ip), serverPort(port), running(false)
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
    // 执行本行语句完成对应变量、对象或业务状态处理。
    running = true;
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
            int ret = sendto(sock, jpegData.data(), jpegData.size(), 0, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
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
