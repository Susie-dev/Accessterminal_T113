// 引入项目内部头文件 "audio_manager.h"。
#include "audio_manager.h"
// 引入系统或第三方头文件 <QFile>。
#include <QFile>
// 引入系统或第三方头文件 <QDebug>。
#include <QDebug>
// 引入系统或第三方头文件 <QtConcurrent/QtConcurrent>。
#include <QtConcurrent/QtConcurrent>
// 引入系统或第三方头文件 <alsa/asoundlib.h>。
#include <alsa/asoundlib.h>
// 引入系统或第三方头文件 <QMutex>。
#include <QMutex>

// 定义类 AudioManager 的成员函数 AudioManager。
AudioManager::AudioManager(QObject* parent)
    // 继续当前代码块的参数、表达式或结构定义。
    : QObject(parent)
// 进入新的代码块作用域。
{
// 结束当前代码块作用域。
}

// 定义类 AudioManager 的成员函数 ~AudioManager。
AudioManager::~AudioManager() {}

// 定义或声明函数 AudioManager::playAsync。
void AudioManager::playAsync(const QString& pcmFilePath)
// 进入新的代码块作用域。
{
    // 继续当前代码块的参数、表达式或结构定义。
    stop(); // 打断上一次
    // 执行本行语句完成对应变量、对象或业务状态处理。
    interruptFlag = std::make_shared<std::atomic<bool>>(false);
    // 使用自动类型推导保存表达式结果。
    auto flag = interruptFlag;
    // 继续当前代码块的参数、表达式或结构定义。
    playFuture = QtConcurrent::run([=]() {
        // 发出 Qt 信号 playbackStarted 通知其他对象。
        emit playbackStarted(pcmFilePath);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        playPcmFile(pcmFilePath, flag.get());
        // 发出 Qt 信号 playbackFinished 通知其他对象。
        emit playbackFinished(pcmFilePath);
    // 结束当前代码块作用域。
    });
// 结束当前代码块作用域。
}

// 定义或声明函数 AudioManager::stop。
void AudioManager::stop()
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if (interruptFlag) *interruptFlag = true;
// 结束当前代码块作用域。
}

// 定义或声明函数 AudioManager::playPcmFile。
void AudioManager::playPcmFile(const QString& pcmFilePath, std::atomic<bool>* interruptFlag)
// 进入新的代码块作用域。
{
    // 声明静态变量或静态常量，保持跨调用状态。
    static QMutex pcmMutex;
    // ALSA 设备通常不支持多个播放流同时写入，用静态互斥锁串行化提示音播放。
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QMutexLocker locker(&pcmMutex);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_t *pcm_handle;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int rc = snd_pcm_open(&pcm_handle, "hw:audiocodec", SND_PCM_STREAM_PLAYBACK, 0);
    // 根据条件判断是否执行该分支。
    if (rc < 0) {
        // 输出警告日志。
        qWarning() << "[AudioManager] 无法打开PCM设备:" << snd_strerror(rc) << rc;
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_t *params;
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_alloca(&params);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_any(pcm_handle, params);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_access(pcm_handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_format(pcm_handle, params, SND_PCM_FORMAT_S16_LE);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_channels(pcm_handle, params, 1);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    unsigned int rate = 16000;
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params_set_rate_near(pcm_handle, params, &rate, 0);
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_hw_params(pcm_handle, params);

    // 执行本行语句完成对应变量、对象或业务状态处理。
    QFile file(pcmFilePath);
    // 根据条件判断是否执行该分支。
    if (!file.open(QIODevice::ReadOnly)) {
        // 输出警告日志。
        qWarning() << "[AudioManager] 无法打开PCM文件:" << pcmFilePath;
        // 调用 ALSA 音频接口配置或读写 PCM 设备。
        snd_pcm_close(pcm_handle);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray data;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    bool interrupted = false;
    // 开始循环处理满足条件的数据或状态。
    while (!(data = file.read(320)).isEmpty()) {
        // 根据条件判断是否执行该分支。
        if (interruptFlag && *interruptFlag) {
            // 被新提示音打断时 drop 当前缓冲，避免上一段音频尾巴继续播出。
            // 调用 ALSA 音频接口配置或读写 PCM 设备。
            snd_pcm_drop(pcm_handle);
            // 执行本行语句完成对应变量、对象或业务状态处理。
            interrupted = true;
            // 跳出当前循环或 switch 分支。
            break;
        // 结束当前代码块作用域。
        }
        // 执行本行语句完成对应变量、对象或业务状态处理。
        int frames = data.size() / 2;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        int rcw = snd_pcm_writei(pcm_handle, data.constData(), frames);
        // 根据条件判断是否执行该分支。
        if (rcw < 0) {
            // 输出警告日志。
            qWarning() << "[AudioManager] snd_pcm_writei错误:" << snd_strerror(rcw);
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    file.close();
    // 根据条件判断是否执行该分支。
    if (!interrupted) {
        // 调用 ALSA 音频接口配置或读写 PCM 设备。
        snd_pcm_drain(pcm_handle);
    // 结束当前代码块作用域。
    }
    // 调用 ALSA 音频接口配置或读写 PCM 设备。
    snd_pcm_close(pcm_handle);
// 结束当前代码块作用域。
}
