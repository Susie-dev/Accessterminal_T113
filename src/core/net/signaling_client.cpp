/**
 * @file signaling_client.cpp
 * @brief SignalingClient 实现。
 */
#include "core/net/signaling_client.h"

#include <QDebug>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>

namespace accessterminal {

namespace {
/// 断线重连间隔，沿用重构前的 3 秒。
constexpr int kReconnectDelayMs = 3000;
} // namespace

SignalingClient::SignalingClient(QObject* parent)
    : QObject(parent)
{
    m_socket = new QTcpSocket(this);
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);

    connect(m_socket, &QTcpSocket::connected, this, &SignalingClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &SignalingClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &SignalingClient::onReadyRead);
    // 用 QOverload 挑选 error(QAbstractSocket::SocketError) 重载：
    // errorOccurred 直到 Qt 5.15 才引入，而 T113 的 Tina SDK 通常仍是
    // Qt 5.9~5.12，直接用它会在旧 SDK 上编译失败。
    //
    // 在 Qt 5.15+ 上该重载会被标记为弃用，因此这里就地屏蔽这个警告。
    // 不用 errorOccurred 是**刻意的决定**（旧 SDK 上不存在），
    // 保留警告只会让每次编译都出现无法消除的噪声。
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    connect(m_socket,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, &SignalingClient::onSocketError);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_running) {
            qDebug() << "[信令] 正在重连" << m_serverIp << m_port;
            m_socket->connectToHost(m_serverIp, m_port);
        }
    });
}

SignalingClient::~SignalingClient()
{
    // 避免析构期间回调重连。
    m_running = false;
}

void SignalingClient::start(const QString& serverIp, quint16 port)
{
    m_serverIp = serverIp;
    m_port = port;
    m_running = true;
    qDebug() << "[信令] 连接室内机" << m_serverIp << m_port;
    m_socket->connectToHost(m_serverIp, m_port);
}

void SignalingClient::stop()
{
    m_running = false;
    m_reconnectTimer->stop();
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
    }
}

bool SignalingClient::isConnected() const
{
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

bool SignalingClient::send(protocol::Signal signal)
{
    const QByteArray payload = protocol::frame(signal);
    if (payload.isEmpty()) {
        qWarning() << "[信令] 尝试发送未知信令，已忽略";
        return false;
    }
    if (!isConnected()) {
        qWarning() << "[信令] 未连接，发送失败:" << payload.trimmed();
        return false;
    }
    const qint64 written = m_socket->write(payload);
    m_socket->flush();
    return written == payload.size();
}

void SignalingClient::onConnected()
{
    qDebug() << "[信令] 已连接:" << m_socket->peerAddress().toString() << m_socket->peerPort();
    if (!m_wasConnected) {
        m_wasConnected = true;
        emit connectedChanged(true);
    }
}

void SignalingClient::onDisconnected()
{
    qDebug() << "[信令] 连接断开";
    if (m_wasConnected) {
        m_wasConnected = false;
        emit connectedChanged(false);
    }
    scheduleReconnect();
}

void SignalingClient::onSocketError()
{
    // 不弹窗、不中断：重连由 disconnected/scheduleReconnect 处理。
    const QString msg = m_socket->errorString();
    qWarning() << "[信令] socket 错误:" << msg << "目标:" << m_serverIp << m_port;
    emit errorOccurred(msg);
    // 连接被拒绝时不会触发 disconnected，此处主动安排重连。
    if (m_socket->state() == QAbstractSocket::UnconnectedState) {
        scheduleReconnect();
    }
}

void SignalingClient::scheduleReconnect()
{
    if (!m_running) {
        return;
    }
    if (!m_reconnectTimer->isActive()) {
        m_reconnectTimer->start(kReconnectDelayMs);
    }
}

void SignalingClient::onReadyRead()
{
    const QByteArray data = m_socket->readAll();
    // TCP 是字节流，一条 readAll 可能包含多条以 '\n' 分隔的信令，
    // 也可能只有半条。按终止符切分并逐条解析，避免粘包/半包漏判。
    const QList<QByteArray> lines = data.split(protocol::kSignalTerminator);
    for (const QByteArray& line : lines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const protocol::Signal sig = protocol::fromWire(trimmed);
        if (sig == protocol::Signal::Unknown) {
            qWarning() << "[信令] 收到未知信令，已忽略:" << trimmed;
            continue;
        }
        qDebug() << "[信令] 收到:" << trimmed;
        emit signalReceived(sig);
    }
}

} // namespace accessterminal
