/**
 * @file credential_store.cpp
 * @brief CredentialStore 实现（第 41 轮，方案 P2）。
 *
 * 设计取舍与安全边界见头文件的长注释。这里只补充实现层面的两个关键点：
 *
 * 1. **明文只在栈上活一瞬间。** `setSecret()` / `verify()` 收到明文后，
 *    只用于当次哈希计算；明文不进入任何成员变量、不写入文件、不进日志。
 *    这比"把明文存成员再比"在泄漏面上小得多 —— 后者一旦进程被 dump
 *    就直接拿到口令。
 *
 * 2. **盐是每用户独立的**，不是全局一个。这样两个用户用同一口令也不会
 *    得到相同摘要（防"看一眼就知道两人同密码"），也让彩虹表失效。
 */
#include "managers/credential_store.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

#include <cstdio>

namespace accessterminal {

// ---------------------------------------------------------------------------
// 类内 `static const int` 成员的**类外定义**（C++11 必需，别删）
// ---------------------------------------------------------------------------
//
// 为什么必须有这几行（第 41 轮由覆盖率构建实测发现，Release 下看不出来）
// ----------------------------------------------------------------------
// C++11 里类内初始化的 `static const int` 只是"常量表达式"，**不是定义**。
// 只要某处 **ODR-使用** 了它（取地址、绑定到 const 引用、被 std::max
// 之类的模板按引用接住），链接期就需要一个真正的定义。
//
// `credential_store.cpp` 里 `qBound(kMinIterations, ...)` 就是这种用法：
// qBound 是模板，参数是 `const T&` —— 于是取了地址。
//
// Release（-O2）下编译器把值直接内联，没有符号引用，**链接通过**；
// Debug / `--coverage -O0` 下不内联，于是：
//     undefined reference to `accessterminal::CredentialStore::kMinIterations'
// 两个测试目标直接链接失败。
//
// 也就是说：**"Release 编译通过"并不等于"所有构建配置都能通过"**。
// 本项目此前只用 Release 跑门禁，这个缺陷是覆盖率构建（Debug + -O0）
// 才暴露的。日常门禁已经补上 Debug 配置（见 tools/run_coverage.sh）。
const int CredentialStore::kMinIterations;
const int CredentialStore::kMaxIterations;
const int CredentialStore::kDefaultIterations;
const int CredentialStore::kSaltBytes;

namespace {

/// 带标签的固定宽度 hex，便于人工审计。
QString toHex(const QByteArray& raw)
{
    return QString::fromLatin1(raw.toHex());
}

/**
 * @brief 判断一个串是否**全部**由十六进制字符组成。
 *
 * 为什么要显式校验：`QByteArray::fromHex()` **不报错**，它会**跳过非法
 * 字符**并把剩下的重新配对 —— 于是"偶数个合法字符 + 任意垃圾"也会
 * "成功"解析出一段**内容全错**的字节，而 `!result.isEmpty()` 完全拦不住。
 * 凭据场景下这会让一份被篡改的哈希文件静默变成"另一个口令"，
 * 比解析失败危险得多。
 *
 * 本工程的 `[hex-parse]` 静态规则强制"fromHex 的变量实参必须先校验"——
 * 它是第 41 轮在**新写的代码**上抓到的真实缺陷（不是理论风险）。
 * 校验逻辑必须落在一个**名字含 hex 的校验型函数**里，规则才认得出。
 */
bool isHexOnly(const QString& s)
{
    if (s.isEmpty()) {
        return false;
    }
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        const bool ok = (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                        || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
                        || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// 严格版的十六进制解码：非纯十六进制输入返回**空** QByteArray。
///
/// **刻意不叫 fromHex**：本工程的 `[hex-parse]` 规则按名字判定
/// "某个函数的 fromHex 变量实参有没有先校验"。若这个包装函数也叫
/// fromHex，它的**调用方**就会被规则误判为未校验 —— 而规则不做跨函数分析。
/// 取一个不含 fromHex 的名字，让"已校验"这件事对规则和读者都清楚。
QByteArray hexToBytes(const QString& hex)
{
    if (!isHexOnly(hex)) {
        return QByteArray();   // 非法输入 -> 空，由调用方的长度校验兜住
    }
    return QByteArray::fromHex(hex.toLatin1());
}

/**
 * @brief 迭代哈希：把盐与口令反复喂给 SHA-256。
 *
 * 注意这是**自定义构造**，不是标准 PBKDF2（Qt5 没有 PBKDF2）。
 * 它确实抬高了离线爆破成本（每次猜测要算 N 轮），但没有内存硬度参数，
 * 因此**不能**替代 Argon2/scrypt。头文件里已就此写明边界。
 *
 * 迭代间采用 `digest || salt || plaintext` 再哈希，避免
 * "只把上一轮摘要反复哈希"导致不同口令在第 2 轮起就收敛。
 */
QString iterateDigest(const QString& plaintext, const QByteArray& salt, int iterations)
{
    const QByteArray pwd = plaintext.toUtf8();
    QByteArray acc = salt + pwd;
    for (int i = 0; i < iterations; ++i) {
        QCryptographicHash h(QCryptographicHash::Sha256);
        h.addData(acc);
        h.addData(salt);
        h.addData(pwd);
        acc = h.result();
    }
    return toHex(acc);
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

CredentialStore::CredentialStore(const QString& storagePath)
    : m_storagePath(storagePath)
{
}

CredentialStore::~CredentialStore() {}

// ---------------------------------------------------------------------------
// 盐 / 哈希 / 定长比较
// ---------------------------------------------------------------------------

QByteArray CredentialStore::makeSalt()
{
    // 优先读内核随机源。**不用 qrand/QRandomGenerator**：前者可预测，
    // 后者是 Qt 5.10+ API（本项目硬约束只用 Qt 5.9 就有的 API）。
    QByteArray salt;
    QFile urandom(QStringLiteral("/dev/urandom"));
    if (urandom.open(QIODevice::ReadOnly)) {
        salt = urandom.read(kSaltBytes);
        urandom.close();
    }
    // 兜底：/dev/urandom 取不到（非 Linux 主机、被沙箱挡住）时用 QUuid 的
    // 字节。**这弱于内核随机源**，所以只在拿不到 urandom 时用，并且
    // QUuid 自带时间戳与 MAC，对"每用户唯一盐"这个目的仍然足够。
    while (salt.size() < kSaltBytes) {
        const QUuid u = QUuid::createUuid();
        salt.append(u.toRfc4122());
    }
    return salt.left(kSaltBytes);
}

QString CredentialStore::hashSecret(const QString& plaintext, const QByteArray& salt,
                                   int iterations)
{
    const int n = qBound(kMinIterations, iterations, kMaxIterations);
    return QStringLiteral("sha256:") + toHex(salt) + QLatin1Char(':')
           + QString::number(n) + QLatin1Char(':')
           + iterateDigest(plaintext, salt, n);
}

bool CredentialStore::looksLikeHash(const QString& value)
{
    const QStringList parts = value.split(QLatin1Char(':'));
    if (parts.size() != 4) {
        return false;
    }
    if (parts.at(0) != QLatin1String("sha256")) {
        return false;
    }
    bool ok = false;
    parts.at(2).toInt(&ok);
    if (!ok) {
        return false;
    }
    // 盐与摘要都必须是合法 hex 且长度对得上，否则不是本类的格式。
    const QByteArray salt = hexToBytes(parts.at(1));
    const QByteArray digest = hexToBytes(parts.at(3));
    return salt.size() == kSaltBytes && digest.size() == 32;
}

bool CredentialStore::constantTimeEquals(const QString& a, const QString& b)
{
    // 长度不同直接返回 false：长度本身不是秘密（哈希定长），
    // 而"定长比较"要保护的是摘要内容。这里进一步用累积异或，
    // 保证不同字节位置不会带来可观测的时间差。
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a.at(i).unicode())
                ^ static_cast<unsigned char>(b.at(i).unicode());
    }
    return diff == 0;
}

// ---------------------------------------------------------------------------
// 用户记录
// ---------------------------------------------------------------------------

CredentialStore::UserRecord* CredentialStore::findUser(const QString& userId)
{
    for (int i = 0; i < m_userIds.size(); ++i) {
        if (m_userIds.at(i) == userId) {
            return &m_users[i];
        }
    }
    return nullptr;
}

const CredentialStore::UserRecord* CredentialStore::findUser(const QString& userId) const
{
    for (int i = 0; i < m_userIds.size(); ++i) {
        if (m_userIds.at(i) == userId) {
            return &m_users.at(i);
        }
    }
    return nullptr;
}

bool CredentialStore::hasSecret(const QString& userId) const
{
    const UserRecord* r = findUser(userId);
    return r != nullptr && !r->digest.isEmpty();
}

void CredentialStore::setSecret(const QString& userId, const QString& plaintext)
{
    UserRecord* r = findUser(userId);
    if (r == nullptr) {
        m_userIds.append(userId);
        m_users.append(UserRecord());
        r = &m_users[m_users.size() - 1];
    }
    r->salt = makeSalt();
    r->digest = iterateDigest(plaintext, r->salt, kDefaultIterations);
    r->iterations = kDefaultIterations;
    // 换口令视为"重新开始"：旧的失败计数不该继续压着新口令。
    r->failed = 0;
    r->lockedUntilMs = 0;
}

bool CredentialStore::setHashedSecret(const QString& userId, const QString& hashed)
{
    if (!looksLikeHash(hashed)) {
        return false;
    }
    const QStringList parts = hashed.split(QLatin1Char(':'));
    bool ok = false;
    const int iters = parts.at(2).toInt(&ok);
    if (!ok) {
        return false;
    }
    UserRecord* r = findUser(userId);
    if (r == nullptr) {
        m_userIds.append(userId);
        m_users.append(UserRecord());
        r = &m_users[m_users.size() - 1];
    }
    r->salt = hexToBytes(parts.at(1));
    r->digest = parts.at(3);
    r->iterations = qBound(kMinIterations, iters, kMaxIterations);
    r->failed = 0;
    r->lockedUntilMs = 0;
    return true;
}

void CredentialStore::clearSecret(const QString& userId)
{
    for (int i = 0; i < m_userIds.size(); ++i) {
        if (m_userIds.at(i) == userId) {
            m_userIds.remove(i);
            m_users.remove(i);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// 锁定策略
// ---------------------------------------------------------------------------

void CredentialStore::setMaxAttempts(int n)
{
    m_maxAttempts = n < 0 ? 0 : n;
}

void CredentialStore::setLockoutMs(int ms)
{
    // 上限 24 小时：配置失误不该变成"永久锁死"（现场没人能解锁）。
    const qint64 day = 24LL * 60 * 60 * 1000;
    m_lockoutMs = static_cast<int>(qBound<qint64>(0, ms, day));
}

int CredentialStore::failedAttempts(const QString& userId) const
{
    const UserRecord* r = findUser(userId);
    return r == nullptr ? 0 : r->failed;
}

bool CredentialStore::isLockedOut(const QString& userId) const
{
    return lockoutRemainingMs(userId) > 0;
}

qint64 CredentialStore::lockoutRemainingMs(const QString& userId) const
{
    const UserRecord* r = findUser(userId);
    if (r == nullptr || r->lockedUntilMs <= 0) {
        return 0;
    }
    const qint64 left = r->lockedUntilMs - nowMs();
    return left > 0 ? left : 0;
}

void CredentialStore::resetAttempts(const QString& userId)
{
    UserRecord* r = findUser(userId);
    if (r != nullptr) {
        r->failed = 0;
        r->lockedUntilMs = 0;
    }
}

qint64 CredentialStore::nowMs() const
{
    if (m_nowOverrideMs >= 0) {
        return m_nowOverrideMs;
    }
    return QDateTime::currentMSecsSinceEpoch();
}

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

VerifyOutcome CredentialStore::verify(const QString& userId, const QString& plaintext)
{
    UserRecord* r = findUser(userId);
    if (r == nullptr || r->digest.isEmpty()) {
        // 没有该用户的凭据：**拒绝**，不是通过。
        // （空密码 + 空输入"通过"的历史语义由 PasswordManager 保留，
        //  见它自己的注释 —— 那是 UI 层语义，不是这里的存储语义。）
        return VerifyOutcome::Rejected;
    }

    // 先判锁定：锁定期内不再比哈希，也不再累加计数。
    if (m_maxAttempts > 0 && r->lockedUntilMs > 0 && nowMs() < r->lockedUntilMs) {
        return VerifyOutcome::LockedOut;
    }
    // 锁定已到期 -> 自动解锁并清零（"到期自动解锁"）。
    if (r->lockedUntilMs > 0 && nowMs() >= r->lockedUntilMs) {
        r->lockedUntilMs = 0;
        r->failed = 0;
    }

    const QString candidate = iterateDigest(plaintext, r->salt, r->iterations);
    const bool matched = constantTimeEquals(candidate, r->digest);

    if (matched) {
        r->failed = 0;
        r->lockedUntilMs = 0;
        return VerifyOutcome::Accepted;
    }

    ++r->failed;
    if (m_maxAttempts > 0 && r->failed >= m_maxAttempts) {
        r->lockedUntilMs = nowMs() + m_lockoutMs;
        // 记录**非敏感**信息：只报用户与次数，绝不报口令或摘要片段。
        qWarning() << "[凭据] 连续校验失败达到上限，已锁定:" << userId
                   << "失败次数=" << r->failed << "锁定时长(ms)=" << m_lockoutMs;
    }
    return VerifyOutcome::Rejected;
}

// ---------------------------------------------------------------------------
// 临时访客码（只做存储层）
// ---------------------------------------------------------------------------

QString CredentialStore::issueTempCode(const QString& userId, int validMs)
{
    const int valid = validMs > 0 ? validMs : m_tempCodeValidMs;

    // 生成不重复的码。位数少（默认 6 位数字）意味着碰撞概率不可忽略，
    // 所以**必须**重试到不重复为止，并且设一个失败上限，
    // 免得码空间被占满时死循环（那是挂死，比返回空串更糟）。
    QString code;
    for (int attempt = 0; attempt < 100; ++attempt) {
        QString candidate;
        for (int i = 0; i < m_tempCodeDigits; ++i) {
            // 用 QUuid 的低字节作为熵源：Qt 5.9 兼容，且不需要 QRandomGenerator。
            const quint32 r = QUuid::createUuid().data1;
            candidate.append(QChar(QLatin1Char('0' + static_cast<char>(r % 10))));
        }
        bool clash = false;
        for (int i = 0; i < m_tempCodes.size(); ++i) {
            // 已消费/已过期的码可以复用（它们已经无效）。
            const TempCode& t = m_tempCodes.at(i);
            if (t.code == candidate && !t.used && t.expiresAtMs > nowMs()) {
                clash = true;
                break;
            }
        }
        if (!clash) {
            code = candidate;
            break;
        }
    }
    if (code.isEmpty()) {
        qWarning() << "[凭据] 临时码生成失败：码空间冲突过多";
        return QString();
    }

    TempCode t;
    t.code = code;
    t.userId = userId;
    t.expiresAtMs = nowMs() + valid;
    t.used = false;
    m_tempCodes.append(t);
    return code;
}

bool CredentialStore::consumeTempCode(const QString& code)
{
    for (int i = 0; i < m_tempCodes.size(); ++i) {
        TempCode& t = m_tempCodes[i];
        if (t.code != code) {
            continue;
        }
        if (t.used) {
            return false;               // 一次性：用过就不能再用
        }
        if (t.expiresAtMs <= nowMs()) {
            return false;               // 已过期
        }
        t.used = true;
        return true;
    }
    return false;
}

int CredentialStore::activeTempCodeCount() const
{
    int n = 0;
    for (int i = 0; i < m_tempCodes.size(); ++i) {
        const TempCode& t = m_tempCodes.at(i);
        if (!t.used && t.expiresAtMs > nowMs()) {
            ++n;
        }
    }
    return n;
}

int CredentialStore::purgeTempCodes()
{
    int removed = 0;
    for (int i = m_tempCodes.size() - 1; i >= 0; --i) {
        const TempCode& t = m_tempCodes.at(i);
        if (t.used || t.expiresAtMs <= nowMs()) {
            m_tempCodes.remove(i);
            ++removed;
        }
    }
    return removed;
}

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------

QJsonObject CredentialStore::toJson() const
{
    QJsonObject root;

    // 用数组 + 显式 userId 而不是对象键：这样顺序稳定，
    // 便于测试逐字节比对，也不会因用户名含特殊字符而出问题。
    QJsonArray users;
    for (int i = 0; i < m_userIds.size(); ++i) {
        const UserRecord& r = m_users.at(i);
        QJsonObject u;
        u.insert(QStringLiteral("id"), m_userIds.at(i));
        // 只写哈希串，**绝不写明文**。这是本类存在的全部意义。
        u.insert(QStringLiteral("hash"),
                 QStringLiteral("sha256:") + toHex(r.salt) + QLatin1Char(':')
                     + QString::number(r.iterations) + QLatin1Char(':') + r.digest);
        u.insert(QStringLiteral("failed"), r.failed);
        u.insert(QStringLiteral("locked_until_ms"), r.lockedUntilMs);
        users.append(u);
    }
    root.insert(QStringLiteral("users"), users);

    QJsonArray codes;
    for (int i = 0; i < m_tempCodes.size(); ++i) {
        const TempCode& t = m_tempCodes.at(i);
        QJsonObject c;
        c.insert(QStringLiteral("code"), t.code);
        c.insert(QStringLiteral("user"), t.userId);
        c.insert(QStringLiteral("expires_at_ms"), t.expiresAtMs);
        c.insert(QStringLiteral("used"), t.used);
        codes.append(c);
    }
    root.insert(QStringLiteral("temp_codes"), codes);

    QJsonObject policy;
    policy.insert(QStringLiteral("max_attempts"), m_maxAttempts);
    policy.insert(QStringLiteral("lockout_ms"), m_lockoutMs);
    root.insert(QStringLiteral("policy"), policy);

    // 版本号：将来格式变更时用于识别与迁移。
    root.insert(QStringLiteral("version"), 1);
    return root;
}

bool CredentialStore::fromJson(const QJsonObject& obj)
{
    if (obj.isEmpty()) {
        return false;
    }

    QVector<QString> ids;
    QVector<UserRecord> recs;
    const QJsonArray users = obj.value(QStringLiteral("users")).toArray();
    for (int i = 0; i < users.size(); ++i) {
        const QJsonObject u = users.at(i).toObject();
        const QString id = u.value(QStringLiteral("id")).toString();
        const QString hash = u.value(QStringLiteral("hash")).toString();
        if (id.isEmpty() || !looksLikeHash(hash)) {
            // 单条记录损坏不该让整份凭据失效 —— 但那也**不能**被当成
            // "该用户没有口令"而放行；跳过它，让它继续无法通过校验。
            qWarning() << "[凭据] 跳过一条无法解析的用户记录（id 非空但哈希非法）";
            continue;
        }
        const QStringList parts = hash.split(QLatin1Char(':'));
        UserRecord r;
        r.salt = hexToBytes(parts.at(1));
        r.digest = parts.at(3);
        bool ok = false;
        r.iterations = qBound(kMinIterations, parts.at(2).toInt(&ok), kMaxIterations);
        if (!ok) {
            r.iterations = kDefaultIterations;
        }
        r.failed = u.value(QStringLiteral("failed")).toInt(0);
        r.lockedUntilMs = static_cast<qint64>(
            u.value(QStringLiteral("locked_until_ms")).toDouble(0));
        ids.append(id);
        recs.append(r);
    }

    QVector<TempCode> codes;
    const QJsonArray arr = obj.value(QStringLiteral("temp_codes")).toArray();
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonObject c = arr.at(i).toObject();
        TempCode t;
        t.code = c.value(QStringLiteral("code")).toString();
        t.userId = c.value(QStringLiteral("user")).toString();
        t.expiresAtMs = static_cast<qint64>(
            c.value(QStringLiteral("expires_at_ms")).toDouble(0));
        t.used = c.value(QStringLiteral("used")).toBool(false);
        if (!t.code.isEmpty()) {
            codes.append(t);
        }
    }

    const QJsonObject policy = obj.value(QStringLiteral("policy")).toObject();
    m_maxAttempts = policy.value(QStringLiteral("max_attempts")).toInt(m_maxAttempts);
    if (m_maxAttempts < 0) {
        m_maxAttempts = 0;
    }
    setLockoutMs(policy.value(QStringLiteral("lockout_ms")).toInt(m_lockoutMs));

    m_userIds = ids;
    m_users = recs;
    m_tempCodes = codes;
    return true;
}

// ---------------------------------------------------------------------------
// 磁盘读写
// ---------------------------------------------------------------------------

bool CredentialStore::load()
{
    if (!isPersistent()) {
        return false;
    }
    QFile f(m_storagePath);
    if (!f.exists()) {
        // 首次启动的正常情形，不是错误。
        return false;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        // 存在但打不开：这是**真问题**（权限/被占用），必须告警 ——
        // 否则会静默退回"没有凭据"，表现成"门忽然打不开"。
        qWarning() << "[凭据] 存储文件无法打开:" << m_storagePath
                   << f.errorString() << "（将保持内存中的现有凭据）";
        return false;
    }
    const QByteArray raw = f.readAll();
    f.close();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // **关键**：解析失败时保持内存内容不变。若在这里清空，
        // 门禁会突然"接受任意口令"或"全部拒绝"，两种都是现场事故。
        qWarning() << "[凭据] 存储文件解析失败:" << m_storagePath
                   << err.errorString() << "（保持内存中的现有凭据，不覆盖）";
        return false;
    }
    return fromJson(doc.object());
}

bool CredentialStore::save() const
{
    if (!isPersistent()) {
        return false;
    }
    const QFileInfo info(m_storagePath);
    QDir dir(info.absolutePath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        qWarning() << "[凭据] 无法创建存储目录:" << info.absolutePath();
        return false;
    }

    QSaveFile f(m_storagePath);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "[凭据] 存储文件无法写入:" << m_storagePath << f.errorString();
        return false;
    }
    const QJsonDocument doc(toJson());
    if (f.write(doc.toJson(QJsonDocument::Indented)) < 0) {
        qWarning() << "[凭据] 写入失败:" << m_storagePath << f.errorString();
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        // commit() 失败 = 原子替换没发生，原文件**未被破坏**（QSaveFile 语义）。
        qWarning() << "[凭据] 原子提交失败:" << m_storagePath << f.errorString();
        return false;
    }

    // 凭据文件含口令哈希，收紧到"仅属主可读写"。
    // 注意：QFile::setPermissions 在部分嵌入式文件系统上可能无效，
    // 所以这里**只是加固**，不把失败当致命错误。
    QFile::setPermissions(m_storagePath,
                          QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

} // namespace accessterminal
