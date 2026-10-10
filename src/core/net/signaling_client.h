/**
 * @file signaling_client.h
 * @brief 门口机侧 TCP 信令客户端（连接室内机 8887 端口）。
 *
 * 从原 Widget 中抽离，职责单一：
 *   - 连接/断线自动重连（保留原有的 3 秒重连语义）
 *   - 按 '\n' 切分信令并转成 protocol::Signal
 *   - 发送信令
 *
 * 不涉及任何音视频与 UI 逻辑，便于单独测试。
 */
#ifndef ACCESSTERMINAL_CORE_NET_SIGNALING_CLIENT_H
#define ACCESSTERMINAL_CORE_NET_SIGNALING_CLIENT_H

#include <QObject>
#include <QString>

#include "core/net/protocol.h"

class QTcpSocket;
class QTimer;

namespace accessterminal {

/**
 * @brief 门口机信令客户端。
 *
 * 用法：构造后调用 start()，通过信号接收信令。
 */
class SignalingClient : public QObject
{
    Q_OBJECT

public:
    explicit SignalingClient(QObject* parent = nullptr);
    ~SignalingClient() override;

    /**
     * @brief 设置目标地址并开始连接。
     * @param serverIp 室内机 IP。
     * @param port 信令端口，默认 protocol::kSignalingPort。
     */
    void start(const QString& serverIp, quint16 port = protocol::kSignalingPort);

    /// 断开连接并停止自动重连。
    void stop();

    /// 当前是否已连接。
    bool isConnected() const;

    /**
     * @brief 发送一条信令。
     * @param signal 信令类型。
     * @return 已连接且写入成功返回 true。
     */
    bool send(protocol::Signal signal);

signals:
    /// 收到一条可识别的信令。
    void signalReceived(accessterminal::protocol::Signal signal);
    /// 连接状态变化，便于 UI 显示。
    void connectedChanged(bool connected);
    /// 发生错误（仅用于日志，不应打断流程）。
    void errorOccurred(const QString& message);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError();

private:
    void scheduleReconnect();

    QTcpSocket* m_socket = nullptr;
    QTimer* m_reconnectTimer = nullptr;
    QString m_serverIp;
    quint16 m_port = protocol::kSignalingPort;
    bool m_running = false;
    bool m_wasConnected = false;

    /// TCP 字节流解码器：跨 readyRead 保存半包，避免拆包丢信令。
    protocol::StreamDecoder m_decoder;
};

} // namespace accessterminal

#endif // ACCESSTERMINAL_CORE_NET_SIGNALING_CLIENT_H
