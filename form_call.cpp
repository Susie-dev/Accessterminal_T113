// 引入项目内部头文件 "form_call.h"。
#include "form_call.h"
// 引入项目内部头文件 "ui_form_call.h"。
#include "ui_form_call.h"
// 引入项目内部头文件 "widget.h"。
#include "widget.h"

// 定义类 Form_CALL 的成员函数 Form_CALL。
Form_CALL::Form_CALL(QWidget *parent) :
    // 继续当前代码块的参数、表达式或结构定义。
    QWidget(parent),
    // 继续当前代码块的参数、表达式或结构定义。
    ui(new Ui::Form_CALL)
// 进入新的代码块作用域。
{
    // 访问或更新 Qt 界面控件。
    ui->setupUi(this);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    this->setAttribute(Qt::WA_TranslucentBackground, true);
   // this->setStyleSheet("background-color: rgba(255, 255, 255, 180); ");
    // 访问或更新 Qt 界面控件。
    ui->btnstop->setFocusPolicy(Qt::NoFocus);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(ui->btnstop, &QPushButton::clicked, this, [this]() {
        // 打断主界面的提示音
        // 执行本行语句完成对应变量、对象或业务状态处理。
        QWidget* w = parentWidget();
        // 开始循环处理满足条件的数据或状态。
        while (w && !qobject_cast<Widget*>(w)) w = w->parentWidget();
        // 根据条件判断是否执行该分支。
        if (w) {
            // 使用自动类型推导保存表达式结果。
            auto widget = qobject_cast<Widget*>(w);
            // 根据条件判断是否执行该分支。
            if (widget) {
                // 执行本行语句完成对应变量、对象或业务状态处理。
                widget->audioManager->stop();
                // 通过TCP信令通道发送stopsend
                // 根据条件判断是否执行该分支。
                if (widget->tcpSignalSocket && widget->tcpSignalSocket->state() == QAbstractSocket::ConnectedState) {
                    // 执行本行语句完成对应变量、对象或业务状态处理。
                    widget->tcpSignalSocket->write("stopsend\n");
                    // 执行本行语句完成对应变量、对象或业务状态处理。
                    widget->tcpSignalSocket->flush();
                // 结束当前代码块作用域。
                }
            // 结束当前代码块作用域。
            }
        // 结束当前代码块作用域。
        }
        // 发出 Qt 信号 hangupRequested 通知其他对象。
        emit hangupRequested();
        // 不主动关闭窗口，交由主界面处理
    // 结束当前代码块作用域。
    });
    // 执行本行语句完成对应变量、对象或业务状态处理。
    callTimer = new QTimer(this);
    // 建立 Qt 信号与槽或 lambda 回调连接。
    connect(callTimer, &QTimer::timeout, this, [this]() {
        // 执行本行语句完成对应变量、对象或业务状态处理。
        callSeconds++;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        int min = callSeconds / 60;
        // 执行本行语句完成对应变量、对象或业务状态处理。
        int sec = callSeconds % 60;
        // 访问或更新 Qt 界面控件。
        ui->label->setText(QString("通话中 %1:%2").arg(min, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0')));
    // 结束当前代码块作用域。
    });
// 结束当前代码块作用域。
}

// 定义或声明函数 Form_CALL::setStatusText。
void Form_CALL::setStatusText(const QString& text) {
    // 访问或更新 Qt 界面控件。
    ui->label->setText(text);
// 结束当前代码块作用域。
}

// 定义或声明函数 Form_CALL::startCallTimer。
void Form_CALL::startCallTimer() {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    callSeconds = 0;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    callTimer->start(1000);
    // 访问或更新 Qt 界面控件。
    ui->label->setText("通话中 00:00");
// 结束当前代码块作用域。
}

// 定义或声明函数 Form_CALL::stopCallTimer。
void Form_CALL::stopCallTimer() {
    // 执行本行语句完成对应变量、对象或业务状态处理。
    callTimer->stop();
// 结束当前代码块作用域。
}

// 定义类 Form_CALL 的成员函数 ~Form_CALL。
Form_CALL::~Form_CALL()
// 进入新的代码块作用域。
{
    // 释放动态创建的对象资源。
    delete ui;
// 结束当前代码块作用域。
}
