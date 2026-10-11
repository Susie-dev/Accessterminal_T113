/**
 * @file widget.cpp
 * @brief 门口机主窗口实现：UI 装配与模块编排。
 *
 * 行为契约（相对重构前保持不变，务必遵守）：
 *   1. 音视频线程只在收到室内机 "accept" 之后才创建，待机时零占用。
 *   2. 未连接信令服务器时点击呼叫只提示错误，不进入通话。
 *   3. 挂断（本地按钮或远端 recstop）统一走 hangUpCall()，
 *      必须停止并销毁全部音视频线程、关闭 UDP socket 与通话窗口。
 *   4. 指纹识别在启动后自动循环，识别完成 500ms 后重新开始。
 *   5. 人脸识别仅在检测到人体（PE4 高电平）时才进行。
 */
#include "ui/widget.h"
#include "ui_widget.h"

#include "config/app_config.h"
#include "core/media/audio_receiver_thread.h"
#include "core/media/audio_sender_thread.h"
#include "core/media/camera_preview_server.h"
#include "core/media/video_push_thread.h"
#include "core/net/protocol.h"
#include "core/net/signaling_client.h"
#include "core/vision/v4l2_camera.h"
#include "hal/fingerprint_hailin.h"
#include "hal/gpio.h"
#include "managers/access_controller.h"
#include "managers/audio_manager.h"
#include "managers/door_actuator.h"
#include "managers/face_manager.h"
#include "managers/fingerprint_manager.h"
#include "managers/password_manager.h"
#include "ui/form_call.h"
#include "ui/form_finger.h"
#include "ui/form_passwd.h"

#include <QBuffer>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QtConcurrent/QtConcurrent>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <errno.h>

using accessterminal::AccessMethod;
using accessterminal::AppConfig;
using accessterminal::protocol::Signal;

namespace {

/// 摄像头画面在界面上的固定显示尺寸。
constexpr int kPreviewWidth = 430;
constexpr int kPreviewHeight = 475;

/// 人脸注册所需样本数不再单独定义常量：改用
/// FaceManager::sampleTarget()，避免两个 3 各改各的造成静默错位。

/// 异步人脸检测时缩小的尺寸，降低 Haar 计算量。
constexpr int kDetectWidth = 320;
constexpr int kDetectHeight = 240;

/// 线程等待退出的最长时间。
constexpr int kThreadStopTimeoutMs = 3000;

/// 开锁脉冲宽度（毫秒）：与重构前"开锁 1 秒"的现场语义一致。
/// 将来若要现场可调，应在 AppConfig 里加旋钮并遵守开发约定第 10 条
/// （公开的开关必须真的生效）；留给显式决策，不在这里悄悄写死配置读取。
constexpr int kUnlockPulseMs = 1000;

/// 放行静默期（毫秒）：**0 = 保持现状，不抑制**。
/// 取非 0（例如 5000）会改变现场行为：同一段时间内第二次认证不再开门 ——
/// 这正是"门口站着的人被 3Hz 人脸识别反复开门"的解药，但改了就与
/// "行为兼容"冲突，必须显式决策（见 docs/交接与继续开发.md 的待裁决事项）。
constexpr int kUnlockSilentPeriodMs = 0;

} // namespace

// ============================================================================
// GpioPe4MonitorThread：人体红外轮询
// ============================================================================

void GpioPe4MonitorThread::run()
{
    // PE4 为人体/红外检测输入。使用统一封装，避免散落的 gpiod 调用。
    accessterminal::GpioLine line;
    if (!line.openInput(QStringLiteral("gpiochip0"), 132, QStringLiteral("pe4_monitor"))) {
        qWarning() << "[红外] PE4 初始化失败，人体检测不可用";
        return;
    }

    while (m_running) {
        if (m_paused) {
            msleep(100); // 暂停时短暂休眠，避免 CPU 空转
            continue;
        }
        const int value = line.value();
        if (value < 0) {
            qWarning() << "[红外] PE4 读取失败，停止监测";
            break;
        }
        if (value == 1) {
            emit pe4High();
        } else {
            emit pe4Low();
        }
        msleep(400); // 约 400ms 轮询一次，降低抖动与 CPU 占用
    }
}

// ============================================================================
// Widget：构造与析构
// ============================================================================

Widget::Widget(QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{
    ui->setupUi(this);

    // 触摸屏场景下移除按钮焦点框，避免点击后留下虚线。
    const QList<QPushButton*> buttons = {
        ui->btnadd, ui->btncall, ui->btnface,
        ui->btnpower, ui->btnpsswd, ui->btnfinger_step
    };
    for (QPushButton* btn : buttons) {
        btn->setFocusPolicy(Qt::NoFocus);
    }
    setStyleSheet(QStringLiteral("QPushButton:focus { outline: none; }"));

    // 隐藏鼠标指针（嵌入式触摸屏无鼠标）。
    setCursor(Qt::BlankCursor);

    // 摄像头显示区域：透明背景 + 圆角。
    ui->labelcam->setAttribute(Qt::WA_TranslucentBackground, true);
    if (ui->labelcam->parentWidget()) {
        ui->labelcam->parentWidget()->setAttribute(Qt::WA_TranslucentBackground, true);
    }
    ui->labelcam->setStyleSheet(QStringLiteral(
        "background-color: rgb(85, 87, 83); border: none;"));

    // ---- 日期时间显示：每秒刷新 ----
    auto* dateTimeTimer = new QTimer(this);
    connect(dateTimeTimer, &QTimer::timeout, this, [this]() {
        const QDateTime now = QDateTime::currentDateTime();
        ui->labelDate->setText(now.date().toString(QStringLiteral("yyyy-MM-dd")));
        ui->labelTime->setText(now.time().toString(QStringLiteral("HH:mm:ss")));
    });
    dateTimeTimer->start(1000);

    const QDateTime now = QDateTime::currentDateTime();
    ui->labelDate->setText(now.date().toString(QStringLiteral("yyyy-MM-dd")));
    ui->labelTime->setText(now.time().toString(QStringLiteral("HH:mm:ss")));

    qInfo() << "[启动] 智能门禁系统初始化";

    setupGpio();
    setupModules();

    qInfo() << "[完成] 系统初始化完成，等待用户操作";
}

Widget::~Widget()
{
    // 顺序很重要：先停线程，再销毁其依赖的 socket 与硬件。

    if (m_cameraPushThread) {
        m_cameraPushThread->stop();
        m_cameraPushThread->wait(kThreadStopTimeoutMs);
        delete m_cameraPushThread;
        m_cameraPushThread = nullptr;
    }
    if (m_audioSenderThread) {
        m_audioSenderThread->stop();
        m_audioSenderThread->wait(kThreadStopTimeoutMs);
        delete m_audioSenderThread;
        m_audioSenderThread = nullptr;
    }
    if (m_audioReceiverThread) {
        m_audioReceiverThread->stop();
        m_audioReceiverThread->wait(kThreadStopTimeoutMs);
        delete m_audioReceiverThread;
        m_audioReceiverThread = nullptr;
    }
    if (m_audioSocketFd >= 0) {
        ::close(m_audioSocketFd);
        m_audioSocketFd = -1;
    }
    if (m_frameTimer) {
        m_frameTimer->stop();
        delete m_frameTimer;
        m_frameTimer = nullptr;
    }
    if (m_pe4Thread) {
        m_pe4Thread->stop();
        m_pe4Thread->wait(kThreadStopTimeoutMs);
        delete m_pe4Thread;
        m_pe4Thread = nullptr;
    }
    if (m_fingerprint) {
        m_fingerprint->close();
        delete m_fingerprint;
        m_fingerprint = nullptr;
    }
    if (m_camera) {
        m_camera->close();
        delete m_camera;
        m_camera = nullptr;
    }
    // GPIO 线由 GpioLine 析构自动释放（补光灯；门锁执行器由 QObject 父子关系回收）。
    delete m_fillLight;
    m_fillLight = nullptr;

    delete ui;
}

// ============================================================================
// 装配
// ============================================================================

void Widget::setupGpio()
{
    const AppConfig& cfg = AppConfig::instance();

    // 补光灯：PE5（线号默认 133）。全工程只允许一个对象占用这条线。
    m_fillLight = new accessterminal::GpioLine();
    if (!m_fillLight->openOutput(QStringLiteral("gpiochip0"),
                               static_cast<unsigned int>(cfg.doorGpioLine()),
                               QStringLiteral("accessterminal"), 0)) {
        qWarning() << "[GPIO] PE5 初始化失败，补光不可用";
        delete m_fillLight;
        m_fillLight = nullptr;
    }

    // 门锁执行器：与补光**分离**的唯一开锁出口。
    //
    // 这里刻意**不接线**（setLine(nullptr)）：板端尚未确认门锁执行器究竟
    // 接在哪条线 —— app_config.h 的注释说"开锁继电器与补光灯共用 PE5"，
    // 而相邻仓库的板端实测结论是"PE5 是补光灯，不是门锁"。在事实裁决之前
    // 把 PE5 接给执行器，就等于"采集人脸时给锁上电"，是危险的猜测。
    // 不接线时 unlock() 走**模拟开锁**：明确日志 + realActuator=false，
    // 绝不假装门开了。裁决结论出来后再在这里填线。
    // 见 docs/借鉴Accessterminal方案.md 的 P0 与"需要显式决策的事项"。
    m_doorActuator = new accessterminal::DoorActuator(this);
    m_doorActuator->setDurationMs(kUnlockPulseMs);
    m_doorActuator->setLine(nullptr);

    // 放行决策中枢：唯一的"谁能进门"出口。
    m_accessController = new accessterminal::AccessController(this);
    m_accessController->setActuator(m_doorActuator);
    // 静默期：0 = **保持现状**（不做任何抑制）。
    // 取非 0 会改变现场行为（同一段时间内的第二次认证不再驱动门锁），
    // 属于需要显式决策的变更，因此默认 0；能力本身有单元测试覆盖
    // （tests/test_access_controller.cpp）。
    m_accessController->setSilentPeriodMs(kUnlockSilentPeriodMs);
    connect(m_accessController, &accessterminal::AccessController::actuated, this,
            [](const QString& source, bool realActuator) {
                // 只在"没真的开门"时出声：现场必须能一眼看出是模拟开锁，
                // 否则会把提示音当成门开了。
                if (!realActuator) {
                    qWarning() << "[开锁] 本次为模拟开锁（板端未接门锁执行器），来源:"
                               << source;
                }
            });
}

void Widget::requestUnlock(accessterminal::AccessMethod method, const QString& source)
{
    // 认证成功的**唯一**开锁出口。四条路径（密码/指纹/人脸/远程）都必须
    // 经过这里；规则 [actuator] 会拦住任何"在别处直接写 GPIO"的写法。
    m_accessController->request(method, source);
}

void Widget::setupModules()
{
    const AppConfig& cfg = AppConfig::instance();

    // ---- 提示音 ----
    m_audioManager = new AudioManager(this);
    m_connAudioManager = new AudioManager(this); // 独立实例，conn.pcm 可被单独打断

    // ---- 指纹：串口设备来自配置 ----
    m_fingerprint = new HailinFingerprint(cfg.fingerprintPort(), this);
    if (m_fingerprint->open()) {
        qInfo() << "[指纹] 模块串口打开成功:" << cfg.fingerprintPort();
    } else {
        qWarning() << "[指纹] 模块串口打开失败:" << cfg.fingerprintPort();
    }

    m_fingerprintManager = new FingerprintManager(this);
    m_fingerprintManager->setDevice(m_fingerprint);
    m_nextFingerId = cfg.fingerprintIdStart();

    // 指纹识别结果 -> 提示音，并循环重启识别。
    connect(m_fingerprintManager, &FingerprintManager::identifyResult, this,
            [this, &cfg](bool success, int /*id*/, int /*score*/, const QString& msg) {
                if (msg == QStringLiteral("请按下指纹进行识别")) {
                    return; // 采集提示不播报，避免持续噪音
                }
                m_audioManager->playAsync(
                    cfg.pcmFile(success ? QStringLiteral("fingerdoor.pcm")
                                        : QStringLiteral("fingererr.pcm")));
                if (success) {
                    // 指纹通过 -> 开锁。**这一条才是现场真正执行的路径**：
                    // Form_finger 的内联状态机与这里的 identifyResult 是产线
                    // 路径，原先那个 Widget::onFingerprintIdentify 槽没有任何
                    // 调用者（verify.py 的 [dead-slot] 一直在告警），已删除。
                    requestUnlock(AccessMethod::Fingerprint, QStringLiteral("指纹"));
                }
                // 识别完成后延时重新开始，实现常驻循环。
                QTimer::singleShot(500, this, [this]() {
                    m_fingerprintManager->startIdentify();
                });
            });

    // 指纹注册结果 -> 分步提示音。
    connect(m_fingerprintManager, &FingerprintManager::enrollResult, this,
            [this](bool success, int /*id*/, const QString& msg) {
                const AppConfig& c = AppConfig::instance();
                if (msg.contains(QStringLiteral("请按下手指进行"))) {
                    m_audioManager->playAsync(c.pcmFile(QStringLiteral("caijifinger.pcm")));
                    return;
                }
                if (msg.contains(QStringLiteral("采集成功"))) {
                    m_audioManager->playAsync(c.pcmFile(QStringLiteral("fingerone.pcm")));
                    return;
                }
                if (success && msg == QStringLiteral("注册成功")) {
                    m_audioManager->playAsync(c.pcmFile(QStringLiteral("opendoor.pcm")));
                } else if (!success) {
                    m_audioManager->playAsync(c.pcmFile(QStringLiteral("fingererr.pcm")));
                }
            });

    // 程序启动即开始指纹识别。
    QTimer::singleShot(0, this, [this]() { m_fingerprintManager->startIdentify(); });

    // ---- 人脸 ----
    m_faceManager = new FaceManager(this);
    // 判定阈值来自配置（重构前是 FaceManager 内硬编码的 90，同名配置项不生效）。
    m_faceManager->setConfidenceThreshold(cfg.faceConfidenceThreshold());

    const bool cascadeLoaded = loadFaceCascade(cfg.faceCascadePath());
    qInfo() << "[人脸检测] 分类器加载" << (cascadeLoaded ? "成功" : "失败")
            << cfg.faceCascadePath();

    connect(m_faceManager, &FaceManager::faceSampled, this,
            [this](int current, int /*total*/) {
                const AppConfig& c = AppConfig::instance();
                // 采集中打开补光，第 1 张开、第 3 张关（保留原行为）。
                if (current == 1) {
                    m_audioManager->playAsync(c.pcmFile(QStringLiteral("faceone.pcm")));
                    if (m_fillLight) m_fillLight->setValue(1);
                } else if (current == 2) {
                    m_audioManager->playAsync(c.pcmFile(QStringLiteral("facetwo.pcm")));
                } else if (current == 3) {
                    m_audioManager->playAsync(c.pcmFile(QStringLiteral("facethree.pcm")));
                    if (m_fillLight) m_fillLight->setValue(0);
                }
            });

    connect(m_faceManager, &FaceManager::trainFinished, this, [](bool success) {
        qInfo() << "[人脸] 训练" << (success ? "完成" : "失败，样本不足");
    });

    connect(m_faceManager, &FaceManager::recognizeResult, this,
            [this](bool success, double confidence) {
                qInfo() << "[人脸识别] 结果:" << (success ? "成功" : "失败")
                        << "confidence=" << confidence;
                if (m_fillLight) m_fillLight->setValue(0);
                const AppConfig& c = AppConfig::instance();
                m_audioManager->playAsync(
                    c.pcmFile(success ? QStringLiteral("opendoor.pcm")
                                      : QStringLiteral("faceerr.pcm")));
                if (success) {
                    // 人脸通过 -> 开锁。提示音照旧先响（不改变现场时序）。
                    requestUnlock(AccessMethod::Face, QStringLiteral("人脸"));
                }
            });

    // ---- 密码 ----
    m_passwordManager = new PasswordManager(this);
    connect(m_passwordManager, &PasswordManager::passwordChecked, this,
            [this](bool success) {
                const AppConfig& c = AppConfig::instance();
                if (success) {
                    m_audioManager->playAsync(c.pcmFile(QStringLiteral("opendoor.pcm")));
                    // 密码通过 -> 开锁（唯一出口）。
                    requestUnlock(AccessMethod::Password, QStringLiteral("密码"));
                    if (m_passwdForm) {
                        m_passwdForm->close();
                        m_passwdForm = nullptr;
                    }
                } else {
                    m_audioManager->playAsync(c.pcmFile(QStringLiteral("passwderr.pcm")));
                }
            });

    // ---- 摄像头与远程监看 ----
    m_camera = new V4L2Camera(cfg.cameraDevice());
    if (!m_camera->open()) {
        qWarning() << "[摄像头] 无法打开设备:" << cfg.cameraDevice();
        QMessageBox::critical(this, QStringLiteral("错误"),
                              QStringLiteral("无法打开摄像头"));
    } else {
        qInfo() << "[摄像头] 设备打开成功:" << cfg.cameraDevice();
    }
    m_previewServer = new CameraUdpServer(
        m_camera, accessterminal::protocol::kPreviewPort, this);

    // 采集+显示定时器。
    m_frameTimer = new QTimer(this);
    connect(m_frameTimer, &QTimer::timeout, this, &Widget::updateFrame);
    m_frameTimer->start(cfg.frameIntervalMs());

    // ---- 人体红外 ----
    m_pe4Thread = new GpioPe4MonitorThread(this);
    connect(m_pe4Thread, &GpioPe4MonitorThread::pe4Low, this, [this]() {
        m_hasPerson = false;
    });
    connect(m_pe4Thread, &GpioPe4MonitorThread::pe4High, this, [this]() {
        m_hasPerson = true;
    });
    m_pe4Thread->start();

    // ---- 信令 ----
    m_signaling = new accessterminal::SignalingClient(this);
    connect(m_signaling, &accessterminal::SignalingClient::signalReceived,
            this, &Widget::onSignalReceived);
    connect(m_signaling, &accessterminal::SignalingClient::connectedChanged,
            this, [](bool connected) {
                qInfo() << "[信令] 连接状态:" << (connected ? "已连接" : "已断开");
            });

    qInfo() << "[网络配置] 本机:" << cfg.selfIp() << "服务器:" << cfg.serverIp();
    m_signaling->start(cfg.serverIp(), accessterminal::protocol::kSignalingPort);
}

bool Widget::loadFaceCascade(const QString& path)
{
    return m_faceCascade.load(path.toStdString());
}

// ============================================================================
// 通话会话
// ============================================================================

void Widget::startCallSession()
{
    // 只打断 conn.pcm，不影响主提示音播放器。
    if (m_connAudioManager) {
        m_connAudioManager->stop();
    }
    qInfo() << "[信令] 收到 accept，创建音视频线程";

    const AppConfig& cfg = AppConfig::instance();

    // ---- 1. 创建并绑定音频 UDP socket（音视频共用）----
    if (m_audioSocketFd < 0) {
        m_audioSocketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (m_audioSocketFd < 0) {
            qWarning() << "[音频] 创建 UDP socket 失败, errno=" << errno;
        } else {
            struct sockaddr_in localAddr;
            std::memset(&localAddr, 0, sizeof(localAddr));
            localAddr.sin_family = AF_INET;
            localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
            localAddr.sin_port = htons(accessterminal::protocol::kAudioPort);
            if (::bind(m_audioSocketFd,
                       reinterpret_cast<struct sockaddr*>(&localAddr),
                       sizeof(localAddr)) < 0) {
                qWarning() << "[音频] 绑定端口"
                           << accessterminal::protocol::kAudioPort
                           << "失败, errno=" << errno;
                ::close(m_audioSocketFd);
                m_audioSocketFd = -1;
            } else {
                qInfo() << "[音频] 已绑定端口" << accessterminal::protocol::kAudioPort;
            }
        }
    }

    if (m_audioSocketFd < 0) {
        qWarning() << "[音频] 无法创建/绑定 socket，音视频线程不启动";
        return;
    }

    // 音频 socket 已就绪，通话会话正式开始。此后所有延时/异步操作都要用
    // m_callActive 判断这次会话是否仍然有效（见 widget.h 中的说明）。
    m_callActive = true;

    // ---- 2. 视频推流线程 ----
    if (m_cameraPushThread) {
        m_cameraPushThread->stop();
        m_cameraPushThread->wait(kThreadStopTimeoutMs);
        delete m_cameraPushThread;
        m_cameraPushThread = nullptr;
    }
    m_cameraPushThread = new CameraPushThread(m_camera, cfg.serverIp(),
                                             accessterminal::protocol::kVideoPort, this);
    m_cameraPushThread->start();

    // ---- 3. 音频采集线程 ----
    if (m_audioSenderThread) {
        m_audioSenderThread->stop();
        m_audioSenderThread->wait(kThreadStopTimeoutMs);
        delete m_audioSenderThread;
        m_audioSenderThread = nullptr;
    }
    m_audioSenderThread = new AudioSenderThread(m_audioSocketFd, cfg.serverIp(),
                                                accessterminal::protocol::kAudioPort, this);
    connect(m_audioSenderThread, &AudioSenderThread::errorMsg, this,
            [](const QString& msg) { qWarning() << "[音频发送]" << msg; });
    m_audioSenderThread->start();
    // 采集线程报告连接丢失时立即挂断，避免通话卡死。
    connect(m_audioSenderThread, &AudioSenderThread::connectionLost,
            this, &Widget::hangUpCall);

    // ---- 4. 音频播放线程：延迟 400ms 启动，避免与采集争用声卡 ----
    //
    // 用 m_callGeneration 作为“会话代次”令牌而不是只检查 m_callActive：
    // 若本次通话在 400ms 内被挂断、随后又立刻接通了**新**的一次通话，
    // 此时 m_callActive 已重新为 true，仅凭它无法区分“本代的延时任务”与
    // “新一代的会话”，旧令牌会误在新通话上再建一个播放线程。
    // 代次令牌使过期的延时任务必然失效。
    const quint64 generation = ++m_callGeneration;
    QTimer::singleShot(400, this, [this, generation]() {
        // 会话已结束，或已被新一次通话取代：丢弃这个过期的延时任务。
        if (!m_callActive || generation != m_callGeneration) {
            qDebug() << "[音频] 延迟启动播放线程被跳过（会话已结束或已换代）";
            return;
        }
        // 二重防御：fd 必须在建线程前仍然有效。
        // 绝不能把 -1 传给 AudioReceiverThread —— 其 run() 会对该 fd 调用
        // fcntl() 与 FD_SET()，属于未定义行为。
        if (m_audioSocketFd < 0) {
            qWarning() << "[音频] socket 已关闭，不启动播放线程";
            return;
        }
        if (m_audioReceiverThread) {
            m_audioReceiverThread->stop();
            m_audioReceiverThread->wait(kThreadStopTimeoutMs);
            delete m_audioReceiverThread;
            m_audioReceiverThread = nullptr;
        }
        m_audioReceiverThread = new AudioReceiverThread(m_audioSocketFd, this);
        m_audioReceiverThread->setVolume(30); // 默认 30% 音量
        connect(m_audioReceiverThread, &AudioReceiverThread::errorMsg, this,
                [](const QString& msg) { qWarning() << "[音频接收]" << msg; });
        m_audioReceiverThread->start();
        // 播放线程报告连接丢失时立即挂断（保留重构前行为）。
        connect(m_audioReceiverThread, &AudioReceiverThread::connectionLost,
                this, &Widget::hangUpCall);
    });

    // ---- 5. 通话计时 ----
    if (m_callForm) {
        m_callForm->startCallTimer();
    }
    qInfo() << "[完成] 音视频线程全部启动";
}

void Widget::hangUpCall()
{
    // 第一件事就把会话标记为结束：此后任何尚未执行的延时任务（例如延迟
    // 400ms 创建音频播放线程的那个）都会据此自行放弃，不会再去碰即将被
    // 关闭的 socket。必须放在拆解逻辑之前。
    m_callActive = false;
    ++m_callGeneration;

    if (m_connAudioManager) {
        m_connAudioManager->stop();
    }

    // 1. 关闭通话窗口。
    if (m_callForm) {
        m_callForm->stopCallTimer();
        m_callForm->close();
    }

    // 2. 停止并销毁音视频线程（每个线程都带超时，避免主线程卡死）。
    if (m_cameraPushThread) {
        m_cameraPushThread->stop();
        if (!m_cameraPushThread->wait(kThreadStopTimeoutMs)) {
            qWarning() << "[挂断] 视频线程退出超时";
        }
        delete m_cameraPushThread;
        m_cameraPushThread = nullptr;
    }
    if (m_audioSenderThread) {
        m_audioSenderThread->stop();
        if (!m_audioSenderThread->wait(kThreadStopTimeoutMs)) {
            qWarning() << "[挂断] 音频采集线程退出超时";
        }
        delete m_audioSenderThread;
        m_audioSenderThread = nullptr;
    }
    if (m_audioReceiverThread) {
        m_audioReceiverThread->stop();
        if (!m_audioReceiverThread->wait(kThreadStopTimeoutMs)) {
            qWarning() << "[挂断] 音频播放线程退出超时";
        }
        delete m_audioReceiverThread;
        m_audioReceiverThread = nullptr;
    }

    // 3. 释放通话窗口对象。
    if (m_callForm) {
        delete m_callForm;
        m_callForm = nullptr;
    }

    // 4. 关闭音频 socket。
    if (m_audioSocketFd >= 0) {
        ::close(m_audioSocketFd);
        m_audioSocketFd = -1;
    }

    // 5. 恢复人体检测。
    if (m_pe4Thread) {
        m_pe4Thread->resume();
    }
}

void Widget::onSignalReceived(Signal signal)
{
    switch (signal) {
    case Signal::Accept:
        startCallSession();
        break;
    case Signal::RecStop:
        qInfo() << "[信令] 收到 recstop，自动挂断";
        hangUpCall();
        break;
    case Signal::Unlock:
        qInfo() << "[信令] 收到 unlock，播放开门音";
        if (m_audioManager) {
            m_audioManager->playAsync(
                AppConfig::instance().pcmFile(QStringLiteral("opendoor.pcm")));
        }
        // 室内机的远程开锁 -> 本机开锁（唯一出口）。
        requestUnlock(AccessMethod::Remote, QStringLiteral("远程"));
        break;
    default:
        // 门口机不处理 call/getframe/stop/stopsend，忽略即可。
        break;
    }
}

// ============================================================================
// 信令发送
// ============================================================================

void Widget::notifyPeerStopSend()
{
    // 重构前 Form_CALL 的判断是
    //   if (widget->tcpSignalSocket &&
    //       widget->tcpSignalSocket->state() == QAbstractSocket::ConnectedState)
    // SignalingClient::send() 内部已做“未连接则告警并返回 false”的等价判断，
    // 因此这里直接调用即可，信令内容与时机保持一致。
    if (m_signaling) {
        m_signaling->send(accessterminal::protocol::Signal::StopSend);
    }
}

// ============================================================================
// 按钮响应
// ============================================================================

void Widget::on_btncall_clicked()
{
    // 先播放呼叫音，结束后接等待音 conn.pcm（独立播放器，可被 accept 打断）。
    m_audioManager->playAsync(
        AppConfig::instance().pcmFile(QStringLiteral("calling.pcm")));

    auto* conn = new QMetaObject::Connection;
    *conn = connect(m_audioManager, &AudioManager::playbackFinished, this,
                    [this, conn](const QString& file) {
                        const QString calling =
                            AppConfig::instance().pcmFile(QStringLiteral("calling.pcm"));
                        if (file == calling) {
                            m_connAudioManager->playAsync(
                                AppConfig::instance().pcmFile(QStringLiteral("conn.pcm")));
                            QObject::disconnect(*conn);
                            delete conn;
                        }
                    });

    // 重建通话窗口，并与 widget_3 左上角对齐。
    if (m_callForm) {
        m_callForm->close();
        delete m_callForm;
        m_callForm = nullptr;
    }
    m_callForm = new Form_CALL(this);
    m_callForm->move(ui->widget_3->mapToGlobal(QPoint(0, 0)));
    m_callForm->setStatusText(QStringLiteral("正在呼叫..."));
    m_callForm->show();
    m_callForm->raise();

    connect(m_callForm, &Form_CALL::hangupRequested, this, [this]() {
        if (m_audioManager) m_audioManager->stop();
        if (m_connAudioManager) m_connAudioManager->stop();
        hangUpCall();
    });

    if (!m_signaling->send(Signal::Call)) {
        QMessageBox::warning(this, QStringLiteral("错误"),
                             QStringLiteral("信令服务器未连接"));
        return;
    }

    qInfo() << "[完成] 信令已发送，等待服务器 accept";
    if (m_pe4Thread) {
        m_pe4Thread->pause(); // 通话期间暂停人体检测，避免误触发
    }
}

void Widget::on_btnadd_clicked()
{
    m_audioManager->playAsync(
        AppConfig::instance().pcmFile(QStringLiteral("regnew.pcm")));

    if (!m_fingerForm) {
        m_fingerForm = new Form_finger(this);
        // 只连接 addFaceRequested —— 它是 Form_finger 中唯一的真信号。
        //
        // 这里原本还有一行
        //     connect(m_fingerForm, &Form_finger::addFingerRequested,
        //             this, &Widget::onAddFingerFromFingerForm);
        // 但 Form_finger::addFingerRequested() 声明在 `public slots:` 区：它自己
        // 就是 ui->addfinger 的槽，内部完成整套分步注册（轮询→采集→生成特征→
        // 合并→存储）。Qt5 的 PMF 版 connect 要求第二个实参是信号，而 moc 只为
        // `signals:` 区的成员生成信号索引，因此该 connect 在运行期必然失败
        // （打印 "signal not found in Form_finger"）并返回 false，对应的
        // Widget::onAddFingerFromFingerForm()（进而 HAL 的 stepRegister()）从来
        // 没有被执行过。
        //
        // 指纹注册实际由 Form_finger 内部的分步流程完成，不需要再通知父窗口；
        // 保留那行连接只会得到一个“看起来配好了、实际从未生效”的假象，
        // 并可能诱导以后有人在两条注册路径上重复注册同一个手指（两者都会
        // 直接读写同一个串口）。故删去。
        connect(m_fingerForm, &Form_finger::addFaceRequested,
                this, &Widget::onAddFaceFromFingerForm);
    }
    m_fingerForm->move(ui->widget_3->mapToGlobal(QPoint(0, 0)));
    m_fingerForm->show();
    m_fingerForm->raise();
}

void Widget::on_btnpsswd_clicked()
{
    auto* passwdForm = new Form_passwd(this);
    passwdForm->setAttribute(Qt::WA_DeleteOnClose); // 关闭时自动销毁
    connect(passwdForm, &Form_passwd::passwordEntered,
            m_passwordManager, &PasswordManager::checkPassword);
    passwdForm->move(ui->widget_3->mapToGlobal(QPoint(0, 0)));
    passwdForm->show();
    passwdForm->raise();
    m_passwdForm = passwdForm;

    m_audioManager->playAsync(
        AppConfig::instance().pcmFile(QStringLiteral("inputpasswd.pcm")));
}

void Widget::on_btnpower_clicked()
{
    // 熄灭补光灯（保留原语义：重构前此处为 gpiod_line_set_value(line5, 0)）。
    if (m_fillLight) {
        m_fillLight->setValue(0);
    }
}

// ============================================================================
// 人脸回调
// ============================================================================

// 这里原有两个不可达的槽：onFingerprintIdentify / onFingerprintEnroll。
// verify.py 的 [dead-slot] 规则连续告警了十几轮：它们既没有调用者、也不是
// 任何 connect 的目标；现场真正执行的是 setupModules() 里的
// FingerprintManager::identifyResult lambda（识别）与 Form_finger 的内联
// 状态机（注册）。第 40 轮做 P1 时把它们**删除**，理由：
//   1. 死掉的平行实现最容易被后来者"顺手修一下"，而改动它不影响现场行为 ——
//      这正是规则原文警告的陷阱（"为它写的单元测试覆盖的是平行实现"）；
//   2. 指纹识别成功要开锁，开锁动作必须只有一处（见 requestUnlock）。
// 注：FingerprintManager::startEnroll() 同样零生产调用者，但它是**公开 API**
// 且有单元测试，删不删需要拍板，因此保留并在交接文档里记为待裁决事项。

void Widget::onAddFaceFromFingerForm()
{
    if (m_faces.empty()) {
        return;
    }
    const auto maxFace = std::max_element(
        m_faces.begin(), m_faces.end(),
        [](const cv::Rect& a, const cv::Rect& b) { return a.area() < b.area(); });
    if (maxFace == m_faces.end()) {
        return;
    }
    const QImage frame = m_camera->getFrame();
    if (frame.isNull()) {
        return;
    }
    const QRect faceRect(maxFace->x, maxFace->y, maxFace->width, maxFace->height);
    const QImage faceImage = frame.copy(faceRect).scaled(100, 100, Qt::KeepAspectRatio);
    m_faceManager->addFaceSample(faceImage);
    // 用 FaceManager 自己的门槛判断，而不是界面侧的副本常量。
    if (m_faceManager->sampleCount() == m_faceManager->sampleTarget()) {
        m_faceManager->train();
    }
}

// ============================================================================
// 画面刷新、人脸检测与识别
// ============================================================================

void Widget::updateFrame()
{
    static int detectCounter = 0;
    static QElapsedTimer recognizeCooldown;

    const AppConfig& cfg = AppConfig::instance();

    QImage frame = m_camera->getFrame();
    if (frame.isNull()) {
        return;
    }

    // 在画面上叠加人脸框与采集计数。
    {
        QPainter painter(&frame);
        painter.setPen(QPen(Qt::red, 3));
        QFont font = painter.font();
        font.setPointSize(18);
        painter.setFont(font);
        for (const auto& face : m_faces) {
            painter.drawRect(face.x, face.y, face.width, face.height);
            const QString text = QString::number(m_collectCount);
            painter.setPen(QPen(Qt::black, 5)); // 黑色描边提升可读性
            painter.drawText(face.x + 6, face.y + 28, text);
            painter.setPen(QPen(Qt::white, 1));
            painter.drawText(face.x + 6, face.y + 28, text);
            painter.setPen(QPen(Qt::red, 3));
        }
        painter.end();
    }

    // 按固定尺寸裁剪显示，保持画面比例。
    const QSize labelSize(kPreviewWidth, kPreviewHeight);
    const QImage scaled = frame.scaled(labelSize, Qt::KeepAspectRatioByExpanding,
                                       Qt::FastTransformation);
    const QRect cropRect((scaled.width() - labelSize.width()) / 2,
                         (scaled.height() - labelSize.height()) / 2,
                         labelSize.width(), labelSize.height());
    ui->labelcam->setPixmap(QPixmap::fromImage(scaled.copy(cropRect)));

    // ---- 异步人脸检测：每 N 帧一次，缩小尺寸降低计算量 ----
    if (++detectCounter % cfg.faceDetectInterval() == 0 && !m_faceCascade.empty()) {
        // 用 QPointer 观察 this：任务在后台线程运行时，Widget 可能已被析构
        // （例如用户关机或界面销毁）。原来的实现直接捕获裸 this，既会在
        // 已析构对象上读 m_faceCascade，又会把结果 invokeMethod 到一个
        // 已不存在的对象——两者都是 use-after-free。
        // QPointer 在对象销毁后自动变空，是最轻量的守卫。
        //
        // m_faceCascade 是生成检测器前就要用到的成员，因此在提交任务时就
        // 把分类器按值拷贝进 lambda，避免任务体内再触碰 this 的成员。
        QPointer<Widget> self(this);
        // 必须是**非 const** 拷贝：cv::CascadeClassifier::detectMultiScale()
        // 不是 const 成员函数（内部有可变的缓存/状态），用 const 对象调用会
        // 编不过：
        //   error: passing 'const cv::CascadeClassifier' as 'this' argument
        //          discards qualifiers [-fpermissive]
        // 按值拷贝进 lambda 的初衷（避免任务体内再触碰 this 的成员）不变。
        cv::CascadeClassifier cascade = m_faceCascade;
        const QImage small = frame.scaled(kDetectWidth, kDetectHeight, Qt::KeepAspectRatio);
        // 注意 lambda 必须带 mutable：lambda 的 operator() 默认是 const，
        // 按值捕获的 cascade 在体内就成了 const 对象，而
        // cv::CascadeClassifier::detectMultiScale() 不是 const 成员，
        // 于是即使外面不加 const 仍然编不过（同上一条错误）。
        QtConcurrent::run([self, small, cascade]() mutable {
            if (self.isNull()) {
                return; // Widget 已析构，直接放弃本次检测
            }
            const cv::Mat mat(small.height(), small.width(), CV_8UC4,
                              const_cast<uchar*>(small.bits()), small.bytesPerLine());
            cv::Mat gray;
            cv::cvtColor(mat, gray, cv::COLOR_BGRA2GRAY);
            std::vector<cv::Rect> rects;
            cascade.detectMultiScale(gray, rects, 1.1, 3, 0, cv::Size(30, 30));
            // 结果投递回 UI 线程处理（跨线程直接访问 m_faces 不安全）。
            QVector<cv::Rect> qrects;
            qrects.reserve(static_cast<int>(rects.size()));
            for (const cv::Rect& r : rects) {
                qrects.append(r);
            }
            const int w = small.width();
            const int h = small.height();
            // 二次检查：从提交到现在又过了一段时间，对象可能刚被销毁。
            // 定时器挂在 ctx 上，ctx 析构时定时器一并销毁，闭包不会执行；
            // 这里显式判断是为了避免无谓地构造闭包。
            // 用 self.data() 显式取裸指针，避免依赖 QPointer 的隐式转换。
            if (self.isNull()) {
                return;
            }
            Widget* ctx = self.data();
            // 为什么不是 QMetaObject::invokeMethod(ctx, lambda, Qt::QueuedConnection)：
            // 那个 **functor 重载是 Qt 5.10 才引入的**，而目标 SDK（Tina Linux）
            // 常见 Qt 5.9~5.12，写 5.10+ 的 API 会让交叉编译直接失败。
            // QTimer::singleShot(msec, context, functor) 这个上下文重载自
            // Qt 5.4 起就有，语义等价：0 毫秒后在本对象所属线程执行，且
            // ctx 先析构时该调用被取消（不会投递到已析构对象上）。
            // tools/verify.py 的 [qt5] 规则守着这条边界。
            QTimer::singleShot(0, ctx, [self, qrects, w, h]() {
                if (!self.isNull()) {
                    self->onFaceDetectResult(qrects, w, h);
                }
            });
        });
    }

    // ---- 人脸识别：仅在检测到人时进行，且受冷却时间限制 ----
    if (!m_hasPerson) {
        return; // 无人时不识别也不播报
    }
    const bool cooldownElapsed =
        !recognizeCooldown.isValid()
        || recognizeCooldown.elapsed() > cfg.faceRecognizeIntervalMs();
    if (cooldownElapsed) {
        if (!m_faces.empty()) {
            const auto maxFace = std::max_element(
                m_faces.begin(), m_faces.end(),
                [](const cv::Rect& a, const cv::Rect& b) { return a.area() < b.area(); });
            if (maxFace != m_faces.end()) {
                const QRect faceRect(maxFace->x, maxFace->y, maxFace->width, maxFace->height);
                const QImage faceImage =
                    frame.copy(faceRect).scaled(100, 100, Qt::KeepAspectRatio);
                m_faceManager->recognize(faceImage);
                recognizeCooldown.restart();
            }
        } else {
            // 有人但未检出人脸：提示调整位置并打开补光。
            m_audioManager->playAsync(cfg.pcmFile(QStringLiteral("oncam.pcm")));
            recognizeCooldown.restart();
            if (m_fillLight) m_fillLight->setValue(1);
        }
    }
}

void Widget::onFaceDetectResult(const QVector<cv::Rect>& detected, int imgW, int imgH)
{
    const QImage frame = m_camera->getFrame();
    if (frame.isNull() || imgW <= 0 || imgH <= 0) {
        return;
    }
    // 检测在缩小图上进行，需把坐标还原到原始分辨率。
    const double scaleX = static_cast<double>(frame.width()) / imgW;
    const double scaleY = static_cast<double>(frame.height()) / imgH;
    m_faces.clear();
    m_faces.reserve(detected.size());
    for (const auto& face : detected) {
        m_faces.append(cv::Rect(static_cast<int>(face.x * scaleX),
                                static_cast<int>(face.y * scaleY),
                                static_cast<int>(face.width * scaleX),
                                static_cast<int>(face.height * scaleY)));
    }
}

void Widget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
}
