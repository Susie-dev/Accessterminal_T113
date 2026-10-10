// 逐行注释：开始头文件防重复包含保护 AUDIORECEIVERTHREAD_H。
#ifndef AUDIORECEIVERTHREAD_H
// 逐行注释：定义预处理宏或常量 AUDIORECEIVERTHREAD_H。
#define AUDIORECEIVERTHREAD_H

// 逐行注释：引入系统或第三方头文件 <QThread>。
#include <QThread>
// 逐行注释：引入系统或第三方头文件 <atomic>。
#include <atomic>
// 逐行注释：引入系统或第三方头文件 <alsa/asoundlib.h>。
#include <alsa/asoundlib.h>
// 逐行注释：引入系统或第三方头文件 <QString>。
#include <QString>

// 逐行注释：声明类 AudioReceiverThread。
class AudioReceiverThread : public QThread {
    // 逐行注释：继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
// 逐行注释：声明类成员的 public 访问权限区域。
public:
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    AudioReceiverThread(int sockfd, QObject *parent = nullptr);
    // 逐行注释：定义或声明函数 stop。
    void stop();
    // 逐行注释：定义或声明函数 setVolume。
    void setVolume(int percent); // 0~100
// 逐行注释：声明类成员的 protected 访问权限区域。
protected:
    // 逐行注释：定义或声明函数 run。
    void run() override;
// 逐行注释：声明 Qt 信号槽成员区域。
signals:
    // 逐行注释：定义或声明函数 errorMsg。
    void errorMsg(const QString &msg);
    // 逐行注释：定义或声明函数 finishedMsg。
    void finishedMsg();
    // 逐行注释：定义或声明函数 connectionLost。
    void connectionLost();
// 逐行注释：声明类成员的 private 访问权限区域。
private:
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    std::atomic<bool> running;
    // 逐行注释：继续当前代码块的参数、表达式或结构定义。
    int currentVolume = 30; // 默认30%
    // 逐行注释：继续当前代码块的参数、表达式或结构定义。
    int sockfd; // 新增：外部传入的socket fd
// 逐行注释：结束当前代码块作用域。
};

// 逐行注释：结束当前预处理条件块。
#endif // AUDIORECEIVERTHREAD_H 
