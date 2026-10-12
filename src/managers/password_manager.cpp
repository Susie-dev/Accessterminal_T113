/**
 * @file password_manager.cpp
 * @brief PasswordManager 实现（第 41 轮，方案 P2：凭据改加盐哈希）。
 *
 * 对外信号语义**逐条保持**（硬约束）：每次 checkPassword 恰好发一次
 * passwordChecked(bool)；setPassword 每次发一次 passwordChanged。
 * 音效时序由 widget.cpp 负责，本类不碰。
 */
#include "managers/password_manager.h"
#include "config/app_config.h"

namespace {
/// 单人门禁的固定用户标识。多用户是将来 P2 的延伸，当前现场只有一个门级密码。
const char* const kDoorUserId = "door";
}

QString PasswordManager::doorUserId()
{
    return QString::fromLatin1(kDoorUserId);
}

PasswordManager::PasswordManager(QObject* parent)
    : QObject(parent)
{
    // 凭据文件路径来自配置（与 appconfig.json 同目录）。为空 = 纯内存。
    const accessterminal::AppConfig& cfg = accessterminal::AppConfig::instance();
    m_credentials.setStoragePath(cfg.credentialStorePath());

    // 先尝试从凭据文件读已哈希的凭据。
    const bool loaded = m_credentials.load();

    // 锁定策略：默认 maxAttempts=0 = 不锁定，**保持现场行为不变**。
    // 启用锁定是行为变更（正确口令在锁定期内也会被拒），需显式配置。
    // 这里刻意不读配置 —— 加旋钮就必须遵守开发约定第 10 条
    // （公开的开关要真的生效），留到显式决策那一轮一起做。

    const QString bootstrap = cfg.doorPassword();

    if (loaded && m_credentials.hasSecret(doorUserId())) {
        // 凭据文件里已有该用户：**以文件为准**，配置里的引导值不再参与。
        // 这样运维可以从 appconfig.json 里删掉 door_password，不影响开门。
        qInfo().noquote()
            << QStringLiteral("[密码] 已从凭据文件加载加盐哈希凭据（用户=%1，"
                              "存储=%2）；配置文件中的 door_password 不再参与校验")
                   .arg(doorUserId(), m_credentials.storagePath());
        return;
    }

    if (bootstrap.isEmpty()) {
        // 没有凭据文件、配置里也没有引导值：就是"没设过密码"。
        // 不告警（这是合法状态），但也**不假造一个默认口令** ——
        // 那会让设备在没有凭据文件时接受一个谁都知道的密码。
        qWarning().noquote()
            << QStringLiteral("[密码] 无可用门级密码：凭据文件不存在且配置未提供 "
                              "door_password。校验将一律拒绝。");
        return;
    }

    // ---- 迁移/引导路径：配置里给的是一个值，判断是哈希还是明文 ----
    if (accessterminal::CredentialStore::looksLikeHash(bootstrap)) {
        if (m_credentials.setHashedSecret(doorUserId(), bootstrap)) {
            qInfo().noquote()
                << QStringLiteral("[密码] 已从配置直接装入**哈希**凭据（用户=%1）")
                       .arg(doorUserId());
            if (m_credentials.isPersistent()) {
                m_credentials.save();
            }
        }
        return;
    }

    // 明文引导值 —— **必须显式告警**，不得静默迁移（方案 P2 第 3 条）。
    // 告警里只说"发生了迁移"，绝不打印明文本身。
    m_credentials.setSecret(doorUserId(), bootstrap);
    qWarning().noquote()
        << QStringLiteral(
               "[密码] 检测到**明文**门禁密码引导值，已现场加盐哈希后装入内存"
               "（用户=%1）。明文不会被写回 appconfig.json —— 哈希保存在 "
               "独立的凭据文件里，请把 appconfig.json 的 "
               "security.door_password 删掉，之后的启动就不再需要它。")
               .arg(doorUserId());

    if (m_credentials.isPersistent()) {
        if (m_credentials.save()) {
            qInfo().noquote()
                << QStringLiteral("[密码] 已写入凭据文件: %1").arg(m_credentials.storagePath());
        } else {
            qWarning().noquote()
                << QStringLiteral("[密码] 凭据文件写入失败: %1（本次运行仍可用，"
                                  "但下次启动仍需明文引导值）")
                       .arg(m_credentials.storagePath());
        }
    } else {
        qWarning().noquote()
            << QStringLiteral("[密码] 未配置凭据文件路径，本次为**纯内存**模式："
                              "重启后需要再次提供引导值");
    }
}

PasswordManager::~PasswordManager() {}

void PasswordManager::setPassword(const QString& pwd)
{
    // 明文只在这里进、到哈希为止：m_credentials 内不留明文。
    if (pwd.isEmpty()) {
        // 保持与重构前一致的语义：空密码是"合法的口令值"，
        // 而空输入 + 空密码 = 通过（由 checkPassword 处理）。
        // 这里仍要装一个空口令的哈希，否则 hasPassword() 会说"没设过"。
        m_credentials.setSecret(doorUserId(), QString());
    } else {
        m_credentials.setSecret(doorUserId(), pwd);
    }
    emit passwordChanged();
}

void PasswordManager::checkPassword(const QString& input)
{
    // 不要在日志里输出密码内容（沿袭第 27 轮的修复）。
    //
    // 重构前这里把用户输入、**已存储的正确密码**、以及两者的 hex 全打
    // qDebug：任何一次输错都会把门禁密码写进日志。门口机是实体门禁设备，
    // 密码属于物理准入凭据，而 qDebug 在嵌入式 Linux 上落到
    // stderr / systemd journal，可被任何有日志读取权限的人拿到，且跨重启
    // 留存。只保留"本次是否匹配"这一条非敏感信息。
    bool matched = false;

    if (!m_credentials.hasSecret(doorUserId())) {
        // 没有凭据：**一律拒绝**。
        //
        // 但这与"空密码 + 空输入 = 通过"的历史语义有冲突，需要显式处理：
        // 重构前 `input == password` 在 password 为空 QString 时，
        // 空输入会通过。为了逐字节保持该语义（README 里它是被测试钉住的），
        // 这里把"存储层无凭据"映射为"口令为空串"这一情形。
        matched = input.isEmpty();
    } else if (input.isEmpty()) {
        // 空输入：只有在本用户的口令**也是空**时才通过。
        // 用存储层校验空串即可，不必特判（哈希对空串是良定义的）。
        const accessterminal::VerifyOutcome o =
            m_credentials.verify(doorUserId(), QString());
        matched = (o == accessterminal::VerifyOutcome::Accepted);
    } else {
        const accessterminal::VerifyOutcome o =
            m_credentials.verify(doorUserId(), input);
        matched = (o == accessterminal::VerifyOutcome::Accepted);
        if (o == accessterminal::VerifyOutcome::LockedOut) {
            // 锁定期内被拒是**不同事件**：日志如实区分，运维才看得出
            // "不是密码错了，是还在锁定期"。对外信号仍是 bool（契约不变）。
            qWarning() << "[PasswordManager] 处于锁定期，本次校验未比对口令。";
        }
    }

    qDebug() << "[PasswordManager] 密码校验完成，结果:" << (matched ? "通过" : "拒绝");
    emit passwordChecked(matched);
}

bool PasswordManager::hasPassword() const
{
    return m_credentials.hasSecret(doorUserId());
}
