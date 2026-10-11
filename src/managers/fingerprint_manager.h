// 开始头文件防重复包含保护 FINGERPRINT_MANAGER_H。
#ifndef FINGERPRINT_MANAGER_H
// 定义预处理宏或常量 FINGERPRINT_MANAGER_H。
#define FINGERPRINT_MANAGER_H

// 引入系统或第三方头文件 <QObject>。
#include <QObject>

// 声明类 HailinFingerprint。
class HailinFingerprint;
// 声明类 QTimer（仅以指针成员持有，无需完整定义）。
class QTimer;

// 声明类 FingerprintManager。
class FingerprintManager : public QObject
// 进入新的代码块作用域。
{
    // 继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
// 声明类成员的 public 访问权限区域。
public:
    // 声明显式构造函数 FingerprintManager。
    explicit FingerprintManager(QObject* parent = nullptr);
    // 声明或定义析构函数 FingerprintManager。
    ~FingerprintManager();

    // 定义或声明函数 setDevice。
    void setDevice(HailinFingerprint* device);

    /**
     * @brief 暂停/恢复常驻识别轮询（与指纹注册流程互斥）。
     *
     * 为什么必须互斥：注册流程（Form_finger）与常驻识别（startIdentify）
     * 操作的是**同一个串口**。识别轮询每 500ms 主动下发 CMD_GET_IMAGE，
     * 然后 readPacket(500) 去抢读应答；而注册流程同样在下发命令后
     * readPacket(2000) 等待**它自己**的应答。两者会互为对方之应答：
     *  - 识别轮询把注册流程等待的 0x02/0x05/0x06 应答从串口缓冲里取走，
     *    注册流程拿到空字节数组，判为“采集图像失败”而静默失败；
     *  - 反之注册流程取走识别应答，识别轮询也只看到失败。
     * 重构前 baseline 就存在这个问题，只是「步骤注册」那条路径从未被
     * 真正连上（见 widget.cpp 中已删除的 connect），所以表现出来只是
     * 注册偶发失败。
     *
     * 进入注册窗口时置 true：识别轮询会立即停止，且在暂停期间不会再被
     * 拉起；退出注册窗口时置 false：常驻识别自动重新开始。
     */
    void setSuspended(bool suspended);

    /// 当前是否处于暂停状态。
    bool isSuspended() const { return m_suspended; }

    /**
     * @brief 串口打不开时的重试间隔（毫秒）。
     *
     * 为什么要重试而不是直接放弃：指纹串口在**启动时**打不开是常态
     * （设备缺失、权限不对、USB/串口在启动之后才枚举出来）。原实现不检查
     * 串口状态就进入常驻识别循环，于是以 ~2Hz 反复写一个关闭的串口：
     * 每轮刷出 Qt 自带的 "QIODevice::write (QSerialPort): device not open"
     * 与 "[指纹] 串口写入不完整"，且**永远不会自愈**（全工程只有一处
     * open()，在 widget.cpp 启动时调用一次）。
     *
     * 现在的行为：串口不可用时不进识别循环，改为按本间隔低频重试 open()，
     * 一旦可用就自动恢复识别，不需要重启进程。
     *
     * 对外暴露这个数字，是为了让测试与文档只有一个事实来源
     * （同 FaceManager::sampleTarget() 的理由）。
     */
    static int reopenRetryMs() { return 5000; }

// 声明 Qt 信号槽成员区域。
public slots:
    // 定义或声明函数 startEnroll。
    void startEnroll(int id);      // 开始注册流程
    // 定义或声明函数 startIdentify。
    void startIdentify();          // 开始识别流程

// 声明 Qt 信号槽成员区域。
signals:
    // 注册结果：success 是否成功，id 注册ID，msg 详细信息
    // 定义或声明函数 enrollResult。
    void enrollResult(bool success, int id, const QString& msg);
    // 识别结果：success 是否成功，id 匹配到的ID，score 匹配分数，msg 详细信息
    // 定义或声明函数 identifyResult。
    void identifyResult(bool success, int id, int score, const QString& msg);

// 声明类成员的 private 访问权限区域。
private:
    // 执行本行语句完成对应变量、对象或业务状态处理。
    HailinFingerprint* fingerprint = nullptr;
    /// 常驻识别轮询的句柄。
    ///
    /// 必须保存：轮询回调里 readPacket() 是**阻塞读**且跑在 GUI 线程上，
    /// 只有“读到手指图像”那一条路径会停它。baseline 用的是局部变量
    /// `QTimer* timer = new QTimer(this);`，外部谁也拿不到句柄，于是注册
    /// 流程无法让它先停下来（串口抢读），窗口析构也无法让它停止空转。
    QTimer* m_pollTimer = nullptr;
    /// 注册流程轮询的句柄（回调里同为阻塞读，必须可暂停/停止）。
    QTimer* m_enrollTimer = nullptr;
    /// 注册期间置 true，识别轮询不得运行。
    bool m_suspended = false;
    /// 是否正在等待手指（防重入）。原实现是函数内 static，跨实例共享。
    bool m_waitingFinger = false;
    /// 是否正在注册（防重入）。原实现是函数内 static，跨实例共享。
    bool m_enrolling = false;
    /// 串口不可用时的重试定时器句柄（单次触发，可复用；未排程时为 nullptr）。
    QTimer* m_reopenTimer = nullptr;
    /// 是否已经就"串口不可用"提示过一次（避免每 5 秒刷一条同样的日志）。
    bool m_portUnavailable = false;
    /// 停掉识别/注册轮询定时器并清空句柄。
    void stopPoll();
    /// 串口不可用时排一次低频重试（已在排程中则不重复排）。
    void scheduleReopenRetry();
    /// 重试到期：再试一次 open()，成功则恢复识别，失败则重新排程。
    void onReopenRetry();
// 结束当前代码块作用域。
};

// 结束当前预处理条件块。
#endif // FINGERPRINT_MANAGER_H
