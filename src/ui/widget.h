/**
 * @file widget.h
 * @brief 门口机主界面：只负责 UI 装配与事件分发。
 *
 * 重构说明：原实现的 Widget 同时承担 UI、网络、硬件、AI 与线程管理，
 * 单文件超过 1300 行。现按分层拆解：
 *   - 信令       -> core/net/SignalingClient
 *   - 音视频线程  -> core/media 子模块
 *   - 硬件/协议   -> hal/fingerprint_hailin, hal/gpio
 *   - 业务逻辑    -> managers 子模块
 *   - 参数        -> config/AppConfig
 * Widget 仅保留界面控件操作与各模块之间的编排。
 */
#ifndef ACCESSTERMINAL_UI_WIDGET_H
#define ACCESSTERMINAL_UI_WIDGET_H

#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QThread>
#include <QVector>
#include <QWidget>
#include <atomic>
// cv::CascadeClassifier 由 objdetect 模块提供，仅 include core.hpp 不足以
// 获得其声明（已由主机端真实编译验证）。
#include <opencv2/core.hpp>
#include <opencv2/objdetect.hpp>

#include "core/net/protocol.h"

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

class QTimer;
class QResizeEvent;

// 工程内既有 Qt 类沿用全局命名空间（与重构前一致，降低改动风险）；
// 新增基础设施模块位于 accessterminal 命名空间内。
class AudioManager;
class CameraUdpServer;
class FaceManager;
class FingerprintManager;
class HailinFingerprint;
class PasswordManager;
class CameraPushThread;
class AudioSenderThread;
class AudioReceiverThread;
class V4L2Camera;
class Form_CALL;
class Form_finger;
class Form_passwd;

namespace accessterminal {
class SignalingClient;
class GpioLine;
class DoorActuator;
} // namespace accessterminal

// 使 protocol::Signal 可用于跨线程排队的信号槽。
Q_DECLARE_METATYPE(accessterminal::protocol::Signal)

/**
 * @brief 人体红外（PE4）监测线程。
 *
 * 独立线程轮询 GPIO 电平，通过信号通知 UI 线程是否有人靠近。
 * 通话期间可 pause，避免误触发。
 */
class GpioPe4MonitorThread : public QThread
{
    Q_OBJECT

public:
    explicit GpioPe4MonitorThread(QObject* parent = nullptr)
        : QThread(parent) {}

    /// 请求线程退出并在 run() 末尾释放 GPIO 资源。
    void stop() { m_running = false; }
    void pause() { m_paused = true; }
    void resume() { m_paused = false; }

signals:
    void pe4High();
    void pe4Low();

protected:
    void run() override;

private:
    std::atomic<bool> m_running{true};
    std::atomic<bool> m_paused{false};
};

/**
 * @brief 门口机主窗口。
 */
class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget* parent = nullptr);
    ~Widget() override;

    /// 供子窗口访问的共享模块。
    FingerprintManager* fingerprintManager() const { return m_fingerprintManager; }
    AudioManager* audioManager() const { return m_audioManager; }
    HailinFingerprint* fingerprint() const { return m_fingerprint; }

    /// 下一个可用的指纹 ID（注册流程使用）。
    int nextFingerId() const { return m_nextFingerId; }
    void incrementNextFingerId() { ++m_nextFingerId; }

    /**
     * @brief 通知室内机“本端已停止发送”（协议信令 stopsend）。
     *
     * 供通话窗口的本地挂断按钮调用。重构前该逻辑直接写在 Form_CALL 里
     * 并通过 widget->tcpSignalSocket 裸访问 socket；现在信令通道归
     * SignalingClient 所有，故收敛为一个明确的接口，避免 UI 层依赖
     * socket 细节。
     *
     * 注意：仅在**本地**挂断时发送，远端 recstop 触发的挂断不发，
     *       以保持与重构前完全一致的信令时序。
     */
    void notifyPeerStopSend();

private slots:
    // ---- 主界面按钮 ----
    void on_btncall_clicked();
    void on_btnadd_clicked();
    void on_btnpsswd_clicked();
    void on_btnpower_clicked();

    // ---- 信令 ----
    void onSignalReceived(accessterminal::protocol::Signal signal);

    // ---- 指纹 ----
    void onFingerprintIdentify(bool success, int id, int score, const QString& msg);
    void onFingerprintEnroll(bool success, int id, const QString& msg);

    // ---- 人脸 ----
    void onAddFaceFromFingerForm();
    void onFaceDetectResult(const QVector<cv::Rect>& faces, int imgWidth, int imgHeight);

private:
    /// 一次性装配：加载配置、创建模块、连接信号。
    void setupModules();
    /// 装配 GPIO（补光灯）与门锁执行器。
    void setupGpio();

    /**
     * @brief 认证成功后的**唯一**开锁入口。
     *
     * 密码 / 指纹 / 人脸 / 远程 unlock 四条路径都必须走这里，不允许任何
     * 一处直接写 GPIO。它只做一件事：把请求转给 m_doorActuator，由后者
     * 决定"真实开锁"还是"模拟开锁"（板端未接执行器时）。
     * 规则 `[actuator]` 守着"这里必须是唯一出口"。
     *
     * @param source 来源描述，用于日志与信号（"密码"/"指纹"/"人脸"/"远程"）。
     */
    void requestUnlock(const QString& source);

    /// 处理服务器 accept：创建音视频线程，进入通话。
    void startCallSession();
    /// 挂断：停止并销毁音视频线程、关闭通话窗口。
    void hangUpCall();

    /// 定时采集一帧：显示、异步人脸检测与识别。
    void updateFrame();

    /// 加载 Haar 人脸分类器。
    bool loadFaceCascade(const QString& path);

    void resizeEvent(QResizeEvent* event) override;

    Ui::Widget* ui = nullptr;

    // ---- UI 子窗口 ----
    Form_finger* m_fingerForm = nullptr;
    /// 密码输入窗口。用 QPointer 而非裸指针：该窗口设了
    /// Qt::WA_DeleteOnClose，点“确定”或用户关闭窗口后对象即被销毁；
    /// 裸指针会立即变成悬垂指针，而 passwordChecked 回调里还会访问它。
    /// QPointer 在被指向对象销毁后自动变空，是这里最直接的守卫。
    QPointer<Form_passwd> m_passwdForm;
    Form_CALL* m_callForm = nullptr;

    // ---- 业务模块 ----
    FingerprintManager* m_fingerprintManager = nullptr;
    AudioManager* m_audioManager = nullptr;      ///< 通用提示音
    AudioManager* m_connAudioManager = nullptr;  ///< 专用于 conn.pcm，可被单独打断
    FaceManager* m_faceManager = nullptr;
    PasswordManager* m_passwordManager = nullptr;
    HailinFingerprint* m_fingerprint = nullptr;

    // ---- 网络 ----
    accessterminal::SignalingClient* m_signaling = nullptr;

    // ---- 摄像头与视觉 ----
    V4L2Camera* m_camera = nullptr;
    CameraUdpServer* m_previewServer = nullptr;  ///< 远程监看（UDP 50000）
    cv::CascadeClassifier m_faceCascade;
    QVector<cv::Rect> m_faces;
    int m_collectCount = 0;

    // ---- 通话期间的音视频（仅在 accept 后创建）----
    AudioSenderThread* m_audioSenderThread = nullptr;
    AudioReceiverThread* m_audioReceiverThread = nullptr;
    CameraPushThread* m_cameraPushThread = nullptr;
    int m_audioSocketFd = -1;  ///< 音视频共用的 UDP socket，由 Widget 统一持有

    /// 通话会话是否处于活跃状态（accept 后置 true，hangUpCall() 置 false）。
    ///
    /// 存在的理由：音频播放线程是**延迟 400ms** 才创建的（错开声卡争用）。
    /// 若在这 400ms 内通话就被挂断（远端 recstop、本地挂断按钮、或采集线程
    /// 报告连接丢失），那个已排队的延时 lambda 仍会触发。没有本标志时它会
    /// 用已经被 close() 并置为 -1 的 m_audioSocketFd 去建线程，线程进入
    /// run() 后对 fd = -1 执行 fcntl/FD_SET，属于未定义行为。
    ///
    /// 所有“通话期间才有效”的延时/异步操作都必须先检查本标志。
    bool m_callActive = false;

    /// 通话会话代次计数器，每次 startCallSession() 递增。
    ///
    /// 配合 m_callActive 解决“旧延时任务误作用于新通话”的问题：
    /// 延时任务在排队时记下当时的代次，执行时若代次已变则说明它属于一次
    /// 已经结束的通话，必须丢弃。
    quint64 m_callGeneration = 0;

    // ---- GPIO 与轮询线程 ----
    GpioPe4MonitorThread* m_pe4Thread = nullptr;
    // 补光灯：PE5，线号 133。**只做补光**，不承担开锁语义
    // （第 40 轮把执行器拆了出去，见 m_doorActuator）。
    // libgpiod 对同一条线只允许一个 consumer，因此全工程只能有一个对象
    // 占用它；若板端确认 PE5 是继电器而非补光灯，则补光与开锁必须二选一，
    // 那是一次需要显式决策的现场行为变更（见 docs/借鉴Accessterminal方案.md）。
    accessterminal::GpioLine* m_fillLight = nullptr;
    // 门锁执行器：唯一的"开门"出口。板端未接执行器时走模拟开锁。
    // 认证成功的四条路径（密码/指纹/人脸/远程 unlock）都必须经过
    // requestUnlock()，不允许任何一处直接写 GPIO。
    accessterminal::DoorActuator* m_doorActuator = nullptr;
    bool m_hasPerson = false;

    // ---- 其它 ----
    QTimer* m_frameTimer = nullptr;
    int m_nextFingerId = 1;
};

#endif // ACCESSTERMINAL_UI_WIDGET_H
