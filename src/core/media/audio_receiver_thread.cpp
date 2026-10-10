//引入项目内部头文件 "audioreceiverthread.h"。
#include "core/media/audio_receiver_thread.h"
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
// errno / EAGAIN / EINTR：收包与写 PCM 都要靠它区分“暂时不可用”和“真错误”。
// glibc 可能经其它头间接引入，但依赖间接引入不可靠，故显式包含。
#include <errno.h>
// select()/fd_set/FD_ZERO 由 POSIX 规定在 <sys/select.h> 中声明。
// glibc 通常经其它头间接引入，但依赖间接引入不可靠（在更严格的
// libc 或开启 _POSIX_C_SOURCE 时可能不可见），故显式包含。
#include <sys/select.h>

#include "core/net/protocol.h"
#include "config/app_config.h"

// 音频参数与端口统一来自 protocol.h，设备名来自 AppConfig。
namespace {
using namespace accessterminal::protocol;
// 必须保持 ALSA 的枚举类型：snd_pcm_hw_params_set_format 的形参是
// snd_pcm_format_t，若先存成 constexpr int 再传回，C++ 不允许 int 隐式
// 转回枚举，会编译失败。此处用 auto 保留原类型（重构前的 baseline 是
// 直接把 SND_PCM_FORMAT_S16_LE 传给该函数）。
constexpr auto kSampleFormat = SND_PCM_FORMAT_S16_LE;

/**
 * @brief 取得 ALSA 播放设备名字符串。
 * @return 指向进程级静态缓冲的指针，生命周期覆盖整个运行期。
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
#define LOCAL_PORT kAudioPort

namespace {
/// -EINTR 重试同一包的上限，避免信号风暴导致死循环。
const int kWriteMaxRetries = 3;
} // namespace

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
    snd_mixer_t *handle = nullptr;
    snd_mixer_selem_id_t *sid = nullptr;
    //声明静态变量或静态常量，保持跨调用状态。
    static const char *card = "default"; // 或 "hw:0"、"hw:audiocodec"
    //声明静态变量或静态常量，保持跨调用状态。
    static const char *selem_name = "Playback"; // 可尝试 "Master"、"PCM"、"Speaker"、"Playback"

    // 音量调节属于尽力而为：任何一步失败都只记录日志并安全返回，
    // 绝不能因为 mixer 不可用而影响通话主流程（原实现未检查返回值，
    // handle 为空时会向下继续调用导致崩溃）。
    int err = snd_mixer_open(&handle, 0);
    if (err < 0 || handle == nullptr) {
        qWarning() << "[AudioReceiverThread] 无法打开 mixer:" << snd_strerror(err);
        return;
    }
    if ((err = snd_mixer_attach(handle, card)) < 0
        || (err = snd_mixer_selem_register(handle, nullptr, nullptr)) < 0
        || (err = snd_mixer_load(handle)) < 0) {
        qWarning() << "[AudioReceiverThread] mixer 初始化失败:" << snd_strerror(err);
        snd_mixer_close(handle);
        return;
    }
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);
    snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);
    if (elem) {
        long minv = 0, maxv = 0;
        snd_mixer_selem_get_playback_volume_range(elem, &minv, &maxv);
        const long setv = minv + (maxv - minv) * percent / 100;
        snd_mixer_selem_set_playback_volume_all(elem, setv);
    } else {
        qWarning() << "[AudioReceiverThread] 未找到音量控件:" << selem_name;
    }
    snd_mixer_close(handle);
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
    // malloc 返回 void*，在 C++ 中可直接隐式转换为 int16_t*，无需显式转型。
    int16_t *buffer = static_cast<int16_t*>(malloc(frames * CHANNELS * sizeof(int16_t)));
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
            // 一次 select 可读不代表恰好一个完整包，且 socket 为 O_NONBLOCK：
            // 必须循环收包直到 EAGAIN，否则积压包会持续增长、端到端延迟不断累积。
            // 关键：EAGAIN 只是“暂时无数据”，绝不能当成连接断开。
            for (;;) {
                ssize_t bytes = recvfrom(sockfd, buffer,
                                         frames * CHANNELS * sizeof(int16_t),
                                         MSG_DONTWAIT, nullptr, nullptr);
                if (bytes < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break; // 本轮已收完，回到 select 等待
                    }
                    if (errno == EINTR) {
                        continue; // 被信号打断，重试
                    }
                    qWarning() << "[AudioReceiverThread] recvfrom失败, errno=" << errno;
                    emit errorMsg("recvfrom失败");
                    emit connectionLost();
                    // 同 write 错误：必须结束整个循环，否则 connectionLost
                    // 会被每个 select 周期重复发射，导致反复挂断。
                    running = false;
                    goto receive_done;
                }
                if (bytes == 0) {
                    continue; // 空数据报，忽略
                }
                // 期望写入的帧数。先算成 int：一是与 snd_pcm_writei 的返回值
                // （存于 int rcw，与重构前的 baseline 一致）同类型比较，避免
                // int 与 unsigned long long 比较触发 -Wsign-compare；二是每包
                // 最多 320 帧，不存在溢出风险。
                const int framesExpected =
                    static_cast<int>(bytes / (CHANNELS * sizeof(int16_t)));

                // 设备以 SND_PCM_NONBLOCK 打开（见上面的 snd_pcm_open），
                // 因此 -EAGAIN / -EINTR 是**可达的**，且都不是"设备坏了"。
                // 只对 -EINTR 做有界重试（被信号打断时重写同一包），
                // -EPIPE 与 baseline 一致：prepare 后丢弃本包继续收下一包。
                int rcw = 0;
                for (int attempt = 0; attempt < kWriteMaxRetries; ++attempt) {
                    rcw = snd_pcm_writei(pcm_handle, buffer, framesExpected);
                    if (rcw == -EPIPE) {
                        // XRUN：prepare 后本包丢弃，与 baseline 行为一致。
                        snd_pcm_prepare(pcm_handle);
                        break;
                    }
                    if (rcw != -EINTR) {
                        break;
                    }
                }
                if (rcw == -EPIPE) {
                    continue;
                }
                if (rcw == -EAGAIN || rcw == -EWOULDBLOCK) {
                    // 播放缓冲区满。实时音频的正确做法是**丢弃这一包**：
                    // 若按一般错误终止线程，表现就是通话中途突然单向无声；
                    // 而阻塞重试又会让端到端延迟越堆越大。
                    qWarning() << "[AudioReceiverThread] 播放缓冲满，丢弃一包音频";
                    continue;
                }
                if (rcw < 0) {
                    emit errorMsg(QString("write错误: %1").arg(snd_strerror(rcw)));
                    // 必须**结束整个采集循环**。这里若只跳出内层收包循环
                    // （重构早期版本的 `goto receive_done`），外层 while(running)
                    // 会立刻再次 select/recvfrom，把同一个持久性错误无限重复：
                    // 每一轮都重新 emit errorMsg，而收包错误还会反复 emit
                    // connectionLost → 反复触发 Widget::hangUpCall。
                    // baseline 是 `break` 出 while(running)，语义即"线程结束"。
                    running = false;
                    goto receive_done;
                }
                if (rcw != framesExpected) {
                    qWarning() << "[AudioReceiverThread] 播放短写, 写了" << rcw << "帧";
                }
            }
        receive_done:;
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
