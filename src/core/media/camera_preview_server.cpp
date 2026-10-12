#include "core/media/camera_preview_server.h"
#include "core/net/protocol.h"
#include "config/app_config.h"
#include <QBuffer>
#include <QImage>
#include <QHostAddress>

CameraUdpServer::CameraUdpServer(V4L2Camera* camera, quint16 port, QObject* parent)
    : QObject(parent), camera(camera)
{
    udpSocket = new QUdpSocket(this);
    qDebug() << "[CameraUdpServer调试] 开始初始化UDP socket，端口:" << port;
    
    const QHostAddress bindAddress = QHostAddress::AnyIPv4;
    if (udpSocket->bind(bindAddress, port)) {
        qDebug() << "[CameraUdpServer调试] ✓ UDP socket绑定成功:"
                 << bindAddress.toString() << port;
        qDebug() << "[CameraUdpServer调试] UDP socket状态:" << udpSocket->state();

    } else {
        qDebug() << "[CameraUdpServer调试] ✗ UDP socket绑定失败:" << udpSocket->errorString();
        qDebug() << "[CameraUdpServer调试] UDP socket错误代码:" << udpSocket->error();
    }
    
    connect(udpSocket, &QUdpSocket::readyRead, this, &CameraUdpServer::onReadyRead);
    sendTimer = new QTimer(this);
    sendTimer->setInterval(50); // 20fps
    connect(sendTimer, &QTimer::timeout, this, &CameraUdpServer::sendFrame);
    
    qDebug() << "[CameraUdpServer调试] CameraUdpServer初始化完成";
}

void CameraUdpServer::onReadyRead()
{
    qDebug() << "[CameraUdpServer调试] ========== onReadyRead 被调用 ==========";
    qDebug() << "[CameraUdpServer调试] 待处理数据包数量:" << udpSocket->hasPendingDatagrams();

    while (udpSocket->hasPendingDatagrams()) {
        QHostAddress sender;
        quint16 senderPort;
        QByteArray datagram;
        datagram.resize(udpSocket->pendingDatagramSize());
        

        
        // 读取一个数据报。
        //
        // 注意不能忽略返回值：QByteArray::resize() 不会清零新分配的内存，
        // 若实际读到的字节数少于 pendingDatagramSize()（或返回 -1），
        // 后续 fromWire() 会解析到未初始化的尾部字节。
        // 正常情况下 bytesRead == datagram.size()，行为与重构前完全一致。
        const qint64 bytesRead = udpSocket->readDatagram(
            datagram.data(), datagram.size(), &sender, &senderPort);
        if (bytesRead < 0) {
            continue; // 读取失败，丢弃该数据报
        }
        if (bytesRead < datagram.size()) {
            datagram.resize(static_cast<int>(bytesRead));
        }

        const accessterminal::protocol::Signal cmd =
            accessterminal::protocol::fromWire(datagram);


        // 服务端不长期占用视频带宽，只有收到 getframe 后才记录请求端并开始定时推帧。
        if (cmd == accessterminal::protocol::Signal::GetFrame) {

            lastClientAddr = sender;
            lastClientPort = senderPort;
            if (!sending) {
                sending = true;
                sendTimer->start();
            }
        } else if (cmd == accessterminal::protocol::Signal::Stop) {
            qDebug() << "[CameraUdpServer调试] ✓ 收到stop请求，停止发送视频帧";
            sending = false;
            sendTimer->stop();
        }
    }

}

void CameraUdpServer::sendFrame()
{
    if (!sending || lastClientAddr.isNull() || lastClientPort == 0) {
        qDebug() << "[CameraUdpServer调试] sendFrame被调用但条件不满足，跳过发送";
        qDebug() << "[CameraUdpServer调试] sending:" << sending << ", lastClientAddr:" << lastClientAddr.toString() << ", lastClientPort:" << lastClientPort;
        return;
    }
    
    QImage frame = camera->getFrame();
    if (!frame.isNull()) {
        QByteArray jpegData;
        QBuffer buffer(&jpegData);
        buffer.open(QIODevice::WriteOnly);
        // 使用单帧 JPEG 通过 UDP 发送，接收端按包解码；实现简单，但没有重传和分片重组能力。
        frame.save(&buffer, "JPEG");
        qint64 sent = udpSocket->writeDatagram(jpegData, lastClientAddr, lastClientPort);
        
        if (sent != jpegData.size()) {
           // qDebug() << "[CameraUdpServer调试] ✗ 发送视频帧失败，预期发送" << jpegData.size() << "字节，实际发送" << sent << "字节";
           // qDebug() << "[CameraUdpServer调试] UDP socket错误:" << udpSocket->errorString();
        } else {
           // qDebug() << "[CameraUdpServer调试] ✓ 成功发送视频帧，大小:" << jpegData.size() << "字节，目标:" << lastClientAddr.toString() << ":" << lastClientPort;
        }
    } else {
      //  qDebug() << "[CameraUdpServer调试] ✗ 获取视频帧失败";
    }
}
