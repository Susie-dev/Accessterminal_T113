// 开始头文件防重复包含保护 PASSWORD_MANAGER_H。
#ifndef PASSWORD_MANAGER_H
// 定义预处理宏或常量 PASSWORD_MANAGER_H。
#define PASSWORD_MANAGER_H

// 引入系统或第三方头文件 <QObject>。
#include <QObject>
// 引入系统或第三方头文件 <QString>。
#include <QString>
// 引入系统或第三方头文件 <QDebug>。
#include <QDebug>

// 声明类 PasswordManager。
class PasswordManager : public QObject
// 进入新的代码块作用域。
{
    // 继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
// 声明类成员的 public 访问权限区域。
public:
    // 声明显式构造函数 PasswordManager。
    explicit PasswordManager(QObject* parent = nullptr);
    // 声明或定义析构函数 PasswordManager。
    ~PasswordManager();

    // 定义或声明函数 setPassword。
    void setPassword(const QString& pwd);
    // 定义或声明函数 checkPassword。
    void checkPassword(const QString& input);

// 声明 Qt 信号槽成员区域。
signals:
    // 定义或声明函数 passwordChecked。
    void passwordChecked(bool success);
    // 定义或声明函数 passwordChanged。
    void passwordChanged();

// 声明类成员的 private 访问权限区域。
private:
    // 密码来源为 AppConfig（settings.security.door_password），
    // 不再在多个文件里重复硬编码默认值。
    QString password;
// 结束当前代码块作用域。
};

// 结束当前预处理条件块。
#endif // PASSWORD_MANAGER_H
