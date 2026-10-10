// 引入项目内部头文件 "widget.h"。
#include "widget.h"
// 引入项目内部头文件 "ui_widget.h"。
#include "ui_widget.h"
#include "appconfig.h"
// 引入系统或第三方头文件 <QDebug>。
#include <QDebug>
// 引入系统或第三方头文件 <QMessageBox>。
#include <QMessageBox>
// 引入系统或第三方头文件 <atomic>。
#include <atomic>
// 引入系统或第三方头文件 <thread>。
#include <thread>
// 引入系统或第三方头文件 <chrono>。
#include <chrono>
// 引入系统或第三方头文件 <cstring>。
#include <cstring>
// 引入系统或第三方头文件 <arpa/inet.h>。
#include <arpa/inet.h>
// 引入项目内部头文件 "audiosenderthread.h"。
#include "audiosenderthread.h"
// 引入项目内部头文件 "audioreceiverthread.h"。
#include "audioreceiverthread.h"
// 引入系统或第三方头文件 <QTimer>。
#include <QTimer>
// 引入系统或第三方头文件 <QImage>。
#include <QImage>
// 引入系统或第三方头文件 <QPixmap>。
#include <QPixmap>
// 引入系统或第三方头文件 <QFile>。
#include <QFile>
// 引入系统或第三方头文件 <pthread.h>。
#include <pthread.h>
// 引入系统或第三方头文件 <QDateTime>。
#include <QDateTime>
// 引入系统或第三方头文件 <QPainter>。
#include <QPainter>
// 引入系统或第三方头文件 <QPainterPath>。
#include <QPainterPath>
// 引入系统或第三方头文件 <QElapsedTimer>。
#include <QElapsedTimer>
// 引入系统或第三方头文件 <opencv2/opencv.hpp>。
#include <opencv2/opencv.hpp>
// 引入系统或第三方头文件 <opencv2/objdetect.hpp>。
#include <opencv2/objdetect.hpp>
// 引入系统或第三方头文件 <opencv2/face.hpp>。
#include <opencv2/face.hpp>
// 引入系统或第三方头文件 <QBuffer>。
#include <QBuffer>
// 引入系统或第三方头文件 <QImageReader>。
#include <QImageReader>
// 引入系统或第三方头文件 <QPainter>。
#include <QPainter>
// 引入系统或第三方头文件 <QElapsedTimer>。
#include <QElapsedTimer>
// 引入系统或第三方头文件 <QResizeEvent>。
#include <QResizeEvent>
// 引入系统或第三方头文件 <QUdpSocket>。
#include <QUdpSocket>
// 引入系统或第三方头文件 <QHostAddress>。
#include <QHostAddress>
// 引入系统或第三方头文件 <QTcpSocket>。
#include <QTcpSocket>
// 引入系统或第三方头文件 <unistd.h>。
#include <unistd.h>
// 引入系统或第三方头文件 <QtConcurrent/QtConcurrent>。
#include <QtConcurrent/QtConcurrent>
// 引入项目内部头文件 "HailinFingerprint.h"。
#include "HailinFingerprint.h"
// 引入项目内部头文件 "form_passwd.h"。
#include "form_passwd.h"
// 引入系统或第三方头文件 <QMutex>。
#include <QMutex>
// 引入系统或第三方头文件 <memory>。
#include <memory>
// 引入项目内部头文件 "camera/v4l2camera.h"。
#include "camera/v4l2camera.h"
// 引入项目内部头文件 "camera/camera_udp_server.h"。
#include "camera/camera_udp_server.h"

// 引入系统或第三方头文件 <QInputMethod>。
#include <QInputMethod>


// 继续当前代码块的参数、表达式或结构定义。
extern "C" {
     // 执行本行语句完成对应变量、对象或业务状态处理。
     unsigned char* g_last_jpeg = nullptr;
     // 执行本行语句完成对应变量、对象或业务状态处理。
     size_t g_last_jpeg_size = 0;
     // 执行本行语句完成对应变量、对象或业务状态处理。
     pthread_mutex_t g_jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;
// 结束当前代码块作用域。
}

// ========== UI按键相关实现 ==========
// Widget::Widget, on_btncall_clicked, on_btnadd_clicked, on_btnpsswd_clicked, 构造/析构等
// 定义类 Widget 的成员函数 Widget。
Widget::Widget(QWidget *parent)
    // 继续当前代码块的参数、表达式或结构定义。
    : QWidget(parent)
    // 继续当前代码块的参数、表达式或结构定义。
    , ui(new Ui::Widget)
// 进入新的代码块作用域。
{
    // 删除fillLightAutoOffTimer和fillLightIsOn相关代码
    // 访问或更新 Qt 界面控件。
    ui->setupUi(this);
    // 继续当前代码块的参数、表达式或结构定义。
    QList<QPushButton*> btns = {
        // 访问或更新 Qt 界面控件。
        ui->btnadd, ui->btncall, ui->btnface, ui->btnpower,ui->btnpsswd,ui->btnfinger_step/* ... */
    // 结束当前代码块作用域。
    };
    // 开始循环处理满足条件的数据或状态。
    for (QPushButton* btn : btns) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        btn->setFocusPolicy(Qt::NoFocus);
    // 结束当前代码块作用域。
    }
    // 添加定时器刷新日期和时间
    // 创建或使用 Qt 定时器对象。
    QTimer *dateTimeTimer = new QTimer(this);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(dateTimeTimer, &QTimer::timeout, this, [this]() {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        QDateTime current = QDateTime::currentDateTime();
        // 访问或更新 Qt 界面控件。
        ui->labelDate->setText(current.date().toString("yyyy-MM-dd"));
        // 访问或更新 Qt 界面控件。
        ui->labelTime->setText(current.time().toString("HH:mm:ss"));
    // 结束当前代码块作用域。
    });
    // 继续当前代码块的参数、表达式或结构定义。
    dateTimeTimer->start(1000); // 每秒刷新一次

    // 初始化显示
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QDateTime current = QDateTime::currentDateTime();
    // 访问或更新 Qt 界面控件。
    ui->labelDate->setText(current.date().toString("yyyy-MM-dd"));
    // 访问或更新 Qt 界面控件。
    ui->labelTime->setText(current.time().toString("HH:mm:ss"));
    // 输出调试日志。
    qDebug() << "[界面] 时间显示初始化完成";

    // 隐藏鼠标指针
    // 执行本行语句完成对应变量、对象或业务状态处理。
    this->setCursor(Qt::BlankCursor);

    // 设置labelcam和父控件为透明，并设置四角圆角样式表
    // 访问或更新 Qt 界面控件。
    ui->labelcam->setAttribute(Qt::WA_TranslucentBackground, true);
    // 根据条件判断是否执行该分支。
    if (ui->labelcam->parentWidget())
        // 访问或更新 Qt 界面控件。
        ui->labelcam->parentWidget()->setAttribute(Qt::WA_TranslucentBackground, true);
    // 访问或更新 Qt 界面控件。
    ui->labelcam->setStyleSheet(
        // 继续当前代码块的参数、表达式或结构定义。
        "background-color: rgb(85, 87, 83);"
        // 继续当前代码块的参数、表达式或结构定义。
        "border: none;"
    // 执行本行语句完成对应变量、对象或业务状态处理。
    );
  /***********************************UIINIT****************************************************/


    // 声明或创建 C 结构体变量。
    struct gpiod_chip *chip = gpiod_chip_open_by_name("gpiochip0");
    // 继续当前代码块的参数、表达式或结构定义。
    line5 = gpiod_chip_get_line(chip, 133); // PE5
    // 调用 libgpiod 接口访问 GPIO。
    gpiod_line_request_output(line5, "my_app", 0);

    //指纹功能
    // 执行本行语句完成对应变量、对象或业务状态处理。
    fingerprint = new HailinFingerprint("/dev/ttyS4", this);
    // 根据条件判断是否执行该分支。
    if (fingerprint->open()) {
        // 输出调试日志。
        qDebug() << "[指纹] 模块串口打开成功";
    // 结束当前代码块作用域。
    } else {
       // qDebug() << "[指纹] 模块串口打开失败";
    // 结束当前代码块作用域。
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    fingerprintManager = new FingerprintManager(this);
    // 继续当前代码块的参数、表达式或结构定义。
    fingerprintManager->setDevice(fingerprint); // fingerprint 为 HailinFingerprint*
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(fingerprintManager, &FingerprintManager::identifyResult,
            // 继续当前代码块的参数、表达式或结构定义。
            this, [this](bool success, int id, int score, const QString& msg){
        // 根据条件判断是否执行该分支。
        if (msg == "请按下指纹进行识别") {
            // 不再自动播报采集提示音
            // 返回当前函数的执行结果或结束函数。
            return;
        // 结束当前代码块作用域。
        }
        // 根据条件判断是否执行该分支。
        if (success) {
            // 输出调试日志。
            qDebug() << "指纹识别成功";
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioManager->playAsync("/tmp/pcm/fingerdoor.pcm");
        // 结束当前代码块作用域。
        } else {
            // 输出调试日志。
            qDebug() << "指纹识别失败";
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioManager->playAsync("/tmp/pcm/fingererr.pcm");
        // 结束当前代码块作用域。
        }
        // 识别完成后，延时再次启动识别，实现循环
        // 定义类 QTimer 的成员函数 singleShot。
        QTimer::singleShot(500, this, [this](){
            // 执行本行语句完成对应变量、对象或业务状态处理。
            fingerprintManager->startIdentify();
        // 结束当前代码块作用域。
        });
    // 结束当前代码块作用域。
    });
    // 移除按钮触发
    // connect(ui->btnfinger_step, &QPushButton::clicked, fingerprintManager, &FingerprintManager::startIdentify);
    // 程序启动后自动开始指纹识别
    // 定义类 QTimer 的成员函数 singleShot。
    QTimer::singleShot(0, this, [this](){
        // 执行本行语句完成对应变量、对象或业务状态处理。
        fingerprintManager->startIdentify();
    // 结束当前代码块作用域。
    });

    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(fingerprintManager, &FingerprintManager::enrollResult, this, [this](bool success, int id, const QString& msg){
    // 根据条件判断是否执行该分支。
    if (msg.contains("请按下手指进行")) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioManager->playAsync("/tmp/pcm/caijifinger.pcm");
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if (msg.contains("采集成功")) {
        // 可选：每次采集成功提示音
        // 继续当前代码块的参数、表达式或结构定义。
        audioManager->playAsync("/tmp/pcm/fingerone.pcm"); // 可根据第几次切换音频
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if (success && msg == "注册成功") {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioManager->playAsync("/tmp/pcm/opendoor.pcm");
        // nextId++;
    // 结束当前代码块作用域。
    } else if (!success) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioManager->playAsync("/tmp/pcm/fingererr.pcm");
    // 结束当前代码块作用域。
    }
    // 可在UI显示msg
    // 结束当前代码块作用域。
    });
    //指纹功能结束


//音频功能
    // 执行本行语句完成对应变量、对象或业务状态处理。
    audioManager = new AudioManager(this);
    // 继续当前代码块的参数、表达式或结构定义。
    connAudioManager = new AudioManager(this); // 初始化专用 conn.pcm 播放器
//人脸功能
   // 执行本行语句完成对应变量、对象或业务状态处理。
   faceManager = new FaceManager(this);


    // 执行本行语句完成对应变量、对象或业务状态处理。
    qputenv("QT_IM_MODULE", QByteArray("qtvirtualkeyboard"));
    // 输出调试日志。
    qDebug() << "\n========== [系统启动] 智能门禁系统初始化 ==========";
   // playPcmFileAsync("/tmp/pcm/welcome.pcm");
    // 加载人脸分类器
    // 执行本行语句完成对应变量、对象或业务状态处理。
    bool loaded = loadFaceCascade("/root/haarcascade_frontalface_default.xml");
    // 输出调试日志。
    qDebug() << "[人脸检测] 分类器加载" << (loaded ? "成功" : "失败");

    // 初始化LBPH人脸识别器
    // 执行本行语句完成对应变量、对象或业务状态处理。
    faceRecognizer = cv::face::LBPHFaceRecognizer::create();
    // 输出调试日志。
    qDebug() << "[人脸识别] LBPH识别器初始化完成";

    // 去除所有QPushButton的焦点虚框
    // 执行本行语句完成对应变量、对象或业务状态处理。
    this->setStyleSheet("QPushButton:focus { outline: none; }");



    // 打开本地V4L2摄像头
    // 执行本行语句完成对应变量、对象或业务状态处理。
    camera = new V4L2Camera("/dev/video0");
    // 根据条件判断是否执行该分支。
    if (!camera->open()) {
        // 输出调试日志。
        qDebug() << "[错误] 无法打开摄像头设备";
        // 定义类 QMessageBox 的成员函数 critical。
        QMessageBox::critical(this, "错误", "无法打开摄像头");
    // 结束当前代码块作用域。
    } else {
        // 输出调试日志。
        qDebug() << "[摄像头] V4L2设备打开成功";
    // 结束当前代码块作用域。
    }
    // 启动UDP摄像头服务器
    // 执行本行语句完成对应变量、对象或业务状态处理。
    cameraUdpServer = new CameraUdpServer(camera, 50000, this);

    // 定时器定时采集+检测+显示
    // 执行本行语句完成对应变量、对象或业务状态处理。
    timer = new QTimer(this);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(timer, &QTimer::timeout, this, &Widget::updateFrame);
    // 继续当前代码块的参数、表达式或结构定义。
    timer->start(50); // 20fps，配合跳帧优化实际显示约10fps，更流畅
    // 输出调试日志。
    qDebug() << "[视频显示] 定时器启动，采集帧率: 20fps，显示帧率: 10fps";



    // 初始化TCP信令socket
    // 执行本行语句完成对应变量、对象或业务状态处理。
    tcpSignalSocket = new QTcpSocket(this);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(tcpSignalSocket, &QTcpSocket::readyRead, this, &Widget::onTcpReadyRead);
    connect(tcpSignalSocket, &QTcpSocket::connected, this, [this]() {
        qDebug() << "[信令] TCP已连接:" << tcpSignalSocket->peerAddress().toString()
                 << tcpSignalSocket->peerPort();
    });
    connect(tcpSignalSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this,
            [this](QAbstractSocket::SocketError) {
        qWarning() << "[信令] TCP连接失败:" << tcpSignalSocket->errorString()
                   << "目标:" << smartproServerIp() << 8887;
    });
    // 断线自动重连
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(tcpSignalSocket, &QTcpSocket::disconnected, this, [this]() {
        // 输出调试日志。
        qDebug() << "[信令] TCP连接断开，3秒后自动重连...";
        // 定义类 QTimer 的成员函数 singleShot。
        QTimer::singleShot(3000, this, [this]() {
            // 输出调试日志。
            qDebug() << "[信令] 正在尝试重连...";
            // 执行本行语句完成对应变量、对象或业务状态处理。
            tcpSignalSocket->connectToHost(smartproServerIp(), 8887);
        // 结束当前代码块作用域。
        });
    // 结束当前代码块作用域。
    });
    // 程序启动时即连接
    // 执行本行语句完成对应变量、对象或业务状态处理。
    qDebug() << "[网络配置] T113地址:" << smartproSelfIp()
             << "服务器地址:" << smartproServerIp();
    tcpSignalSocket->connectToHost(smartproServerIp(), 8887);

    // 移除音频UDP socket的创建和bind（延后到accept后）

    // 输出调试日志。
    qDebug() << "[完成] 系统初始化完成，等待用户操作...";
    // 输出调试日志。
    qDebug() << "===============================================\n";

    // 执行本行语句完成对应变量、对象或业务状态处理。
    pe4Thread = new GpioPe4MonitorThread(this);
    // 修改GpioPe4MonitorThread，增加pe4Low信号
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(pe4Thread, &GpioPe4MonitorThread::pe4Low, this, [this](){
        // 执行本行语句完成对应变量、对象或业务状态处理。
        hasPerson = false;
    // 结束当前代码块作用域。
    });
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(pe4Thread, &GpioPe4MonitorThread::pe4High, this, [this](){
        // 执行本行语句完成对应变量、对象或业务状态处理。
        hasPerson = true;
    // 结束当前代码块作用域。
    });
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pe4Thread->start();

    // 指纹功能初始化与信号连接
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(fingerprint, &HailinFingerprint::identifyResult, this, &Widget::onFingerprintIdentify);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(fingerprint, &HailinFingerprint::enrollResult, this, &Widget::onFingerprintEnroll);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(fingerprint, &HailinFingerprint::debugInfo, this, &Widget::onFingerprintDebug);

    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(ui->btnpsswd, &QPushButton::clicked, this, &Widget::on_btnpsswd_clicked);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(fingerprint, &HailinFingerprint::stepRegisterStatus, this, &Widget::onStepRegisterStatus);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(fingerprint, &HailinFingerprint::stepIdentifyStatus, this, &Widget::onStepIdentifyStatus);
    // 连接音频线程断开信号
    // 根据条件判断是否执行该分支。
    if(audioThread) connect(audioThread, &AudioSenderThread::connectionLost, this, &Widget::hangupCall);
    // 根据条件判断是否执行该分支。
    if(audioRecvThread) connect(audioRecvThread, &AudioReceiverThread::connectionLost, this, &Widget::hangupCall);

    // Widget 构造函数中添加 faceManager 信号连接
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(faceManager, &FaceManager::faceSampled, this, [this](int current, int total){
        // 根据条件判断是否执行该分支。
        if (current == 1) {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioManager->playAsync("/tmp/pcm/faceone.pcm");
            // 调用 libgpiod 接口访问 GPIO。
            gpiod_line_set_value(line5, 1);
        // 结束当前代码块作用域。
        }
        // 在前一条件不满足时继续判断该分支。
        else if (current == 2) audioManager->playAsync("/tmp/pcm/facetwo.pcm");
        // 在前一条件不满足时继续判断该分支。
        else if (current == 3) {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioManager->playAsync("/tmp/pcm/facethree.pcm");
            // 调用 libgpiod 接口访问 GPIO。
            gpiod_line_set_value(line5, 0);
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    });
    // 在人脸注册采集样本时点亮补光灯
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(faceManager, &FaceManager::trainFinished, this, [this](bool success){
        // 根据条件判断是否执行该分支。
        if (success) {
            // 输出调试日志。
            qDebug() << "[人脸] 训练完成";
        // 结束当前代码块作用域。
        } else {
            // 输出调试日志。
            qDebug() << "[人脸] 训练失败，样本不足";
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    });
    // faceManager::recognizeResult信号槽只播放识别结果音频
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(faceManager, &FaceManager::recognizeResult, this, [this](bool success, double confidence){
        // 输出调试日志。
        qDebug() << "[人脸识别] 结果:" << (success ? "成功" : "失败") << ", confidence=" << confidence;
         // 调用 libgpiod 接口访问 GPIO。
         gpiod_line_set_value(line5, 0);
        // 根据条件判断是否执行该分支。
        if (success) {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioManager->playAsync("/tmp/pcm/opendoor.pcm");


        // 结束当前代码块作用域。
        } else {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioManager->playAsync("/tmp/pcm/faceerr.pcm");
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    });

    // Widget 构造函数中添加 passwordManager 初始化和信号连接
    // 执行本行语句完成对应变量、对象或业务状态处理。
    passwordManager = new PasswordManager(this);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(passwordManager, &PasswordManager::passwordChecked, this, [this](bool success){
        // 根据条件判断是否执行该分支。
        if (success) {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioManager->playAsync("/tmp/pcm/opendoor.pcm");
            // 关闭密码输入窗口
            // 根据条件判断是否执行该分支。
            if (passwdForm) {
                // 执行本行语句完成对应变量、对象或业务状态处理。
                passwdForm->close();
                // 执行本行语句完成对应变量、对象或业务状态处理。
                passwdForm = nullptr;
            // 结束当前代码块作用域。
            }
        // 结束当前代码块作用域。
        } else {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioManager->playAsync("/tmp/pcm/passwderr.pcm");
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    });
// 结束当前代码块作用域。
}

// 定义类 Widget 的成员函数 ~Widget。
Widget::~Widget()
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if(cameraPushThread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        cameraPushThread->stop();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        cameraPushThread->wait();
        // 释放动态创建的对象资源。
        delete cameraPushThread;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        cameraPushThread = nullptr;
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if(audioThread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioThread->stop();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioThread->wait();
        // 释放动态创建的对象资源。
        delete audioThread;
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if(audioRecvThread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioRecvThread->stop();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioRecvThread->wait();
        // 释放动态创建的对象资源。
        delete audioRecvThread;
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if (audioSocketFd >= 0) {
        // 关闭文件描述符或 socket。
        ::close(audioSocketFd);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioSocketFd = -1;
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if(timer) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        timer->stop();
        // 释放动态创建的对象资源。
        delete timer;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        timer = nullptr;
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if (pe4Thread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        pe4Thread->stop();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        pe4Thread->wait();
        // 释放动态创建的对象资源。
        delete pe4Thread;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        pe4Thread = nullptr;
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if (fingerDetectThread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        fingerDetectThread->stop();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        fingerDetectThread->wait();
        // 释放动态创建的对象资源。
        delete fingerDetectThread;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        fingerDetectThread = nullptr;
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if (fingerprint) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        fingerprint->close();
        // 释放动态创建的对象资源。
        delete fingerprint;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        fingerprint = nullptr;
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if (camera) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        camera->close();
        // 释放动态创建的对象资源。
        delete camera;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        camera = nullptr;
    // 结束当前代码块作用域。
    }
    // 释放动态创建的对象资源。
    delete ui;

// 结束当前代码块作用域。
}

// ========== 音频/网络/通话相关实现 ==========
// hangupCall, onServerAccept, onTcpReadyRead, AudioSenderThread/AudioReceiverThread相关
// 定义或声明函数 Widget::hangupCall。
void Widget::hangupCall()
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if (connAudioManager) connAudioManager->stop(); // 挂断时停止 conn.pcm 播放
    // 1. 立即关闭窗口
    // 根据条件判断是否执行该分支。
    if (callForm) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        callForm->stopCallTimer();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        callForm->close();
    // 结束当前代码块作用域。
    }
    // 2. 安全结束音视频线程
    // 输出调试日志。
    qDebug() << "[hangupCall] Stopping cameraPushThread...";
    // 根据条件判断是否执行该分支。
    if (cameraPushThread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        cameraPushThread->stop();
        // 根据条件判断是否执行该分支。
        if (!cameraPushThread->wait(3000)) {
            // 输出警告日志。
            qWarning() << "[hangupCall] cameraPushThread wait timeout!";
        // 结束当前代码块作用域。
        }
        // 释放动态创建的对象资源。
        delete cameraPushThread;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        cameraPushThread = nullptr;
        // 输出调试日志。
        qDebug() << "[hangupCall] cameraPushThread stopped.";
    // 结束当前代码块作用域。
    }
    // 输出调试日志。
    qDebug() << "[hangupCall] Stopping audioThread...";
    // 根据条件判断是否执行该分支。
    if (audioThread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioThread->stop();
        // 根据条件判断是否执行该分支。
        if (!audioThread->wait(3000)) {
            // 输出警告日志。
            qWarning() << "[hangupCall] audioThread wait timeout!";
        // 结束当前代码块作用域。
        }
        // 释放动态创建的对象资源。
        delete audioThread;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioThread = nullptr;
        // 输出调试日志。
        qDebug() << "[hangupCall] audioThread stopped.";
    // 结束当前代码块作用域。
    }
    // 输出调试日志。
    qDebug() << "[hangupCall] Stopping audioRecvThread...";
    // 根据条件判断是否执行该分支。
    if (audioRecvThread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioRecvThread->stop();
        // 根据条件判断是否执行该分支。
        if (!audioRecvThread->wait(3000)) {
            // 输出警告日志。
            qWarning() << "[hangupCall] audioRecvThread wait timeout!";
        // 结束当前代码块作用域。
        }
        // 释放动态创建的对象资源。
        delete audioRecvThread;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioRecvThread = nullptr;
        // 输出调试日志。
        qDebug() << "[hangupCall] audioRecvThread stopped.";
    // 结束当前代码块作用域。
    }
    // 3. 释放窗口对象
    // 根据条件判断是否执行该分支。
    if (callForm) {
        // 释放动态创建的对象资源。
        delete callForm;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        callForm = nullptr;
    // 结束当前代码块作用域。
    }
    // 4. 断开TCP信令（已移除，保持连接）
    // if (tcpSignalSocket && tcpSignalSocket->state() == QAbstractSocket::ConnectedState) {
    //     tcpSignalSocket->disconnectFromHost();
    // }
    // 5. 关闭音频socket
    // 根据条件判断是否执行该分支。
    if (audioSocketFd >= 0) {
        // 关闭文件描述符或 socket。
        ::close(audioSocketFd);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioSocketFd = -1;
        // 输出调试日志。
        qDebug() << "[hangupCall] audioSocketFd closed.";
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if (pe4Thread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        pe4Thread->resume();
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// 定义或声明函数 Widget::on_btncall_clicked。
void Widget::on_btncall_clicked()
// 进入新的代码块作用域。
{
    // 先播放呼叫音
    // 执行本行语句完成对应变量、对象或业务状态处理。
    audioManager->playAsync("/tmp/pcm/calling.pcm");
    // 串联播放等待音，conn.pcm 用独立 AudioManager
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QMetaObject::Connection* conn = new QMetaObject::Connection;
    *conn = connect(audioManager, &AudioManager::playbackFinished, this, [this, conn](const QString& file){
        // 根据条件判断是否执行该分支。
        if (file == "/tmp/pcm/calling.pcm") {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            connAudioManager->playAsync("/tmp/pcm/conn.pcm");
            // 定义类 QObject 的成员函数 disconnect。
            QObject::disconnect(*conn);
            // 释放动态创建的对象资源。
            delete conn;
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    });
    // 弹出form_call窗口，左上角与widget_3左上角对齐
    // 根据条件判断是否执行该分支。
    if (callForm) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        callForm->close();
        // 释放动态创建的对象资源。
        delete callForm;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        callForm = nullptr;
    // 结束当前代码块作用域。
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    callForm = new Form_CALL(this);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QPoint globalPos = ui->widget_3->mapToGlobal(QPoint(0, 0));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    callForm->move(globalPos);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    callForm->setStatusText("正在呼叫...");
    // 执行本行语句完成对应变量、对象或业务状态处理。
    callForm->show();
    // 执行本行语句完成对应变量、对象或业务状态处理。
    callForm->raise();
    // 连接挂断信号到hangupCall
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(callForm, &Form_CALL::hangupRequested, this, [this](){
        // 根据条件判断是否执行该分支。
        if (audioManager) audioManager->stop();
        // 根据条件判断是否执行该分支。
        if (connAudioManager) connAudioManager->stop();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        hangupCall();
    // 结束当前代码块作用域。
    });

    // 只负责发数据，不再负责连接
    // 根据条件判断是否执行该分支。
    if (tcpSignalSocket->state() == QAbstractSocket::ConnectedState) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        tcpSignalSocket->write("call\n");
        // 执行本行语句完成对应变量、对象或业务状态处理。
        tcpSignalSocket->flush();
    // 结束当前代码块作用域。
    } else {
        // 定义类 QMessageBox 的成员函数 warning。
        QMessageBox::warning(this, "错误", "信令服务器未连接");
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 3. 不再创建任何音视频线程，全部延后到accept后
    // 输出调试日志。
    qDebug() << "[完成] 信令已发送，等待服务器accept...";
    // 输出调试日志。
    qDebug() << "=============================================";
    // 根据条件判断是否执行该分支。
    if (pe4Thread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        pe4Thread->pause();
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// 服务端accept时调用
// 定义或声明函数 Widget::onServerAccept。
void Widget::onServerAccept()
// 进入新的代码块作用域。
{
    // 只打断 conn.pcm，不影响主 audioManager
    // 根据条件判断是否执行该分支。
    if (connAudioManager) connAudioManager->stop();
    // 输出调试日志。
    qDebug() << "[信令] 收到服务器accept，开始创建音视频线程...";
    // 1. 创建音频UDP socket并bind 12345
    // 根据条件判断是否执行该分支。
    if (audioSocketFd < 0) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioSocketFd = socket(AF_INET, SOCK_DGRAM, 0);
        // 根据条件判断是否执行该分支。
        if (audioSocketFd < 0) {
            // 输出调试日志。
            qDebug() << "[音频] 创建UDP socket失败, errno=" << errno;
        // 结束当前代码块作用域。
        } else {
            // 声明或创建 C 结构体变量。
            struct sockaddr_in local_addr;
            // 清空结构体或缓冲区内容。
            memset(&local_addr, 0, sizeof(local_addr));
            // 执行本行语句完成对应变量、对象或业务状态处理。
            local_addr.sin_family = AF_INET;
            // 执行本行语句完成对应变量、对象或业务状态处理。
            local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            // 执行本行语句完成对应变量、对象或业务状态处理。
            local_addr.sin_port = htons(12345);
            // 根据条件判断是否执行该分支。
            if (bind(audioSocketFd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
                // 输出调试日志。
                qDebug() << "[音频] 绑定端口12345失败, errno=" << errno;
                // 关闭文件描述符或 socket。
                ::close(audioSocketFd);
                // 执行本行语句完成对应变量、对象或业务状态处理。
                audioSocketFd = -1;
            // 结束当前代码块作用域。
            } else {
                // 输出调试日志。
                qDebug() << "[音频] 成功绑定端口12345, sockfd=" << audioSocketFd;
            // 结束当前代码块作用域。
            }
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    }
    // 根据条件判断是否执行该分支。
    if (audioSocketFd < 0) {
        // 输出调试日志。
        qDebug() << "[音频] 无法创建/绑定socket，音频线程不启动";
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 2. 创建并启动视频推流线程
    // 根据条件判断是否执行该分支。
    if (cameraPushThread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        cameraPushThread->stop();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        cameraPushThread->wait();
        // 释放动态创建的对象资源。
        delete cameraPushThread;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        cameraPushThread = nullptr;
    // 结束当前代码块作用域。
    }
    // 注释掉视频推流相关调试信息
    // 执行本行语句完成对应变量、对象或业务状态处理。
    cameraPushThread = new CameraPushThread(camera, smartproServerIp(), 23456, this);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    cameraPushThread->start();
    // 3. 创建并启动音频采集线程
    // 根据条件判断是否执行该分支。
    if (audioThread) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioThread->stop();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioThread->wait();
        // 释放动态创建的对象资源。
        delete audioThread;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioThread = nullptr;
    // 结束当前代码块作用域。
    }
    // 输出调试日志。
    qDebug() << "[音频] 创建AudioSenderThread...";
    // 执行本行语句完成对应变量、对象或业务状态处理。
    audioThread = new AudioSenderThread(audioSocketFd, smartproServerIp(), 12345, this);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(audioThread, &AudioSenderThread::errorMsg, this, [this](const QString &msg){
        // 输出调试日志。
        qDebug() << "[音频错误]" << msg;
        // 定义类 QMessageBox 的成员函数 critical。
        QMessageBox::critical(this, "错误", msg);
    // 结束当前代码块作用域。
    });
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(audioThread, &AudioSenderThread::finishedMsg, this, [this](){
        // 输出调试日志。
        qDebug() << "[音频完成] 音频采集线程已结束";
    // 结束当前代码块作用域。
    });
    // 执行本行语句完成对应变量、对象或业务状态处理。
    audioThread->start();
    // 输出调试日志。
    qDebug() << "[音频] AudioSenderThread已启动";


    // 4. 创建并启动音频播放线程（延迟启动，避免资源冲突）
    // 定义类 QTimer 的成员函数 singleShot。
    QTimer::singleShot(400, this, [this]() {
        // 根据条件判断是否执行该分支。
        if (audioRecvThread) {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioRecvThread->stop();
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioRecvThread->wait();
            // 释放动态创建的对象资源。
            delete audioRecvThread;
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioRecvThread = nullptr;
        // 结束当前代码块作用域。
        }
        // 输出调试日志。
        qDebug() << "[音频] 创建AudioReceiverThread...";
        // 执行本行语句完成对应变量、对象或业务状态处理。
        audioRecvThread = new AudioReceiverThread(audioSocketFd, this);
        // 继续当前代码块的参数、表达式或结构定义。
        audioRecvThread->setVolume(30); // 默认30%音量
        // 根据条件判断是否执行该分支。
        if (audioRecvThread) {
            // 输出调试日志。
            qDebug() << "[音频] AudioReceiverThread 准备 start";
            // 执行本行语句完成对应变量、对象或业务状态处理。
            audioRecvThread->start();
            // 输出调试日志。
            qDebug() << "[音频] AudioReceiverThread 已调用 start";
            // 建立 Qt 信号与槽或 lambda 回调连接。
            connect(audioRecvThread, &AudioReceiverThread::errorMsg, this, [this](const QString &msg){
                // 输出警告日志。
                qWarning() << "[音频接收] 错误:" << msg;
            // 结束当前代码块作用域。
            });
            // 建立 Qt 信号与槽或 lambda 回调连接。
            connect(audioRecvThread, &AudioReceiverThread::finishedMsg, this, [this](){
                // 输出调试日志。
                qDebug() << "[音频接收] 线程已结束";
            // 结束当前代码块作用域。
            });
            // 建立 Qt 信号与槽或 lambda 回调连接。
            connect(audioRecvThread, &AudioReceiverThread::connectionLost, this, [this](){
                // 输出警告日志。
                qWarning() << "[音频接收] 连接丢失";
            // 结束当前代码块作用域。
            });
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    });
    // 5. 启动通话计时
    // 根据条件判断是否执行该分支。
    if (callForm) callForm->startCallTimer();
    // 输出调试日志。
    qDebug() << "[完成] 音频线程全部启动";
// 结束当前代码块作用域。
}

// 定义或声明函数 Widget::onTcpReadyRead。
void Widget::onTcpReadyRead()
// 进入新的代码块作用域。
{
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray datagram = tcpSignalSocket->readAll();
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QStringList messages = QString::fromUtf8(datagram).split('\n', QString::SkipEmptyParts);
    // 开始循环处理满足条件的数据或状态。
    for (const QString& serverMsgRaw : messages) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        QString serverMsg = serverMsgRaw.trimmed();
        // 输出调试日志。
        qDebug() << "get server: " << serverMsg;
        // 根据条件判断是否执行该分支。
        if (serverMsg == "accept") {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            onServerAccept();
        // 结束当前代码块作用域。
        }
        // 新增：处理recstop信令
        // 在前一条件不满足时继续判断该分支。
        else if (serverMsg == "recstop") {
            // 输出调试日志。
            qDebug() << "[信令] 收到recstop，自动挂断通话，当前callForm指针:" << callForm
                     // 继续当前代码块的参数、表达式或结构定义。
                     << ", cameraPushThread:" << cameraPushThread
                     // 继续当前代码块的参数、表达式或结构定义。
                     << ", audioThread:" << audioThread
                     // 继续当前代码块的参数、表达式或结构定义。
                     << ", audioRecvThread:" << audioRecvThread
                     // 执行本行语句完成对应变量、对象或业务状态处理。
                     << ", audioSocketFd:" << audioSocketFd;
            // 执行本行语句完成对应变量、对象或业务状态处理。
            hangupCall();
        // 结束当前代码块作用域。
        }
        // 新增：处理unlock信令
        // 在前一条件不满足时继续判断该分支。
        else if (serverMsg == "unlock") {
            // 输出调试日志。
            qDebug() << "[信令] 收到unlock，播放开门音";
            // 根据条件判断是否执行该分支。
            if (audioManager) audioManager->playAsync("/tmp/pcm/opendoor.pcm");
        // 结束当前代码块作用域。
        }
        // 可在此处理其它信令
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// ========== 指纹相关实现 ==========
// onFingerprintIdentify, onFingerprintEnroll, onFingerprintDebug, onStepRegisterStatus, onStepIdentifyStatus, onAddFingerFromFingerForm
// 定义或声明函数 Widget::onFingerprintIdentify。
void Widget::onFingerprintIdentify(bool success, int id, int score, const QString& msg)
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if (success) {
       // qDebug() << "[指纹] 识别成功, ID=" << id << ", score=" << score << ", msg=" << msg;
        // 定义类 QMessageBox 的成员函数 information。
        QMessageBox::information(this, "指纹识别", QString("识别成功，ID=%1，score=%2").arg(id).arg(score));
    // 结束当前代码块作用域。
    } else {
        // 输出调试日志。
        qDebug() << "[指纹] 识别失败, msg=" << msg;
        // 可选：识别失败时不弹窗，避免频繁打扰
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// 定义或声明函数 Widget::onFingerprintEnroll。
void Widget::onFingerprintEnroll(bool success, int id, const QString& msg)
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if (success) {
       // qDebug() << "[指纹] 注册成功, ID=" << id << ", msg=" << msg;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        nextFingerId++;
        // 定义类 QMessageBox 的成员函数 information。
        QMessageBox::information(this, "指纹注册", QString("注册成功，ID=%1").arg(id));
    // 结束当前代码块作用域。
    } else {
        // 输出调试日志。
        qDebug() << "[指纹] 注册失败, ID=" << id << ", msg=" << msg;
        // 定义类 QMessageBox 的成员函数 warning。
        QMessageBox::warning(this, "指纹注册", QString("注册失败，ID=%1\n原因：%2").arg(id).arg(msg));
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// 定义或声明函数 Widget::onFingerprintDebug。
void Widget::onFingerprintDebug(const QString& info)
// 进入新的代码块作用域。
{
  //  qDebug() << "[指纹调试]" << info;
// 结束当前代码块作用域。
}

// 定义或声明函数 Widget::on_btnpsswd_clicked。
void Widget::on_btnpsswd_clicked()
// 进入新的代码块作用域。
{
    // 执行本行语句完成对应变量、对象或业务状态处理。
    Form_passwd* passwdForm = new Form_passwd(this);
    // 继续当前代码块的参数、表达式或结构定义。
    passwdForm->setAttribute(Qt::WA_DeleteOnClose); // 关闭时自动销毁
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(passwdForm, &Form_passwd::passwordEntered, passwordManager, &PasswordManager::checkPassword);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QPoint globalPos = ui->widget_3->mapToGlobal(QPoint(0, 0));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    passwdForm->move(globalPos);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    passwdForm->show();
    // 执行本行语句完成对应变量、对象或业务状态处理。
    passwdForm->raise();
    // 执行本行语句完成对应变量、对象或业务状态处理。
    audioManager->playAsync("/tmp/pcm/inputpasswd.pcm");
// 结束当前代码块作用域。
}
// 定义或声明函数 Widget::onStepRegisterStatus。
void Widget::onStepRegisterStatus(const QString& msg, int step, bool ok) {}
// 定义或声明函数 Widget::onStepIdentifyStatus。
void Widget::onStepIdentifyStatus(const QString& msg, bool ok, int id, int score) {}

// ========== 人脸相关实现 ==========
// onAddFaceFromFingerForm, updateFrame（含人脸检测/识别）
// 定义或声明函数 Widget::updateFrame。
void Widget::updateFrame()
// 进入新的代码块作用域。
{
    // 声明静态变量或静态常量，保持跨调用状态。
    static int detect_counter = 0;
    // 声明只读变量或常量参数。
    const int detect_interval = 10; // 每10帧检测一次
    // 创建或处理图像帧数据。
    QImage img = camera->getFrame();
    // 根据条件判断是否执行该分支。
    if (img.isNull()) {
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 摄像头画面始终显示
    // 创建绘图对象用于在图像或控件上绘制内容。
    QPainter painter(&img);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    painter.setPen(QPen(Qt::red, 3));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QFont font = painter.font();
    // 执行本行语句完成对应变量、对象或业务状态处理。
    font.setPointSize(18);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    painter.setFont(font);
    // 开始循环处理满足条件的数据或状态。
    for (const auto &face : faces) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        painter.drawRect(face.x, face.y, face.width, face.height);
        // 在左上角画采集次数
        // 执行本行语句完成对应变量、对象或业务状态处理。
        QString text = QString::number(collectCount);
        // 黑色描边
        // 执行本行语句完成对应变量、对象或业务状态处理。
        painter.setPen(QPen(Qt::black, 5));
        // 执行本行语句完成对应变量、对象或业务状态处理。
        painter.drawText(face.x + 6, face.y + 28, text);
        // 白色字体
        // 执行本行语句完成对应变量、对象或业务状态处理。
        painter.setPen(QPen(Qt::white, 1));
        // 执行本行语句完成对应变量、对象或业务状态处理。
        painter.drawText(face.x + 6, face.y + 28, text);
        // 继续当前代码块的参数、表达式或结构定义。
        painter.setPen(QPen(Qt::red, 3)); // 恢复红色画框
    // 结束当前代码块作用域。
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    painter.end();
    // 圆角显示，先扩展再裁剪
    // 继续当前代码块的参数、表达式或结构定义。
    QSize labelSize(430, 475); // 固定为430x475
    // 创建或处理图像帧数据。
    QImage scaledImg = img.scaled(labelSize, Qt::KeepAspectRatioByExpanding, Qt::FastTransformation);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QRect cropRect((scaledImg.width() - labelSize.width())/2, (scaledImg.height() - labelSize.height())/2, labelSize.width(), labelSize.height());
    // 创建或处理用于界面显示的位图。
    QPixmap pix = QPixmap::fromImage(scaledImg.copy(cropRect));
    // 访问或更新 Qt 界面控件。
    ui->labelcam->setPixmap(pix);
    // 异步人脸检测
    // 执行本行语句完成对应变量、对象或业务状态处理。
    bool doDetect = (++detect_counter % detect_interval == 0);
    // 根据条件判断是否执行该分支。
    if (doDetect && !face_cascade.empty()) {
        // 创建或处理图像帧数据。
        QImage smallImg = img.scaled(320, 240, Qt::KeepAspectRatio);
        // 定义类 QtConcurrent 的成员函数 run。
        QtConcurrent::run([this, smallImg]() {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            cv::Mat mat_small(smallImg.height(), smallImg.width(), CV_8UC4, (void*)smallImg.bits(), smallImg.bytesPerLine());
            // 执行本行语句完成对应变量、对象或业务状态处理。
            cv::Mat mat_gray;
            // 定义类 cv 的成员函数 cvtColor。
            cv::cvtColor(mat_small, mat_gray, cv::COLOR_BGRA2GRAY);
            // 执行本行语句完成对应变量、对象或业务状态处理。
            std::vector<cv::Rect> faceRects;
            // 执行本行语句完成对应变量、对象或业务状态处理。
            face_cascade.detectMultiScale(mat_gray, faceRects, 1.1, 3, 0, cv::Size(30, 30));
            // 执行本行语句完成对应变量、对象或业务状态处理。
            QVector<cv::Rect> qfaces = QVector<cv::Rect>::fromStdVector(faceRects);
            // 发出 Qt 信号 QMetaObject 通知其他对象。
            emit QMetaObject::invokeMethod(this, [this, qfaces, w=smallImg.width(), h=smallImg.height()](){
                // 执行本行语句完成对应变量、对象或业务状态处理。
                onFaceDetectResult(qfaces, w, h);
            // 结束当前代码块作用域。
            }, Qt::QueuedConnection);
        // 结束当前代码块作用域。
        });
    // 结束当前代码块作用域。
    }
    // 声明静态变量或静态常量，保持跨调用状态。
    static QElapsedTimer faceRecognizeCooldown;
    // 声明静态变量或静态常量，保持跨调用状态。
    static int faceRecognizeIntervalMs = 2000;
    // 根据条件判断是否执行该分支。
    if (hasPerson) {
        // 根据条件判断是否执行该分支。
        if (!faces.empty()) {
            // 有人且有人脸，自动识别
            // 根据条件判断是否执行该分支。
            if (!faceRecognizeCooldown.isValid() || faceRecognizeCooldown.elapsed() > faceRecognizeIntervalMs) {
                // 输出调试日志。
                qDebug() << "[人脸识别] 开始识别";
                // 使用自动类型推导保存表达式结果。
                auto maxFace = std::max_element(faces.begin(), faces.end(),
                    // 执行本行语句完成对应变量、对象或业务状态处理。
                    [](const cv::Rect& a, const cv::Rect& b) { return a.area() < b.area(); });
                // 根据条件判断是否执行该分支。
                if (maxFace != faces.end()) {
                    // 执行本行语句完成对应变量、对象或业务状态处理。
                    QRect qface(maxFace->x, maxFace->y, maxFace->width, maxFace->height);
                    // 创建或处理图像帧数据。
                    QImage faceImg = img.copy(qface).scaled(100, 100, Qt::KeepAspectRatio);
                    // 执行本行语句完成对应变量、对象或业务状态处理。
                    faceManager->recognize(faceImg);
                    // 执行本行语句完成对应变量、对象或业务状态处理。
                    faceRecognizeCooldown.restart();
                // 结束当前代码块作用域。
                }
            // 结束当前代码块作用域。
            }
        // 结束当前代码块作用域。
        } else {
            // 有人但无脸，每2秒只播放oncam.pcm
            // 根据条件判断是否执行该分支。
            if (!faceRecognizeCooldown.isValid() || faceRecognizeCooldown.elapsed() > faceRecognizeIntervalMs) {
                // 执行本行语句完成对应变量、对象或业务状态处理。
                audioManager->playAsync("/tmp/pcm/oncam.pcm");
                // 执行本行语句完成对应变量、对象或业务状态处理。
                faceRecognizeCooldown.restart();
                // 调用 libgpiod 接口访问 GPIO。
                gpiod_line_set_value(line5, 1);
            // 结束当前代码块作用域。
            }
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    }
    // 无人时不识别也不播报
// 结束当前代码块作用域。
}

// 定义或声明函数 Widget::onFaceDetectResult。
void Widget::onFaceDetectResult(const QVector<cv::Rect>& detected, int imgW, int imgH)
// 进入新的代码块作用域。
{
    // 将检测到的人脸坐标缩放回原图尺寸
    // 创建或处理图像帧数据。
    QImage img = camera->getFrame();
    // 根据条件判断是否执行该分支。
    if (img.isNull()) return;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    double scaleX = double(img.width()) / imgW;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    double scaleY = double(img.height()) / imgH;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    faces.clear();
    // 开始循环处理满足条件的数据或状态。
    for (const auto& face : detected) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        cv::Rect f(face.x * scaleX, face.y * scaleY, face.width * scaleX, face.height * scaleY);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        faces.append(f);
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// 定义或声明函数 Widget::on_btnadd_clicked。
void Widget::on_btnadd_clicked()
// 进入新的代码块作用域。
{
    // 执行本行语句完成对应变量、对象或业务状态处理。
    audioManager->playAsync("/tmp/pcm/regnew.pcm");
    // 根据条件判断是否执行该分支。
    if (!fingerForm) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        fingerForm = new Form_finger(this);
        // 建立 Qt 信号与槽或 lambda 回调连接。
        connect(fingerForm, &Form_finger::addFaceRequested, this, &Widget::onAddFaceFromFingerForm);
        // 建立 Qt 信号与槽或 lambda 回调连接。
        connect(fingerForm, &Form_finger::addFingerRequested, this, &Widget::onAddFingerFromFingerForm);
    // 结束当前代码块作用域。
    }
    // 使form_finger左上角与widget_3左上角对齐
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QPoint globalPos = ui->widget_3->mapToGlobal(QPoint(0, 0));
    // 执行本行语句完成对应变量、对象或业务状态处理。
    fingerForm->move(globalPos);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    fingerForm->show();
    // 执行本行语句完成对应变量、对象或业务状态处理。
    fingerForm->raise();
// 结束当前代码块作用域。
}

// 定义或声明函数 Widget::onAddFaceFromFingerForm。
void Widget::onAddFaceFromFingerForm()
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if (faces.empty()) return;
    // 使用自动类型推导保存表达式结果。
    auto maxFace = std::max_element(faces.begin(), faces.end(),
        // 执行本行语句完成对应变量、对象或业务状态处理。
        [](const cv::Rect& a, const cv::Rect& b) { return a.area() < b.area(); });
    // 根据条件判断是否执行该分支。
    if (maxFace == faces.end()) return;
    // 创建或处理图像帧数据。
    QImage img = camera->getFrame();
    // 根据条件判断是否执行该分支。
    if (img.isNull()) return;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QRect qface(maxFace->x, maxFace->y, maxFace->width, maxFace->height);
    // 创建或处理图像帧数据。
    QImage faceImg = img.copy(qface).scaled(100, 100, Qt::KeepAspectRatio);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    faceManager->addFaceSample(faceImg);
    // 根据条件判断是否执行该分支。
    if (faceManager->sampleCount() == 3) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        faceManager->train();
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// 定义或声明函数 Widget::onAddFingerFromFingerForm。
void Widget::onAddFingerFromFingerForm()
// 进入新的代码块作用域。
{
    // 输出调试日志。
    qDebug() << "[分步注册] 开始分步注册, ID=" << nextFingerId;
    // 定义类 QMessageBox 的成员函数 information。
    QMessageBox::information(this, "分步注册", "请将手指按在指纹模块上，按提示操作，每次采集后抬手再按。");
    // 定义类 QtConcurrent 的成员函数 run。
    QtConcurrent::run(fingerprint, &HailinFingerprint::stepRegister, nextFingerId, 3); // 3次采集
// 结束当前代码块作用域。
}


// 定义或声明函数 Widget::resizeEvent。
void Widget::resizeEvent(QResizeEvent* event) {
    // 定义类 QWidget 的成员函数 resizeEvent。
    QWidget::resizeEvent(event);

// 结束当前代码块作用域。
}

// 定义或声明函数 Widget::loadFaceCascade。
bool Widget::loadFaceCascade(const QString& path)
// 进入新的代码块作用域。
{
    // 返回当前函数的执行结果或结束函数。
    return face_cascade.load(path.toStdString());
// 结束当前代码块作用域。
}



// 定义或声明函数 Widget::on_btnpower_clicked。
void Widget::on_btnpower_clicked()
// 进入新的代码块作用域。
{
    // 调用 libgpiod 接口访问 GPIO。
    gpiod_line_set_value(line5, 0);
// 结束当前代码块作用域。
}
