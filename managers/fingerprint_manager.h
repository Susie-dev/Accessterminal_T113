// 开始头文件防重复包含保护 FINGERPRINT_MANAGER_H。
#ifndef FINGERPRINT_MANAGER_H
// 定义预处理宏或常量 FINGERPRINT_MANAGER_H。
#define FINGERPRINT_MANAGER_H

// 引入系统或第三方头文件 <QObject>。
#include <QObject>

// 声明类 HailinFingerprint。
class HailinFingerprint;

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
// 结束当前代码块作用域。
};

// 结束当前预处理条件块。
#endif // FINGERPRINT_MANAGER_H
