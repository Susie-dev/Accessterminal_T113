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
    // 定义或声明函数 startFingerRegisterStep。
    void startFingerRegisterStep(int step);
    // 定义或声明函数 doFingerCollect。
    void doFingerCollect(int step);
// 结束当前代码块作用域。
};

// 结束当前预处理条件块。
#endif // FORM_FINGER_H
