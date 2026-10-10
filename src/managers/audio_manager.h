// 开始头文件防重复包含保护 AUDIO_MANAGER_H。
#ifndef AUDIO_MANAGER_H
// 定义预处理宏或常量 AUDIO_MANAGER_H。
#define AUDIO_MANAGER_H

// 引入系统或第三方头文件 <QObject>。
#include <QObject>
// 引入系统或第三方头文件 <QString>。
#include <QString>
// 引入系统或第三方头文件 <memory>。
#include <memory>
// 引入系统或第三方头文件 <atomic>。
#include <atomic>
// 引入系统或第三方头文件 <QFuture>。
#include <QFuture>
// 引入系统或第三方头文件 <QVector>。
#include <QVector>

// 声明类 AudioManager。
class AudioManager : public QObject
// 进入新的代码块作用域。
{
    // 继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
// 声明类成员的 public 访问权限区域。
public:
    // 声明显式构造函数 AudioManager。
    explicit AudioManager(QObject* parent = nullptr);
    // 声明或定义析构函数 AudioManager。
    ~AudioManager();

    // 异步播放音频，支持打断
    // 定义或声明函数 playAsync。
    void playAsync(const QString& pcmFilePath);
    // 停止当前播放
    // 定义或声明函数 stop。
    void stop();

// 声明 Qt 信号槽成员区域。
signals:
    // 定义或声明函数 playbackStarted。
    void playbackStarted(const QString& file);
    // 定义或声明函数 playbackFinished。
    void playbackFinished(const QString& file);

// 声明类成员的 private 访问权限区域。
private:
    // 执行本行语句完成对应变量、对象或业务状态处理。
    std::shared_ptr<std::atomic<bool>> interruptFlag;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QFuture<void> playFuture;
    /**
     * @brief 所有仍在运行、且捕获了 this 的异步播放任务。
     *
     * 为什么必须保存**全部**而不是只留最后一个：
     * `playAsync()` 每次都会 `stop(); interruptFlag = make_shared(...)` 然后
     * `playFuture = QtConcurrent::run(...)`。给 `playFuture` **赋值并不会等待**
     * 上一个任务结束（`QFuture` 赋值只转移内部 d 指针的引用，析构也不阻塞），
     * 所以上一个任务可能仍在 `playPcmFile()` 里跑，而它捕获的 `this` 与旧
     * `flag` 仍在栈上。此时：
     *   - 旧任务的 `flag` 已不再是 `interruptFlag` 指向的对象，`stop()` 再也
     *     打不断它（它会一直把当前提示音播完，并继续持有静态 pcmMutex）；
     *   - 更糟的是析构函数只 `waitForFinished()` 最后一个 `playFuture`，
     *     旧任务**完全不被等待**，于是 `~AudioManager()` 返回后它仍在访问
     *     已释放的 this（emit 信号、读成员）—— use-after-free。
     */
    QVector<QFuture<void>> playFutures;
    /// 等待所有未完成任务结束，并清理已完成项（调用前应先 stop()）。
    void waitForAllTasks();
    // 定义或声明函数 playPcmFile。
    void playPcmFile(const QString& pcmFilePath, std::atomic<bool>* interruptFlag);
// 结束当前代码块作用域。
};

// 结束当前预处理条件块。
#endif // AUDIO_MANAGER_H
