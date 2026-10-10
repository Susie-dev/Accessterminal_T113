// 引入项目内部头文件 "audiosenderthread.h"。
#include "core/media/audio_sender_thread.h"
// 引入系统或第三方头文件 <arpa/inet.h>。
#include <arpa/inet.h>
// 引入系统或第三方头文件 <cstring>。
#include <cstring>
// 引入系统或第三方头文件 <cstdlib>。
#include <cstdlib>
// 引入系统或第三方头文件 <QDebug>。
#include <QDebug>
// 引入系统或第三方头文件 <QThread>。
#include <QThread>
// 引入系统或第三方头文件 <errno.h>。
#include <errno.h> // Added for errno

#include "core/net/protocol.h"
#include "config/app_config.h"

// 音频参数统一来自 protocol.h（采样率/声道/每包帧数），
// 设备名来自 AppConfig，避免与接收端及对端不一致。
namespace {
using namespace accessterminal::protocol;
constexpr int kSampleFormat = SND_PCM_FORMAT_S16_LE;

/**
 * @brief 取得 ALSA 采集设备名字符串。
 * @return 指向进程级静态缓冲的指针，其生命周期覆盖整个运行期，
 *         可安全传给 snd_pcm_open 等 C 接口（不随临时对象析构而失效）。
 */
const char* pcmDeviceName()
{
    static const QByteArray name =
        accessterminal::AppConfig::instance().audioDevice().toUtf8();
    return name.constData();
}
} // namespace
#define PCM_DEVICE pcmDeviceName()
#define SAMPLE_FORMAT kSampleFormat
#define SAMPLE_RATE kAudioSampleRate
#define CHANNELS kAudioChannels
#define BUFFER_FRAMES kAudioFramesPerPacket

// 定义类 AudioSenderThread 的成员函数 AudioSenderThread。
AudioSenderThread::AudioSenderThread(int sockfd, const QString& serverIp, quint16 serverPort, QObject *parent)
    // 继续当前代码块的参数、表达式或结构定义。
    : QThread(parent), running(true), sockfd(sockfd), serverIp(serverIp), serverPort(serverPort) {}

// 定义或声明函数 AudioSenderThread::stop。
void AudioSenderThread::stop() { running = false; }

// 定义或声明函数 AudioSenderThread::run。
void AudioSenderThread::run() {
    // 输出调试日志。
    qDebug() << "[AudioSenderThread] 线程启动, sockfd=" << sockfd << ", 目标IP=" << serverIp << ", 端口=" << serverPort;
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_t *pcm_handle;
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_t *params;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int rc;
    // 只用this->sockfd，不再创建/绑定/关闭socket
    // 声明或创建 C 结构体变量。
    struct sockaddr_in serv_addr;
    // 清空结构体或缓冲区内容。
    memset(&serv_addr, 0, sizeof(serv_addr));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    serv_addr.sin_family = AF_INET;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    serv_addr.sin_port = htons(serverPort);
    // 根据条件判断是否执行该分支。
    if (inet_pton(AF_INET, serverIp.toStdString().c_str(), &serv_addr.sin_addr) <= 0) {
        // 输出警告日志。
        qWarning() << "[AudioSenderThread] inet_pton失败，serverIp=" << serverIp;
        // 发出 Qt 信号 errorMsg 通知其他对象。
        emit errorMsg("inet_pton失败");
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 输出调试日志。
    qDebug() << "[AudioSenderThread] 服务器地址设置成功:" << serverIp << serverPort;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    rc = snd_pcm_open(&pcm_handle, PCM_DEVICE, SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
    // 根据条件判断是否执行该分支。
    if (rc < 0) {
        // 输出警告日志。
        qWarning() << "[AudioSenderThread] 无法打开pcm设备:" << PCM_DEVICE << snd_strerror(rc);
        // 发出 Qt 信号 errorMsg 通知其他对象。
        emit errorMsg(QString("无法打开pcm设备: %1").arg(snd_strerror(rc)));
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 输出调试日志。
    qDebug() << "[AudioSenderThread] PCM设备打开成功:" << PCM_DEVICE;
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_alloca(&params);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_any(pcm_handle, params);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_format(pcm_handle, params, SAMPLE_FORMAT);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_channels(pcm_handle, params, CHANNELS);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_rate(pcm_handle, params, SAMPLE_RATE, 0);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params(pcm_handle, params);
    // 输出调试日志。
    qDebug() << "[AudioSenderThread] PCM参数设置完成，开始采集并发送...";
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int frames = BUFFER_FRAMES;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int16_t *buffer = (int16_t *)malloc(frames * CHANNELS * sizeof(int16_t));
    // 开始循环处理满足条件的数据或状态。
    while (running) {
        // 采集线程采用非阻塞 ALSA 读，没数据时短休眠，兼顾实时性和退出响应速度。
        // 执行本行语句完成对应变量、对象或业务状态处理。
        rc = snd_pcm_readi(pcm_handle, buffer, frames);
        // 根据条件判断是否执行该分支。
        if (rc == -EPIPE) {
            // 输出警告日志。
            qWarning() << "[AudioSenderThread] 采集underrun，准备pcm...";
            // 调用 ALSA 音频接口配置或读写 PCM 设备。
            snd_pcm_prepare(pcm_handle);
            // 跳过本轮循环剩余逻辑并进入下一轮。
            continue;
        // 结束当前代码块作用域。
        } else if (rc == -EAGAIN) {
            // 开始循环处理满足条件的数据或状态。
            for (int i = 0; i < 2 && running; ++i) QThread::msleep(2); // 2*2ms=4ms
            // 跳过本轮循环剩余逻辑并进入下一轮。
            continue;
        // 结束当前代码块作用域。
        } else if (rc < 0) {
            // 输出警告日志。
            qWarning() << "[AudioSenderThread] 采集read错误:" << snd_strerror(rc);
            // 发出 Qt 信号 errorMsg 通知其他对象。
            emit errorMsg(QString("read错误: %1").arg(snd_strerror(rc)));
            // 跳出当前循环或 switch 分支。
            break;
        // 结束当前代码块作用域。
        } else if (rc != frames) {
          //  qWarning() << "[AudioSenderThread] 短读, 读到" << rc << "帧";
        // 结束当前代码块作用域。
        }


        // 执行本行语句完成对应变量、对象或业务状态处理。
        int bytes = rc * CHANNELS * sizeof(int16_t);
        // 发送端和接收端共用外部创建的 UDP socket，挂断时由 Widget 统一关闭 fd。
        // 执行本行语句完成对应变量、对象或业务状态处理。
        int sent = sendto(sockfd, buffer, bytes, 0, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
        // 根据条件判断是否执行该分支。
        if (sent != bytes) {
            // 输出警告日志。
            qWarning() << "[AudioSenderThread] sendto失败, sent=" << sent << ", bytes=" << bytes << ", errno=" << errno;
            // 发出 Qt 信号 errorMsg 通知其他对象。
            emit errorMsg("sendto失败");
            // 跳出当前循环或 switch 分支。
            break;
        // 结束当前代码块作用域。
        } else {
           // qDebug() << "[AudioSenderThread] sendto成功, bytes=" << bytes;
        // 结束当前代码块作用域。
        }
        // sleep分片，及时响应stop
        // 开始循环处理满足条件的数据或状态。
        for (int i = 0; i < 2 && running; ++i) QThread::msleep(2); // 2*2ms=4ms
    // 结束当前代码块作用域。
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    free(buffer);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_drain(pcm_handle);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_close(pcm_handle);
    // 输出调试日志。
    qDebug() << "[AudioSenderThread] 采集线程结束，资源已释放。";
    // 发出 Qt 信号 finishedMsg 通知其他对象。
    emit finishedMsg();
// 结束当前代码块作用域。
}
