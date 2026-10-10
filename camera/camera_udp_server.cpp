// 逐行注释：引入项目内部头文件 "camera_udp_server.h"。
#include "camera_udp_server.h"
// 逐行注释：引入项目内部头文件 "widget.h"。
#include "widget.h"
#include "appconfig.h"
// 逐行注释：引入系统或第三方头文件 <QBuffer>。
#include <QBuffer>
// 逐行注释：引入系统或第三方头文件 <QImage>。
#include <QImage>
// 逐行注释：引入系统或第三方头文件 <QHostAddress>。
#include <QHostAddress>

// 逐行注释：定义类 CameraUdpServer 的成员函数 CameraUdpServer。
CameraUdpServer::CameraUdpServer(V4L2Camera* camera, quint16 port, QObject* parent)
    // 逐行注释：继续当前代码块的参数、表达式或结构定义。
    : QObject(parent), camera(camera)
// 逐行注释：进入新的代码块作用域。
{
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    udpSocket = new QUdpSocket(this);
    // 逐行注释：输出调试日志。
    qDebug() << "[CameraUdpServer调试] 开始初始化UDP socket，端口:" << port;
    
    // 逐行注释：根据条件判断是否执行该分支。
    const QHostAddress bindAddress = QHostAddress::AnyIPv4;
    if (udpSocket->bind(bindAddress, port)) {
        // 逐行注释：输出调试日志。
        qDebug() << "[CameraUdpServer调试] ✓ UDP socket绑定成功:"
                 << bindAddress.toString() << port;
        // 逐行注释：输出调试日志。
        qDebug() << "[CameraUdpServer调试] UDP socket状态:" << udpSocket->state();

    // 逐行注释：结束当前代码块作用域。
    } else {
        // 逐行注释：输出调试日志。
        qDebug() << "[CameraUdpServer调试] ✗ UDP socket绑定失败:" << udpSocket->errorString();
        // 逐行注释：输出调试日志。
        qDebug() << "[CameraUdpServer调试] UDP socket错误代码:" << udpSocket->error();
    // 逐行注释：结束当前代码块作用域。
    }
    
    // 逐行注释：建立 Qt 信号与槽或 lambda 回调连接。
    connect(udpSocket, &QUdpSocket::readyRead, this, &CameraUdpServer::onReadyRead);
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    sendTimer = new QTimer(this);
    // 逐行注释：继续当前代码块的参数、表达式或结构定义。
    sendTimer->setInterval(50); // 20fps
    // 逐行注释：建立 Qt 信号与槽或 lambda 回调连接。
    connect(sendTimer, &QTimer::timeout, this, &CameraUdpServer::sendFrame);
    
    // 逐行注释：输出调试日志。
    qDebug() << "[CameraUdpServer调试] CameraUdpServer初始化完成";
// 逐行注释：结束当前代码块作用域。
}

// 逐行注释：定义或声明函数 CameraUdpServer::onReadyRead。
void CameraUdpServer::onReadyRead()
// 逐行注释：进入新的代码块作用域。
{
    // 逐行注释：输出调试日志。
    qDebug() << "[CameraUdpServer调试] ========== onReadyRead 被调用 ==========";
    // 逐行注释：输出调试日志。
    qDebug() << "[CameraUdpServer调试] 待处理数据包数量:" << udpSocket->hasPendingDatagrams();

    // 逐行注释：开始循环处理满足条件的数据或状态。
    while (udpSocket->hasPendingDatagrams()) {
        // 逐行注释：创建或设置网络主机地址。
        QHostAddress sender;
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        quint16 senderPort;
        // 逐行注释：创建或使用字节数组保存协议或音视频数据。
        QByteArray datagram;
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        datagram.resize(udpSocket->pendingDatagramSize());
        

        
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        qint64 bytesRead = udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
        



        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        QString msg = QString::fromUtf8(datagram).trimmed();


        // 服务端不长期占用视频带宽，只有收到 getframe 后才记录请求端并开始定时推帧。
        // 逐行注释：根据条件判断是否执行该分支。
        if (msg == "getframe") {


            // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
            lastClientAddr = sender;
            // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
            lastClientPort = senderPort;
            // 逐行注释：根据条件判断是否执行该分支。
            if (!sending) {
                // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
                sending = true;
                // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
                sendTimer->start();

            // 逐行注释：结束当前代码块作用域。
            } else {

            // 逐行注释：结束当前代码块作用域。
            }
        // 逐行注释：结束当前代码块作用域。
        } else if (msg == "stop") {
            // 逐行注释：输出调试日志。
            qDebug() << "[CameraUdpServer调试] ✓ 收到stop请求，停止发送视频帧";
            // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
            sending = false;
            // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
            sendTimer->stop();

        // 逐行注释：结束当前代码块作用域。
        } else {

        // 逐行注释：结束当前代码块作用域。
        }
    // 逐行注释：结束当前代码块作用域。
    }

// 逐行注释：结束当前代码块作用域。
}

// 逐行注释：定义或声明函数 CameraUdpServer::sendFrame。
void CameraUdpServer::sendFrame()
// 逐行注释：进入新的代码块作用域。
{
    // 逐行注释：根据条件判断是否执行该分支。
    if (!sending || lastClientAddr.isNull() || lastClientPort == 0) {
        // 逐行注释：输出调试日志。
        qDebug() << "[CameraUdpServer调试] sendFrame被调用但条件不满足，跳过发送";
        // 逐行注释：输出调试日志。
        qDebug() << "[CameraUdpServer调试] sending:" << sending << ", lastClientAddr:" << lastClientAddr.toString() << ", lastClientPort:" << lastClientPort;
        // 逐行注释：返回当前函数的执行结果或结束函数。
        return;
    // 逐行注释：结束当前代码块作用域。
    }
    
    // 逐行注释：创建或处理图像帧数据。
    QImage frame = camera->getFrame();
    // 逐行注释：根据条件判断是否执行该分支。
    if (!frame.isNull()) {
        // 逐行注释：创建或使用字节数组保存协议或音视频数据。
        QByteArray jpegData;
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        QBuffer buffer(&jpegData);
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        buffer.open(QIODevice::WriteOnly);
        // 使用单帧 JPEG 通过 UDP 发送，接收端按包解码；实现简单，但没有重传和分片重组能力。
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        frame.save(&buffer, "JPEG");
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        qint64 sent = udpSocket->writeDatagram(jpegData, lastClientAddr, lastClientPort);
        
        // 逐行注释：根据条件判断是否执行该分支。
        if (sent != jpegData.size()) {
           // qDebug() << "[CameraUdpServer调试] ✗ 发送视频帧失败，预期发送" << jpegData.size() << "字节，实际发送" << sent << "字节";
           // qDebug() << "[CameraUdpServer调试] UDP socket错误:" << udpSocket->errorString();
        // 逐行注释：结束当前代码块作用域。
        } else {
           // qDebug() << "[CameraUdpServer调试] ✓ 成功发送视频帧，大小:" << jpegData.size() << "字节，目标:" << lastClientAddr.toString() << ":" << lastClientPort;
        // 逐行注释：结束当前代码块作用域。
        }
    // 逐行注释：结束当前代码块作用域。
    } else {
      //  qDebug() << "[CameraUdpServer调试] ✗ 获取视频帧失败";
    // 逐行注释：结束当前代码块作用域。
    }
// 逐行注释：结束当前代码块作用域。
}
