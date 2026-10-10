#pragma once

#include <QByteArray>
#include <QString>

inline QString smartproServerIp()
{
    const QByteArray value = qgetenv("SMARTPRO_SERVER_IP");
    return value.isEmpty() ? QStringLiteral("192.168.1.4") : QString::fromLocal8Bit(value);
}

inline QString smartproSelfIp()
{
    const QByteArray value = qgetenv("SMARTPRO_SELF_IP");
    return value.isEmpty() ? QStringLiteral("192.168.1.8") : QString::fromLocal8Bit(value);
}
