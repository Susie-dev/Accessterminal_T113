// 引入项目内部头文件 "form_finger.h"。
#include "form_finger.h"
// 引入项目内部头文件 "ui_form_finger.h"。
#include "ui_form_finger.h"
// 引入系统或第三方头文件 <QTimer>。
#include <QTimer>
// 引入项目内部头文件 "widget.h"。
#include "widget.h"

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
    // 执行本行语句完成对应变量、对象或业务状态处理。
    this->close();
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
    if (!mainWidget || !mainWidget->getFingerprint()) return;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    waitingFinger = true;
    // 根据条件判断是否执行该分支。
    if (step == 1) mainWidget->audioManager->playAsync("/tmp/pcm/caijifinger.pcm");
    // 在前一条件不满足时继续判断该分支。
    else if (step == 2) mainWidget->audioManager->playAsync("/tmp/pcm/caijifinger.pcm");
    // 在前一条件不满足时继续判断该分支。
    else if (step == 3) mainWidget->audioManager->playAsync("/tmp/pcm/caijifinger.pcm");
    // 启动定时器轮询检测手指
    // 创建或使用 Qt 定时器对象。
    QTimer* timer = new QTimer(this);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(timer, &QTimer::timeout, this, [=]() {
        // 检测有无手指，假设用 getImage 判断
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray payload;
        // 继续当前代码块的参数、表达式或结构定义。
        payload.append((char)0x01); // CMD_GET_IMAGE
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray pkt = mainWidget->getFingerprint()->buildPacket(payload);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        mainWidget->getFingerprint()->sendPacket(pkt);
        // 创建或使用字节数组保存协议或音视频数据。
        QByteArray resp = mainWidget->getFingerprint()->readPacket(500);
        // 根据条件判断是否执行该分支。
        if (resp.size() >= 12 && (quint8)resp[9] == 0x00) {
            // 执行本行语句完成对应变量、对象或业务状态处理。
            timer->stop();
            // 执行本行语句完成对应变量、对象或业务状态处理。
            timer->deleteLater();
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
    if (!mainWidget || !mainWidget->getFingerprint()) return;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int id = mainWidget->getNextFingerId();
    // 采集图像和生成特征
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray payload;
    // 继续当前代码块的参数、表达式或结构定义。
    payload.append((char)0x01); // CMD_GET_IMAGE
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray pkt = mainWidget->getFingerprint()->buildPacket(payload);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    mainWidget->getFingerprint()->sendPacket(pkt);
    // 创建或使用字节数组保存协议或音视频数据。
    QByteArray resp = mainWidget->getFingerprint()->readPacket(2000);
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
    pkt = mainWidget->getFingerprint()->buildPacket(payload);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    mainWidget->getFingerprint()->sendPacket(pkt);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    resp = mainWidget->getFingerprint()->readPacket(2000);
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
    if (step == 1) mainWidget->audioManager->playAsync("/tmp/pcm/fingerone.pcm");
    // 在前一条件不满足时继续判断该分支。
    else if (step == 2) mainWidget->audioManager->playAsync("/tmp/pcm/fingertwo.pcm");
    // 在前一条件不满足时继续判断该分支。
    else if (step == 3) mainWidget->audioManager->playAsync("/tmp/pcm/fingerthree.pcm");
    // 进入下一步
    // 根据条件判断是否执行该分支。
    if (step < 3) {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        currentStep++;
        // 定义类 QTimer 的成员函数 singleShot。
        QTimer::singleShot(1500, this, [=]() { startFingerRegisterStep(currentStep); });
    // 结束当前代码块作用域。
    } else {
        // 合并特征和存储模板
        // 执行本行语句完成对应变量、对象或业务状态处理。
        payload.clear();
        // 继续当前代码块的参数、表达式或结构定义。
        payload.append((char)0x05); // CMD_REG_MODEL
        // 执行本行语句完成对应变量、对象或业务状态处理。
        pkt = mainWidget->getFingerprint()->buildPacket(payload);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        mainWidget->getFingerprint()->sendPacket(pkt);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        resp = mainWidget->getFingerprint()->readPacket(2000);
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
        pkt = mainWidget->getFingerprint()->buildPacket(payload);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        mainWidget->getFingerprint()->sendPacket(pkt);
        // 执行本行语句完成对应变量、对象或业务状态处理。
        resp = mainWidget->getFingerprint()->readPacket(2000);
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
    // 释放动态创建的对象资源。
    delete ui;
// 结束当前代码块作用域。
}
