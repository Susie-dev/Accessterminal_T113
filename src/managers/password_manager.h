/**
 * @file password_manager.h
 * @brief 门级密码校验：**不再持有明文**，改由 CredentialStore 加盐哈希（第 41 轮，方案 P2）。
 *
 * 对外契约**逐条不变**（这是硬约束）：
 *   - `setPassword(QString)` → 每次发一次 `passwordChanged()`；
 *   - `checkPassword(QString)` → 每次发一次 `passwordChecked(bool)`；
 *   - 空密码 + 空输入 = 通过（沿用重构前的 `input == password` 语义）；
 *   - 构造时口令来源仍是 `AppConfig::doorPassword()`。
 *
 * 变的是什么
 * ----------
 * 重构前成员是 `QString password`（**明文**），校验是 `input == password`。
 * 现在成员只有 `CredentialStore`（内部只有盐 + 摘要），明文只在
 * `setPassword()` / `checkPassword()` 的栈上活一瞬间。
 *
 * 收益不是"看不见明文"这么表面 —— 而是：
 *   1. 进程内存被 dump 时拿不到明文口令；
 *   2. 比对走**定长比较**，不再泄漏"前缀猜对几个字符"的时序信息；
 *   3. 每用户独立盐，相同口令不会产生相同摘要。
 *
 * 明文从哪来（**非静默**迁移，第 41 轮）
 * ------------------------------------
 * `AppConfig::doorPassword()` 目前是明文引导值（`appconfig.json` 里的 `"5788"`）。
 * 构造时按两种情形处理，且**一定会打日志说明走到了哪种**：
 *   - 若配置值**已是哈希串**（`sha256:...`）→ 直接装入；
 *   - 若是明文 → 现场加盐哈希装进内存，并打一条**显式的迁移告警**，
 *     同时（若配置了持久化路径）把它写进凭据文件。
 *
 * **为什么不把明文写回 `appconfig.json`**：那是运维手工编辑的文件，
 * 程序改写它会让"配置是人的输入"这一性质消失，且掉电时有损坏风险。
 * 改用**独立的凭据文件**承载哈希 —— 这也是本项目的既有取向：
 * 配置归配置，密钥材料归密钥材料。运维在首次启动后即可从
 * `appconfig.json` 里删掉 `door_password`（删除后仍能开门，因为哈希已在
 * 凭据文件里），文档里写明了这个流程。
 *
 * 安全边界（**不要高估**）
 * ----------------------
 * 加盐 + 迭代哈希**不是** Argon2/scrypt/PBKDF2：Qt5 不提供 PBKDF2，
 * 本类也没有内存硬度参数。它相对明文比对是实质提升，但**不能**用来保护
 * 高价值口令。更详细的说明见 `credential_store.h`。
 */
#ifndef PASSWORD_MANAGER_H
#define PASSWORD_MANAGER_H

#include <QObject>
#include <QString>
#include <QDebug>

#include "managers/credential_store.h"

class PasswordManager : public QObject
{
    Q_OBJECT
public:
    explicit PasswordManager(QObject* parent = nullptr);
    ~PasswordManager();

    /// 设置门级密码。明文只用于当次哈希，不留存。
    void setPassword(const QString& pwd);
    /// 校验门级密码。结果经 `passwordChecked(bool)` 上报。
    void checkPassword(const QString& input);

    /// 当前是否已装入口令（无论来自配置引导值还是凭据文件）。
    bool hasPassword() const;

    /// 存储层只读访问（供测试与诊断；生产代码不应绕过本类校验）。
    accessterminal::CredentialStore& credentials() { return m_credentials; }
    const accessterminal::CredentialStore& credentials() const { return m_credentials; }

signals:
    void passwordChecked(bool success);
    void passwordChanged();

private:
    /// 门级密码在存储层里的用户标识（单人门禁，固定 ID）。
    static QString doorUserId();

    accessterminal::CredentialStore m_credentials;
};

#endif // PASSWORD_MANAGER_H
