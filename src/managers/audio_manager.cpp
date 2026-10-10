// 引入项目内部头文件 "audio_manager.h"。
#include "managers/audio_manager.h"
// 引入项目内部头文件 "config/app_config.h"。
#include "config/app_config.h"
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

namespace {
/// writei 遇到 XRUN 后重写同一包的最大次数（每次重写前用 snd_pcm_prepare 复位）。
constexpr int kWriteRetries = 3;
} // namespace

// 定义类 AudioManager 的成员函数 AudioManager。
AudioManager::AudioManager(QObject* parent)
    // 继续当前代码块的参数、表达式或结构定义。
    : QObject(parent)
// 进入新的代码块作用域。
{
// 结束当前代码块作用域。
}

// 定义类 AudioManager 的成员函数 ~AudioManager。
AudioManager::~AudioManager()
{
    // 关键：必须等**所有**异步播放任务真正结束后再析构。
    //
    // playAsync() 用 QtConcurrent::run 起后台任务，lambda 以 [=] 捕获了 this
    // 并会调用成员函数 playPcmFile()、发射 playbackStarted/playbackFinished。
    // 原实现析构函数为空，于是 Widget 析构（连带删除 parent 为 this 的
    // AudioManager）时，若后台任务仍在播放，就会在已析构对象上 emit 信号
    // （Qt 会访问已释放的 QObject 内部状态）并读取已释放的成员。
    //
    // 为什么保存的是**全部** future：playAsync() 每次把新任务赋给 playFuture，
    // 而 **QFuture 赋值不会等待**上一个任务（只转移内部 d 指针）。只留最后一个
    // 就没人能等到更早的任务了，故用 playFutures 全量登记。
    //
    // 第 16 轮的澄清（实测后更正）：早先此处断言"旧任务的 flag 已不再是成员
    // 指向的对象，stop() 打不断它"。**那是错的** —— lambda 捕获的是 shared_ptr
    // 的拷贝，与成员指向同一个 atomic，playAsync() 开头的 stop() 照样能打断它。
    // 又因为 playPcmFile() 入口持有静态 pcmMutex、各任务被完全串行化，
    // "只等最后一个"往往恰好也等到了更早的任务。所以本修复消除的是一个**窗口**
    // （任务在锁外 emit 信号期间对象被析构），而未必是必然的崩溃。
    // 相关行为由 tests/test_audio_manager.cpp 固定。
    //
    // 顺序：先置中断标志（让所有循环尽快 drop 并返回），再等待全部结束，
    // 最后才让成员被销毁。仅等待而不置标志的话，一段较长的提示音会让析构
    // 阻塞到整段播完。
    stop();
    waitForAllTasks();
}

// 定义或声明函数 AudioManager::waitForAllTasks。
void AudioManager::waitForAllTasks()
{
    // 先搬出来再等待：等待过程中不会再有新任务加入（调用方已 stop()），
    // 但清空成员可以避免下一轮 playAsync() 把已完成的 future 越积越多。
    //
    // 注意 pending 与循环变量都**不能**加 const：QFuture::waitForFinished()
    // 不是 const 成员（它要修改内部 d 的状态），const 引用会编不过：
    //   error: passing 'const QFuture<void>' as 'this' argument discards
    //          qualifiers [-fpermissive]
    // 而 isRunning() 是 const，所以这里对 f 用非 const 引用即可两者兼顾。
    QVector<QFuture<void>> pending = playFutures;
    playFutures.clear();
    for (QFuture<void>& f : pending) {
        if (f.isRunning()) {
            f.waitForFinished();
        }
    }
    // playFuture 是"最近一个任务"的别名（供既有代码/日志使用），一并失效。
    if (playFuture.isRunning()) {
        playFuture.waitForFinished();
    }
}

// 定义或声明函数 AudioManager::playAsync。
void AudioManager::playAsync(const QString& pcmFilePath)
// 进入新的代码块作用域。
{
    // 继续当前代码块的参数、表达式或结构定义。
    stop(); // 打断上一次
    // 回收已经结束的任务，避免 playFutures 无限增长。
    // 注意这里**只清理已完成的**：仍在运行的任务必须保留在列表里，
    // 否则析构时又会漏等它（这正是原实现只留一个 future 的缺陷）。
    {
        QVector<QFuture<void>> alive;
        alive.reserve(playFutures.size() + 1);
        for (const QFuture<void>& f : playFutures) {
            if (f.isRunning()) {
                alive.append(f);
            }
        }
        playFutures = alive;
    }
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
    // 与 playFuture 一并登记，使析构能等到**所有**捕获了 this 的任务。
    playFutures.append(playFuture);
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
    // 设备名来自 AppConfig，与音视频线程保持一致；用静态缓冲保证
    // 指针生命周期覆盖整个运行期（snd_pcm_open 不会保留该指针）。
    static const QByteArray deviceName =
        accessterminal::AppConfig::instance().audioDevice().toUtf8();
    int rc = snd_pcm_open(&pcm_handle, deviceName.constData(), SND_PCM_STREAM_PLAYBACK, 0);
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
        //
        // XRUN（-EPIPE）后必须 snd_pcm_prepare 复位再重写。
        //
        // ALSA 的写接口在缓冲欠载/过载后进入 XRUN 状态，此后**每一次**
        // writei 都会立刻返回 -EPIPE，直到调用方 prepare 复位。重构前的
        // 代码只打印一条 qWarning 就继续循环：
        //
        //     int rcw = snd_pcm_writei(...);
        //     if (rcw < 0) qWarning() << "...错误:" << snd_strerror(rcw);
        //
        // 于是一旦发生一次 XRUN，本段提示音**剩余的全部数据都会被静默丢掉**，
        // 而且因为每次 writei 都立即失败、不阻塞，循环会瞬间跑完 —— 现场
        // 表现为「这条提示音没响/只响了一个开头」，且只有一行警告，极难定位。
        //
        // 同工程的播放线程 AudioReceiverThread 早已处理了这一点
        // （`if (rcw == -EPIPE) { snd_pcm_prepare(...); continue; }`），
        // 本处与之对齐：EPIPE 时 prepare 并重写同一包，其它错误仍只告警。
        int rcw = 0;
        for (int attempt = 0; attempt < kWriteRetries; ++attempt) {
            rcw = snd_pcm_writei(pcm_handle, data.constData(), frames);
            if (rcw == -EPIPE) {
                // 缓冲被清空，设备需要重新 prepare 才能接受下一包。
                snd_pcm_prepare(pcm_handle);
                continue; // 重写同一包
            }
            break;
        }
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
