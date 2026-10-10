// 开始头文件防重复包含保护 FORM_FINGER_H。
#ifndef FORM_FINGER_H
// 定义预处理宏或常量 FORM_FINGER_H。
#define FORM_FINGER_H

// 引入系统或第三方头文件 <QWidget>。
#include <QWidget>

// 声明或进入命名空间 Ui。
namespace Ui {
// 声明类 Form_finger。
class Form_finger;
// 结束当前代码块作用域。
}

class QTimer;

// 声明类 Form_finger。
class Form_finger : public QWidget
// 进入新的代码块作用域。
{
    // 继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT

// 声明类成员的 public 访问权限区域。
public:
    // 声明显式构造函数 Form_finger。
    explicit Form_finger(QWidget *parent = nullptr);
    // 声明或定义析构函数 Form_finger。
    ~Form_finger();

// 声明 Qt 信号槽成员区域。
signals:
    // 定义或声明函数 addFaceRequested。
    void addFaceRequested();

// 声明 Qt 信号槽成员区域。
public slots:
    // 定义或声明函数 addFingerRequested。
    void addFingerRequested();
    // 定义或声明函数 onAddQuitClicked。
    void onAddQuitClicked();

// 声明类成员的 private 访问权限区域。
private:
    // 执行本行语句完成对应变量、对象或业务状态处理。
    Ui::Form_finger *ui;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int currentStep;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    bool waitingFinger;
    /// “检测手指是否按下”的轮询定时器（500ms）。
    ///
    /// 必须保存句柄：该定时器一旦启动就一直跑（回调里 readPacket() 是阻塞读，
    /// 且位于 GUI 线程），只有“检测到手指”这一条路径会 stop 它。如果用户中途
    /// 点『退出』，原来那种 `QTimer* timer = new QTimer(this);` 的局部变量就再也
    /// 没人能停它——窗口隐藏了，定时器却永远每 500ms 阻塞一次 GUI 线程。
    ///
    /// 生命周期仍由 this 掌管（构造时以 this 为 parent），不需要手动 delete；
    /// 保存句柄只是为了能在退出/析构时 stop()。
    QTimer* m_pollTimer = nullptr;
    /// 轮询“代数”。每停一次轮询就 +1，用于作废此前排队的延时重启。
    ///
    /// 分步注册在两次采集之间会 `QTimer::singleShot(1500, ...)` 延时进入下一步。
    /// 若用户正好在这 1.5 秒内点『退出』，仅 stop 定时器是不够的：那个已经排队的
    /// singleShot 仍会准时触发，重新调用 startFingerRegisterStep() 把轮询又拉起来。
    /// 因此延时回调必须带上“发起时的代数”，与当前值不符就直接放弃。
    /// 与本工程 widget.cpp 中 m_callGeneration 的做法一致。
    int m_pollGeneration = 0;
    /// 停掉“检测手指”轮询，并作废已排队的延时重启。退出窗口与析构时都必须调用。
    void stopFingerPoll();
    // 定义或声明函数 startFingerRegisterStep。
    void startFingerRegisterStep(int step);
    // 定义或声明函数 doFingerCollect。
    void doFingerCollect(int step);
// 结束当前代码块作用域。
};

// 结束当前预处理条件块。
#endif // FORM_FINGER_H
