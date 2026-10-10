// 开始头文件防重复包含保护 OPENSSL_HTTP_H。
#ifndef OPENSSL_HTTP_H
// 定义预处理宏或常量 OPENSSL_HTTP_H。
#define OPENSSL_HTTP_H

// 引入系统或第三方头文件 <QByteArray>。
#include <QByteArray>
// 引入系统或第三方头文件 <QString>。
#include <QString>

// 定义或声明函数 https_post。
QByteArray https_post(const QString &host, const QString &path, const QByteArray &postData, const QString &headers);
// 定义或声明函数 https_get。
QByteArray https_get(const QString &host, const QString &path, const QString &headers);

// 结束当前预处理条件块。
#endif // OPENSSL_HTTP_H
