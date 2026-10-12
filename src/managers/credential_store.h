/**
 * @file credential_store.h
 * @brief 凭据存储：**加盐迭代哈希**、按用户计数/锁定到期、临时码（第 41 轮，方案 P2）。
 *
 * 为什么要有这个类（针对的真实缺口）
 * ---------------------------------
 * 重构前 `PasswordManager::checkPassword()` 是：
 *
 *     const bool matched = (input == password);
 *
 * 即**明文比对**；而密码来自 `AppConfig::doorPassword()`，落到
 * `config/appconfig.json` 里也是明文（`"door_password": "5788"`）。
 * 门口机是**实体门禁设备**：
 *   - 配置文件可被任何能读该文件的人拿到（含运维、备份、镜像）；
 *   - 门禁密码是**物理准入凭据**，泄漏等于把钥匙给出去；
 *   - 明文比对还会因短路比较泄漏"前缀匹配长度"这一类时序信息。
 *
 * 本类把"密码怎么存、怎么比"从 `PasswordManager` 里拿出来，做成一个
 * **可单测的纯逻辑类**（不是 QObject），于是安全性质可以被真正断言，
 * 而不是只靠一条"日志里不许出现敏感变量名"的静态正则。
 *
 * 存储格式（哈希串）
 * -----------------
 *     sha256:<salt-hex>:<iterations>:<digest-hex>
 *
 * 例：`sha256:9f3c...ab:10000:41d8...77`
 *
 *   - `salt`：每用户独立的 16 字节随机盐（十六进制 32 字符）；
 *   - `iterations`：迭代次数（**密钥拉伸**）。单轮 SHA-256 对 4 位数字
 *     口令毫无抵抗力（GPU 每秒可算数十亿次）；迭代把每次猜测的代价乘以
 *     N，直接抬高离线爆破成本。上限由 `kMaxIterations` 兜底，防止配置文件
 *     里写个天文数字把开锁延迟拖成秒级（那是可用性缺陷，也是 DoS 面）；
 *   - `digest`：把盐与口令反复哈希 N 轮的结果。
 *
 * **诚实说明：这不是密码学意义上的"慢哈希"。** 真正的口令存储应当用
 * Argon2 / scrypt / PBKDF2-HMAC，它们有内存硬度参数。本项目**只用 Qt 自带能力**
 * （`QCryptographicHash`，Qt 5.9 即有），而 Qt5 不提供 PBKDF2。加盐 + 迭代
 * 是**在当前依赖约束下能做到的最好**，相对"明文比对"是实质提升，但**不要**
 * 把它当成 Argon2 的替代品去保护高价值口令。这一点必须写在文档里，
 * 不能让后来者以为"已经加盐哈希了所以很安全"。
 *
 * 时序安全
 * --------
 * 摘要比较走 `constantTimeEquals()`，不因首个不同字节提前返回 —— 消除
 * "前缀猜对了几个字符"这类可观测差异。盐的存在也让"相同口令得到相同哈希"
 * 不再成立（防彩虹表 / 防一眼看出两人同密码）。
 *
 * 默认**不改变现场行为**（沿用本项目一贯口径）
 * -------------------------------------------
 *   - `maxAttempts` 默认 **0 = 不锁定**（计数照记、照暴露，但不拒绝）。
 *     启用锁定是**行为变更**（正确的密码在锁定期内也会被拒），必须显式配置；
 *   - `storagePath` 默认**空 = 纯内存**，不落盘。落盘需显式配置路径，
 *     这样单元测试天然是干净隔离的，也不会在生产机上意外写出文件。
 *
 * 与硬约束的关系
 * --------------
 * 本类**不碰** `protocol.h`、端口、信令、UDP 帧格式与音效时序；
 * `PasswordManager` 对外的 `passwordChecked(bool)` / `passwordChanged()`
 * 信号语义**逐条不变**（含"每次调用恰好发一次"与"空密码+空输入→通过"）。
 */
#ifndef ACCESSTERMINAL_MANAGERS_CREDENTIAL_STORE_H
#define ACCESSTERMINAL_MANAGERS_CREDENTIAL_STORE_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace accessterminal {

/**
 * @brief 校验结果的**三值**返回，而不是单纯的 bool。
 *
 * 为什么不是 bool：`PasswordManager` 对外的信号是 `passwordChecked(bool)`，
 * 但"锁定期内被拒"与"口令不对"是两种**不同的**事件，日志与运维语义不同
 * （前者要提示"稍后再试"，后者是"密码错误"）。内部用三值区分，
 * 出口再按既有语义收敛成 bool，既不改外部契约，又不丢信息。
 */
enum class VerifyOutcome {
    Accepted,     ///< 口令正确
    Rejected,     ///< 口令错误
    LockedOut     ///< 处于锁定期，本次不校验口令
};

/**
 * @brief 凭据存储。纯逻辑类，不依赖 Qt 事件循环，可脱离 QObject 单测。
 */
class CredentialStore
{
public:
    /// 迭代次数下界：连 1 轮都不到就不是哈希了。
    static const int kMinIterations = 1;
    /// 迭代次数上界。防止配置里写天文数字把开锁延迟拖成秒级（可用性/DoS）。
    /// 现场用 ARM 时 10000 轮约为数十毫秒，远低于人的感知阈值。
    static const int kMaxIterations = 200000;
    /// 默认迭代次数。可被存储文件里的实际值覆盖（以便将来平滑加大）。
    static const int kDefaultIterations = 10000;
    /// 盐长度（字节）。
    static const int kSaltBytes = 16;

    explicit CredentialStore(const QString& storagePath = QString());
    ~CredentialStore();

    // ---- 存储位置与读写 ----

    /// 存储文件路径。**空 = 纯内存**（不读也不写文件）。
    QString storagePath() const { return m_storagePath; }
    void setStoragePath(const QString& path) { m_storagePath = path; }
    bool isPersistent() const { return !m_storagePath.isEmpty(); }

    /**
     * @brief 从存储文件读取。路径为空返回 false 且不做任何事。
     *
     * 文件不存在**不是错误**（首次启动就是这种情形），返回 false 但不告警；
     * JSON 损坏才是错误，会告警并**保持内存内容不变**（不静默清空凭据 ——
     * 那会让门禁突然接受任意口令或全部拒绝）。
     */
    bool load();

    /**
     * @brief 原子写回存储文件（`QSaveFile`：先写临时文件再 rename）。
     *
     * 用原子写是因为门口机可能被直接断电：非原子写会在掉电瞬间留下
     * **半截 JSON**，下次启动解析失败 —— 那是"门忽然打不开"的现场故障。
     */
    bool save() const;

    // ---- 口令 ----

    /// 该用户是否已设置口令。
    bool hasSecret(const QString& userId) const;
    /// 设置口令（明文进入，**只**以加盐哈希留存；明文不写入任何成员）。
    void setSecret(const QString& userId, const QString& plaintext);
    /// 直接装入一个**已哈希**的串（用于迁移/测试）。非哈希串返回 false。
    bool setHashedSecret(const QString& userId, const QString& hashed);
    /// 删除该用户的口令。
    void clearSecret(const QString& userId);

    /**
     * @brief 校验口令。会**先**判锁定，再比哈希。
     *
     * 计数规则（`maxAttempts > 0` 时才有实际影响）：
     *   - 正确 → 失败计数清零；
     *   - 错误 → 失败计数 +1，达到上限则设置 `lockedUntil = now + lockoutMs`；
     *   - 锁定中 → 直接返回 `LockedOut`，**不再比哈希**（避免在锁定期内
     *     继续给攻击者比对机会），也**不再累加**计数。
     */
    VerifyOutcome verify(const QString& userId, const QString& plaintext);

    // ---- 锁定策略（默认关闭 = 保持现状） ----

    /// 连续失败多少次后锁定。**0 = 永不锁定（默认）**。负数按 0 处理。
    void setMaxAttempts(int n);
    int maxAttempts() const { return m_maxAttempts; }
    /// 锁定时长（毫秒）。上限 24 小时，避免配置失误变成"永久锁死"。
    void setLockoutMs(int ms);
    int lockoutMs() const { return m_lockoutMs; }

    int failedAttempts(const QString& userId) const;
    bool isLockedOut(const QString& userId) const;
    /// 剩余锁定毫秒数（未锁定返回 0）。
    qint64 lockoutRemainingMs(const QString& userId) const;
    void resetAttempts(const QString& userId);

    // ---- 临时访客码（**只做存储层**，本轮不接 UI、不接放行路径） ----
    //
    // 方案第六节把"是否引入临时访客码（需要新增 UI 入口）"列为待决策第 5 条，
    // 用户已确认：**只做存储层，不接 UI**。因此这里提供了完整的
    // 签发 / 消费 / 过期语义与测试，但**没有任何生产调用者** ——
    // 这与"死代码"不同：它有测试、有文档、有明确的上线条件（接 UI）。
    // 规则 check_credential_store_is_used 会把这个状态如实记录。

    /// 签发一个临时码（默认 6 位数字）。返回码本身；`validMs<=0` 用默认有效期。
    QString issueTempCode(const QString& userId, int validMs = 0);
    /// 消费一个临时码：不存在/已用/已过期都返回 false；成功则标记为已用（一次性）。
    bool consumeTempCode(const QString& code);
    /// 未被消费且未过期的临时码数量（诊断用）。
    int activeTempCodeCount() const;
    /// 清掉所有已过期或已消费的临时码（保持存储文件不无限增长）。
    int purgeTempCodes();

    // ---- 可测试性：时钟注入 ----

    /**
     * @brief 覆盖"当前时间"，用于确定性地测试锁定到期。
     *
     * 用注入而不是"sleep 到过期"：后者让测试变慢且**不确定**
     * （README 第 14 条：时序断言要等性质，不要等固定时长）。
     * 传负数恢复使用真实系统时钟。
     */
    void setNowMsForTest(qint64 epochMs) { m_nowOverrideMs = epochMs; }
    void clearNowMsForTest() { m_nowOverrideMs = -1; }

    // ---- 编码/解码（便于测试与审计） ----

    QJsonObject toJson() const;
    bool fromJson(const QJsonObject& obj);

    // ---- 静态工具（公开是为了能被直接单测，而不必绕过存储层） ----

    /// 生成 `kSaltBytes` 字节随机盐。优先 `/dev/urandom`，取不到时退回 QUuid。
    static QByteArray makeSalt();
    /// 按 `sha256:<salt>:<iterations>:<digest>` 生成哈希串。
    static QString hashSecret(const QString& plaintext, const QByteArray& salt,
                              int iterations = kDefaultIterations);
    /// 判断一个串是否已经是本类的哈希格式（用于"明文迁移"识别）。
    static bool looksLikeHash(const QString& value);
    /// 定长（不提前返回）比较，消除前缀时序差异。
    static bool constantTimeEquals(const QString& a, const QString& b);

private:
    struct UserRecord {
        QByteArray salt;      ///< 原始盐字节（JSON 里存 hex）
        QString digest;       ///< 摘要 hex
        int iterations = kDefaultIterations;
        int failed = 0;
        qint64 lockedUntilMs = 0;
    };

    struct TempCode {
        QString code;
        QString userId;
        qint64 expiresAtMs = 0;
        bool used = false;
    };

    /// 当前时间（毫秒）。测试注入优先。
    qint64 nowMs() const;
    UserRecord* findUser(const QString& userId);
    const UserRecord* findUser(const QString& userId) const;

    QString m_storagePath;
    QVector<QString> m_userIds;      ///< 保持稳定顺序，便于 JSON 可比对
    QVector<UserRecord> m_users;
    QVector<TempCode> m_tempCodes;
    int m_maxAttempts = 0;           ///< 0 = 不锁定（默认，保持现状）
    int m_lockoutMs = 60000;
    int m_tempCodeValidMs = 5 * 60 * 1000;
    int m_tempCodeDigits = 6;
    qint64 m_nowOverrideMs = -1;
};

} // namespace accessterminal

#endif // ACCESSTERMINAL_MANAGERS_CREDENTIAL_STORE_H
