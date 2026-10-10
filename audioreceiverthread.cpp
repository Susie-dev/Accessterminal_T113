//引入项目内部头文件 "audioreceiverthread.h"。
#include "audioreceiverthread.h"
//引入系统或第三方头文件 <arpa/inet.h>。
#include <arpa/inet.h>
//引入系统或第三方头文件 <cstring>。
#include <cstring>
//引入系统或第三方头文件 <cstdlib>。
#include <cstdlib>
//引入系统或第三方头文件 <QDebug>。
#include <QDebug>
//引入系统或第三方头文件 <QThread>。
#include <QThread>
//引入系统或第三方头文件 <alsa/asoundlib.h>。
#include <alsa/asoundlib.h>
//引入系统或第三方头文件 <fcntl.h>。
#include <fcntl.h>

//定义预处理宏或常量 PCM_DEVICE。
#define PCM_DEVICE "hw:audiocodec"
//定义预处理宏或常量 SAMPLE_RATE。
#define SAMPLE_RATE 16000
//定义预处理宏或常量 CHANNELS。
#define CHANNELS 1
//定义预处理宏或常量 SAMPLE_FORMAT。
#define SAMPLE_FORMAT SND_PCM_FORMAT_S16_LE
//定义预处理宏或常量 BUFFER_FRAMES。
#define BUFFER_FRAMES 320 // 20ms一包
//定义预处理宏或常量 LOCAL_PORT。
#define LOCAL_PORT 12345 // 与发送端口一致

//定义类 AudioReceiverThread 的成员函数 AudioReceiverThread。
AudioReceiverThread::AudioReceiverThread(int sockfd, QObject *parent)
    //继续当前代码块的参数、表达式或结构定义。
    : QThread(parent), running(true), sockfd(sockfd) {
    //输出调试日志。
    qDebug() << "[AudioReceiverThread] 构造函数被调用, sockfd=" << sockfd;
//结束当前代码块作用域。
}

//定义或声明函数 AudioReceiverThread::stop。
void AudioReceiverThread::stop() { running = false; }

//定义或声明函数 AudioReceiverThread::setVolume。
void AudioReceiverThread::setVolume(int percent)
//进入新的代码块作用域。
{
    //根据条件判断是否执行该分支。
    if (percent < 0) percent = 0;
    //根据条件判断是否执行该分支。
    if (percent > 100) percent = 100;
    //执行本行语句完成对应变量、对象或业务状态处理。
    currentVolume = percent;
    // 通过 ALSA mixer 调整播放音量；不同声卡控件名可能不同，移植时要校验 selem_name。
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_t *handle = nullptr;
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_selem_id_t *sid = nullptr;
    //声明静态变量或静态常量，保持跨调用状态。
    static const char *card = "default"; // 或 "hw:0"、"hw:audiocodec"
    //声明静态变量或静态常量，保持跨调用状态。
    static const char *selem_name = "Playback"; // 可尝试 "Master"、"PCM"、"Speaker"、"Playback"
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_open(&handle, 0);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_attach(handle, card);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_selem_register(handle, nullptr, nullptr);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_load(handle);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_selem_id_malloc(&sid);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_selem_id_set_index(sid, 0);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_selem_id_set_name(sid, selem_name);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);
    //根据条件判断是否执行该分支。
    if (elem) {
        //执行本行语句完成对应变量、对象或业务状态处理。
        long minv, maxv;
        //调用 ALSA 音频接口配置或读写 PCM 设备。
        snd_mixer_selem_get_playback_volume_range(elem, &minv, &maxv);
        //执行本行语句完成对应变量、对象或业务状态处理。
        long setv = minv + (maxv - minv) * percent / 100;
        //调用 ALSA 音频接口配置或读写 PCM 设备。
        snd_mixer_selem_set_playback_volume_all(elem, setv);
    //结束当前代码块作用域。
    }
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_selem_id_free(sid);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_mixer_close(handle);
//结束当前代码块作用域。
}

//定义或声明函数 AudioReceiverThread::run。
void AudioReceiverThread::run() {
    //输出调试日志。
    qDebug() << "[AudioReceiverThread] 线程启动, sockfd=" << sockfd;
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_t *pcm_handle;
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_t *params;
    //执行本行语句完成对应变量、对象或业务状态处理。
    int rc;
    //输出调试日志。
    qDebug() << "[AudioReceiverThread] 使用外部传入的UDP socket fd=" << sockfd;
    //执行本行语句完成对应变量、对象或业务状态处理。
    rc = snd_pcm_open(&pcm_handle, PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    //根据条件判断是否执行该分支。
    if (rc < 0) {
        //输出警告日志。
        qWarning() << "[AudioReceiverThread] 无法打开pcm播放设备:" << PCM_DEVICE << snd_strerror(rc);
        //发出 Qt 信号 errorMsg 通知其他对象。
        emit errorMsg(QString("无法打开pcm播放设备: %1").arg(snd_strerror(rc)));
        //返回当前函数的执行结果或结束函数。
        return;
    //结束当前代码块作用域。
    }
    //输出调试日志。
    qDebug() << "[AudioReceiverThread] PCM播放设备打开成功:" << PCM_DEVICE;
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_alloca(&params);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_any(pcm_handle, params);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_format(pcm_handle, params, SAMPLE_FORMAT);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_channels(pcm_handle, params, CHANNELS);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_rate(pcm_handle, params, SAMPLE_RATE, 0);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params(pcm_handle, params);
    //输出调试日志。
    qDebug() << "[AudioReceiverThread] PCM参数设置完成，开始接收并播放...";
    //执行本行语句完成对应变量、对象或业务状态处理。
    int frames = BUFFER_FRAMES;
    //执行本行语句完成对应变量、对象或业务状态处理。
    int16_t *buffer = (int16_t *)malloc(frames * CHANNELS * sizeof(int16_t));
    //执行本行语句完成对应变量、对象或业务状态处理。
    setVolume(currentVolume);
    // 设置socket为非阻塞
    //执行本行语句完成对应变量、对象或业务状态处理。
    int flags = fcntl(sockfd, F_GETFL, 0);
    //执行本行语句完成对应变量、对象或业务状态处理。
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    // ====== 丢弃延迟启动前堆积的所有UDP包 ======
    //执行本行语句完成对应变量、对象或业务状态处理。
    int discardCount = 0;
    //开始循环处理满足条件的数据或状态。
    while (true) {
        //执行本行语句完成对应变量、对象或业务状态处理。
        fd_set readfds;
        //声明或创建 C 结构体变量。
        struct timeval tv;
        //执行本行语句完成对应变量、对象或业务状态处理。
        FD_ZERO(&readfds);
        //执行本行语句完成对应变量、对象或业务状态处理。
        FD_SET(sockfd, &readfds);
        //执行本行语句完成对应变量、对象或业务状态处理。
        tv.tv_sec = 0;
        //执行本行语句完成对应变量、对象或业务状态处理。
        tv.tv_usec = 0;
        //执行本行语句完成对应变量、对象或业务状态处理。
        int ret = select(sockfd + 1, &readfds, nullptr, nullptr, &tv);
        //根据条件判断是否执行该分支。
        if (ret > 0 && FD_ISSET(sockfd, &readfds)) {
            //执行本行语句完成对应变量、对象或业务状态处理。
            ssize_t bytes = recvfrom(sockfd, buffer, frames * CHANNELS * sizeof(int16_t), MSG_DONTWAIT, nullptr, nullptr);
            //根据条件判断是否执行该分支。
            if (bytes > 0) discardCount++;
        //结束当前代码块作用域。
        } else {
            //跳出当前循环或 switch 分支。
            break;
        //结束当前代码块作用域。
        }
    //结束当前代码块作用域。
    }
    //根据条件判断是否执行该分支。
    if (discardCount > 0) qDebug() << "[AudioReceiverThread] 启动时丢弃了" << discardCount << "个堆积音频包";
    // ====== 正常接收和播放 ======
    //开始循环处理满足条件的数据或状态。
    while (running) {
        // select 设置 20ms 超时，既贴近一包音频时长，也能周期性检查 running 标志。
        //执行本行语句完成对应变量、对象或业务状态处理。
        fd_set readfds;
        //声明或创建 C 结构体变量。
        struct timeval tv;
        //执行本行语句完成对应变量、对象或业务状态处理。
        FD_ZERO(&readfds);
        //执行本行语句完成对应变量、对象或业务状态处理。
        FD_SET(sockfd, &readfds);
        //执行本行语句完成对应变量、对象或业务状态处理。
        tv.tv_sec = 0;
        //继续当前代码块的参数、表达式或结构定义。
        tv.tv_usec = 20000; // 20ms
        //执行本行语句完成对应变量、对象或业务状态处理。
        int ret = select(sockfd + 1, &readfds, nullptr, nullptr, &tv);
        //根据条件判断是否执行该分支。
        if (ret > 0 && FD_ISSET(sockfd, &readfds)) {
            //执行本行语句完成对应变量、对象或业务状态处理。
            ssize_t bytes = recvfrom(sockfd, buffer, frames * CHANNELS * sizeof(int16_t), 0, nullptr, nullptr);
            //根据条件判断是否执行该分支。
            if (bytes <= 0) {
                //输出警告日志。
                qWarning() << "[AudioReceiverThread] recvfrom失败, errno=" << errno;
                //发出 Qt 信号 errorMsg 通知其他对象。
                emit errorMsg("recvfrom失败");
                //发出 Qt 信号 connectionLost 通知其他对象。
                emit connectionLost();
                //跳出当前循环或 switch 分支。
                break;
            //结束当前代码块作用域。
            } else {
              //  qDebug() << "[AudioReceiverThread] recvfrom成功, bytes=" << bytes;
            //结束当前代码块作用域。
            }
            //执行本行语句完成对应变量、对象或业务状态处理。
            int rcw = snd_pcm_writei(pcm_handle, buffer, bytes / (CHANNELS * sizeof(int16_t)));
            //根据条件判断是否执行该分支。
            if (rcw == -EPIPE) {
              //  qWarning() << "[AudioReceiverThread] 播放underrun，准备pcm...";
                //调用 ALSA 音频接口配置或读写 PCM 设备。
                snd_pcm_prepare(pcm_handle);
                //跳过本轮循环剩余逻辑并进入下一轮。
                continue;
            //结束当前代码块作用域。
            } else if (rcw < 0) {
              //  qWarning() << "[AudioReceiverThread] 播放write错误:" << snd_strerror(rcw);
                //发出 Qt 信号 errorMsg 通知其他对象。
                emit errorMsg(QString("write错误: %1").arg(snd_strerror(rcw)));
                //跳出当前循环或 switch 分支。
                break;
            //结束当前代码块作用域。
            } else if (rcw != bytes / (CHANNELS * sizeof(int16_t))) {
                //输出警告日志。
                qWarning() << "[AudioReceiverThread] 播放短写, 写了" << rcw << "帧";
            //结束当前代码块作用域。
            } else {
              //  qDebug() << "[AudioReceiverThread] 播放成功, 写了" << rcw << "帧";
            //结束当前代码块作用域。
            }
        //结束当前代码块作用域。
        }
        // 否则超时，继续循环，检查running
    //结束当前代码块作用域。
    }
    //执行本行语句完成对应变量、对象或业务状态处理。
    free(buffer);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_drain(pcm_handle);
    //调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_close(pcm_handle);
    //输出调试日志。
    qDebug() << "[AudioReceiverThread] 播放线程结束，资源已释放。";
    //发出 Qt 信号 finishedMsg 通知其他对象。
    emit finishedMsg();
//结束当前代码块作用域。
} 
