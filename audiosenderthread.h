// 开始头文件防重复包含保护 AUDIOSENDERTHREAD_H。
#ifndef AUDIOSENDERTHREAD_H
// 定义预处理宏或常量 AUDIOSENDERTHREAD_H。
#define AUDIOSENDERTHREAD_H

// 引入系统或第三方头文件 <QThread>。
#include <QThread>
// 引入系统或第三方头文件 <atomic>。
#include <atomic>
// 引入系统或第三方头文件 <alsa/asoundlib.h>。
#include <alsa/asoundlib.h>
// 引入系统或第三方头文件 <QString>。
#include <QString>

// 声明类 AudioSenderThread。
class AudioSenderThread : public QThread {
    // 继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
// 声明类成员的 public 访问权限区域。
public:
    // 执行本行语句完成对应变量、对象或业务状态处理。
    AudioSenderThread(int sockfd, const QString& serverIp, quint16 serverPort, QObject *parent = nullptr);
    // 定义或声明函数 stop。
    void stop();
// 声明类成员的 protected 访问权限区域。
protected:
    // 定义或声明函数 run。
    void run() override;
// 声明 Qt 信号槽成员区域。
signals:
    // 定义或声明函数 errorMsg。
    void errorMsg(const QString &msg);
    // 定义或声明函数 finishedMsg。
    void finishedMsg();
    // 定义或声明函数 connectionLost。
    void connectionLost();
// 声明类成员的 private 访问权限区域。
private:
    // 执行本行语句完成对应变量、对象或业务状态处理。
    std::atomic<bool> running;
    // 继续当前代码块的参数、表达式或结构定义。
    int sockfd; // 新增：外部传入的socket fd
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QString serverIp;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    quint16 serverPort;
// 结束当前代码块作用域。
};

// 结束当前预处理条件块。
#endif // AUDIOSENDERTHREAD_H 
