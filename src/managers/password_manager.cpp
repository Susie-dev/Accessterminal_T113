// 引入项目内部头文件 "password_manager.h"。
#include "managers/password_manager.h"
#include "config/app_config.h"
// 定义类 PasswordManager 的成员函数 PasswordManager。
PasswordManager::PasswordManager(QObject* parent)
    // 继续当前代码块的参数、表达式或结构定义。
    : QObject(parent)
// 进入新的代码块作用域。
{
    // 初始密码来自配置，避免默认值在多个文件中重复。
    password = accessterminal::AppConfig::instance().doorPassword();
}

// 定义类 PasswordManager 的成员函数 ~PasswordManager。
PasswordManager::~PasswordManager() {}

// 定义或声明函数 PasswordManager::setPassword。
void PasswordManager::setPassword(const QString& pwd)
// 进入新的代码块作用域。
{
    // 执行本行语句完成对应变量、对象或业务状态处理。
    password = pwd;
    // 发出 Qt 信号 passwordChanged 通知其他对象。
    emit passwordChanged();
// 结束当前代码块作用域。
}

// 定义或声明函数 PasswordManager::checkPassword。
void PasswordManager::checkPassword(const QString& input)
// 进入新的代码块作用域。
{
    // 不要在日志里输出密码内容。
    //
    // 重构前的实现把用户输入、**已存储的正确密码**、以及两者的 hex 全都
    // 打到 qDebug：任何一次输错都会把正确的门禁密码写进日志。门口机是
    // 实体门禁设备，密码属于物理准入凭据，而 qDebug 在嵌入式 Linux 上
    // 落到 stderr / systemd journal，可被任何有日志读取权限的人拿到，
    // 且跨重启留存。这等于把门禁密码直接交出去。
    //
    // 只保留“本次是否匹配”这一条非敏感信息：它有诊断价值，且不泄露
    // 任何一位密码。比较逻辑与信号语义保持不变。
    const bool matched = (input == password);
    qDebug() << "[PasswordManager] 密码校验完成，结果:" << (matched ? "通过" : "拒绝");
    // 发出 Qt 信号 passwordChecked 通知其他对象。
    emit passwordChecked(matched);
// 结束当前代码块作用域。
}
