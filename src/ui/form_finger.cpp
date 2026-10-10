// 引入项目内部头文件 "form_finger.h"。
#include "ui/form_finger.h"
// 引入项目内部头文件 "ui_form_finger.h"。
#include "ui_form_finger.h"
// 引入系统或第三方头文件 <QTimer>。
#include <QTimer>
// 引入项目内部头文件 "widget.h"。
#include "config/app_config.h"
#include "ui/widget.h"

// Widget 暴露的模块指针需要完整类型才能调用其成员：
//   - audioManager() -> AudioManager
//   - fingerprint()  -> HailinFingerprint
//   - fingerprintManager() -> FingerprintManager（用于挂起/恢复常驻识别）
// 仅靠 widget.h 中的前置声明无法通过编译。
#include "hal/fingerprint_hailin.h"
#include "managers/audio_manager.h"
#include "managers/fingerprint_manager.h"

#include <QDebug>

// 定义类 Form_finger 的成员函数 Form_finger。
Form_finger::Form_finger(QWidget *parent) :
    // 继续当前代码块的参数、表达式或结构定义。
    QWidget(parent),
    // 继续当前代码块的参数、表达式或结构定义。
    ui(new Ui::Form_finger),
    // 继续当前代码块的参数、表达式或结构定义。
    currentStep(0),
    // 继续当前代码块的参数、表达式或结构定义。
    waitingFinger(false)
// 进入新的代码块作用域。
{
    // 访问或更新 Qt 界面控件。
    ui->setupUi(this);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(ui->addface, &QPushButton::clicked, this, &Form_finger::addFaceRequested);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(ui->addfinger, &QPushButton::clicked, this, &Form_finger::addFingerRequested);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(ui->addquit, &QPushButton::clicked, this, &Form_finger::onAddQuitClicked);

    // 继续当前代码块的参数、表达式或结构定义。
    QList<QPushButton*> btns = {
        // 访问或更新 Qt 界面控件。
        ui->addface, ui->addfinger, ui->addquit
    // 结束当前代码块作用域。
    };
    // 开始循环处理满足条件的数据或状态。
    for (QPushButton* btn : btns) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        btn->setFocusPolicy(Qt::NoFocus);
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// 定义或声明函数 Form_finger::onAddQuitClicked。
void Form_finger::onAddQuitClicked()
// 进入新的代码块作用域。
{
    // 退出时务必停掉轮询：窗口只是被 close() 隐藏（Form_finger 没有设
    // WA_DeleteOnClose，对象仍然活着），若不停定时器，它会继续每 500ms
    // 在 GUI 线程上做一次阻塞串口读，把界面拖死。
    stopFingerPoll();
    // 同时复位等待状态。注意这一步必须放在这里、而不是塞进 stopFingerPoll()：
    // startFingerRegisterStep() 里是「先 waitingFinger = true，再建定时器」，
    // 若 stopFingerPoll() 顺手把它清成 false，计时中的那一步就失去重复点击保护。
    waitingFinger = false;

    // 恢复常驻识别轮询。
    //
    // 注册期间我们把常驻识别挂起了（见 startFingerRegisterStep），否则两者会
    // 抢同一个串口的应答：识别轮询每 500ms 下发 CMD_GET_IMAGE 并抢读应答，
    // 会把注册流程正在等待的 0x01/0x02/0x05/0x06 应答取走，注册随机失败。
    // 这里必须成对恢复，且要覆盖“用户中途点退出”这一路径，否则识别会永久停止。
    if (m_suspendedFingerprintManager) {
        m_suspendedFingerprintManager->setSuspended(false);
        m_suspendedFingerprintManager = nullptr;
    }

    // 执行本行语句完成对应变量、对象或业务状态处理。
    this->close();
// 结束当前代码块作用域。
}

// 定义或声明函数 Form_finger::stopFingerPoll。
void Form_finger::stopFingerPoll()
// 进入新的代码块作用域。
{
    // 代数 +1：作废所有已排队、尚未触发的延时重启（见头文件里的说明）。
    ++m_pollGeneration;
    // 只负责“停表”，不碰 waitingFinger（见 onAddQuitClicked 的注释）。
    // 根据条件判断是否执行该分支。
    if (m_pollTimer) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        m_pollTimer->stop();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        m_pollTimer->deleteLater();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        m_pollTimer = nullptr;
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// 定义或声明函数 Form_finger::addFingerRequested。
void Form_finger::addFingerRequested()
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if (waitingFinger) return; // 防止重复点击
    // 执行本行语句完成对应变量、对象或业务状态处理。
    currentStep = 1;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    startFingerRegisterStep(currentStep);
// 结束当前代码块作用域。
}

// 定义或声明函数 Form_finger::startFingerRegisterStep。
void Form_finger::startFingerRegisterStep(int step)
// 进入新的代码块作用域。
{
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QWidget* w = parentWidget();
    // 开始循环处理满足条件的数据或状态。
    while (w && !qobject_cast<Widget*>(w)) w = w->parentWidget();
    // 执行本行语句完成对应变量、对象或业务状态处理。
    Widget* mainWidget = qobject_cast<Widget*>(w);
    // 根据条件判断是否执行该分支。
    if (!mainWidget || !mainWidget->fingerprint()) return;
    // 挂起常驻识别轮询：两者共用同一个串口，必须互斥（详见 onAddQuitClicked）。
    // 只记一次句柄，退出时成对恢复。
    if (!m_suspendedFingerprintManager && mainWidget->fingerprintManager()) {
        m_suspendedFingerprintManager = mainWidget->fingerprintManager();
        m_suspendedFingerprintManager->setSuspended(true);
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    waitingFinger = true;
    // 根据条件判断是否执行该分支。
    if (step == 1) mainWidget->audioManager()->playAsync(accessterminal::AppConfig::instance().pcmFile(QStringLiteral("caijifinger.pcm")));
    // 在前一条件不满足时继续判断该分支。
    else if (step == 2) mainWidget->audioManager()->playAsync(accessterminal::AppConfig::instance().pcmFile(QStringLiteral("caijifinger.pcm")));
    // 在前一条件不满足时继续判断该分支。
    else if (step == 3) mainWidget->audioManager()->playAsync(accessterminal::AppConfig::instance().pcmFile(QStringLiteral("caijifinger.pcm")));
    // 启动定时器轮询检测手指
    // 复用同一个成员定时器：重复进入本函数（第 2、3 步）时不要叠加新定时器。
    stopFingerPoll();
    // 创建或使用 Qt 定时器对象。
    m_pollTimer = new QTimer(this);
    // 把句柄按值捕获进 lambda（QTimer* 是值语义），这样回调里 stop/deleteLater
    // 操作的是本次这一只定时器；同时 clear 成员，避免回调里把已经 deleteLater
    // 的指针再指一次。
    QTimer* const timer = m_pollTimer;
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(timer, &QTimer::timeout, this, [=]() {
        // 检测有无手指，假设用 getImage 判断
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray payload;
        // 继续当前代码块的参数、表达式或结构定义。
        payload.append((char)0x01); // CMD_GET_IMAGE
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray pkt = mainWidget->fingerprint()->buildPacket(payload);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        mainWidget->fingerprint()->sendPacket(pkt);
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray resp = mainWidget->fingerprint()->readPacket(500);
        // 根据条件判断是否执行该分支。
        if (resp.size() >= 12 && (quint8)resp[9] == 0x00) {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            timer->stop();
            // 执行本行语句完成对应变量、对象或业务状态处理。
            timer->deleteLater();
            // 成员句柄同步清空，保持“句柄有效 <=> 定时器在跑”这一不变式。
            if (m_pollTimer == timer) m_pollTimer = nullptr;
            // 输出调试日志。
            qDebug() << QString("第%1次检测到手指，开始采集").arg(step);
            // 执行本行语句完成对应变量、对象或业务状态处理。
            doFingerCollect(step);
        // 结束当前代码块作用域。
        } else {
            // 继续等待
        // 结束当前代码块作用域。
        }
    // 结束当前代码块作用域。
    });
    // 执行本行语句完成对应变量、对象或业务状态处理。
    timer->start(500);
// 结束当前代码块作用域。
}

// 定义或声明函数 Form_finger::doFingerCollect。
void Form_finger::doFingerCollect(int step)
// 进入新的代码块作用域。
{
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QWidget* w = parentWidget();
    // 开始循环处理满足条件的数据或状态。
    while (w && !qobject_cast<Widget*>(w)) w = w->parentWidget();
    // 执行本行语句完成对应变量、对象或业务状态处理。
    Widget* mainWidget = qobject_cast<Widget*>(w);
    // 根据条件判断是否执行该分支。
    if (!mainWidget || !mainWidget->fingerprint()) return;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int id = mainWidget->nextFingerId();
    // 采集图像和生成特征
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray payload;
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)0x01); // CMD_GET_IMAGE
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray pkt = mainWidget->fingerprint()->buildPacket(payload);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    mainWidget->fingerprint()->sendPacket(pkt);
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray resp = mainWidget->fingerprint()->readPacket(2000);
    // 根据条件判断是否执行该分支。
    if (resp.size() < 12 || (quint8)resp[9] != 0x00) {
        // 输出调试日志。
        qDebug() << QString("第%1次采集图像失败").arg(step);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        waitingFinger = false;
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 执行本行语句完成对应变量、对象或业务状态处理。
    payload.clear();
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)0x02); // CMD_GEN_CHAR
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)step); // BufferID
    // 执行本行语句完成对应变量、对象或业务状态处理。
    pkt = mainWidget->fingerprint()->buildPacket(payload);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    mainWidget->fingerprint()->sendPacket(pkt);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    resp = mainWidget->fingerprint()->readPacket(2000);
    // 根据条件判断是否执行该分支。
    if (resp.size() < 12 || (quint8)resp[9] != 0x00) {
        // 输出调试日志。
        qDebug() << QString("第%1次生成特征失败").arg(step);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        waitingFinger = false;
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 输出调试日志。
    qDebug() << QString("第%1次采集成功").arg(step);
    // 播放对应提示音
    // 根据条件判断是否执行该分支。
    if (step == 1) mainWidget->audioManager()->playAsync(accessterminal::AppConfig::instance().pcmFile(QStringLiteral("fingerone.pcm")));
    // 在前一条件不满足时继续判断该分支。
    else if (step == 2) mainWidget->audioManager()->playAsync(accessterminal::AppConfig::instance().pcmFile(QStringLiteral("fingertwo.pcm")));
    // 在前一条件不满足时继续判断该分支。
    else if (step == 3) mainWidget->audioManager()->playAsync(accessterminal::AppConfig::instance().pcmFile(QStringLiteral("fingerthree.pcm")));
    // 进入下一步
    // 根据条件判断是否执行该分支。
    if (step < 3) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        currentStep++;
        // 记录发起时的代数：若这 1.5 秒内用户点了『退出』（stopFingerPoll()
        // 会 ++m_pollGeneration），下面的延时回调就作废，不再把轮询拉起来。
        const int gen = m_pollGeneration;
        // 定义类 QTimer 的成员函数 singleShot。
        QTimer::singleShot(1500, this, [this, gen]() {
            if (gen != m_pollGeneration) return; // 已被取消（退出/析构）
            startFingerRegisterStep(currentStep);
        });
    // 结束当前代码块作用域。
    } else {
        // 合并特征和存储模板
        // 执行本行语句完成对应变量、对象或业务状态处理。
        payload.clear();
        // 继续当前代码块的参数、表达式或结构定义。
        payload.append((char)0x05); // CMD_REG_MODEL
        // 执行本行语句完成对应变量、对象或业务状态处理。
        pkt = mainWidget->fingerprint()->buildPacket(payload);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        mainWidget->fingerprint()->sendPacket(pkt);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        resp = mainWidget->fingerprint()->readPacket(2000);
        // 根据条件判断是否执行该分支。
        if (resp.size() < 12 || (quint8)resp[9] != 0x00) {
            // 输出调试日志。
            qDebug() << "合并特征失败";
            // 执行本行语句完成对应变量、对象或业务状态处理。
            waitingFinger = false;
            // 返回当前函数的执行结果或结束函数。
            return;
        // 结束当前代码块作用域。
        }
        // 执行本行语句完成对应变量、对象或业务状态处理。
        payload.clear();
        // 继续当前代码块的参数、表达式或结构定义。
        payload.append((char)0x06); // CMD_STORE_CHAR
        // 继续当前代码块的参数、表达式或结构定义。
        payload.append((char)1); // BufferID
        // 执行本行语句完成对应变量、对象或业务状态处理。
        payload.append((char)(id >> 8));
        // 执行本行语句完成对应变量、对象或业务状态处理。
        payload.append((char)(id & 0xFF));
        // 执行本行语句完成对应变量、对象或业务状态处理。
        pkt = mainWidget->fingerprint()->buildPacket(payload);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        mainWidget->fingerprint()->sendPacket(pkt);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        resp = mainWidget->fingerprint()->readPacket(2000);
        // 根据条件判断是否执行该分支。
        if (resp.size() < 12 || (quint8)resp[9] != 0x00) {
            // 输出调试日志。
            qDebug() << "存储模板失败";
            // 执行本行语句完成对应变量、对象或业务状态处理。
            waitingFinger = false;
            // 返回当前函数的执行结果或结束函数。
            return;
        // 结束当前代码块作用域。
        }
        // 输出调试日志。
        qDebug() << "注册成功";
        // 执行本行语句完成对应变量、对象或业务状态处理。
        mainWidget->incrementNextFingerId();
        // 执行本行语句完成对应变量、对象或业务状态处理。
        waitingFinger = false;
    // 结束当前代码块作用域。
    }
// 结束当前代码块作用域。
}

// 定义类 Form_finger 的成员函数 ~Form_finger。
Form_finger::~Form_finger()
// 进入新的代码块作用域。
{
    // 先停轮询再 delete ui：轮询回调里会经由 parentWidget() 向上找 Widget、
    // 访问 ui 所建立的控件树，析构期间让它继续触发是危险的。
    stopFingerPoll();
    // 兜底恢复常驻识别：若窗口是被父窗口直接销毁（没走 onAddQuitClicked），
    // 也必须把识别轮询放出来，否则门口机的指纹识别会永久失效。
    if (m_suspendedFingerprintManager) {
        m_suspendedFingerprintManager->setSuspended(false);
        m_suspendedFingerprintManager = nullptr;
    }
    // 释放动态创建的对象资源。
    delete ui;
// 结束当前代码块作用域。
}
