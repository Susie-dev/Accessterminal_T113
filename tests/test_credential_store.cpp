/**
 * @file test_credential_store.cpp
 * @brief CredentialStore 的行为断言（第 41 轮，方案 P2）。
 *
 * 为什么这些断言必须存在
 * ----------------------
 * 本类的**全部意义**是"口令不以明文留存、比对不泄漏时序、锁定可到期"。
 * 这类性质**无法**靠读代码确认，也不该只靠一条静态正则：
 *
 *   - 「文件里没有明文」是一条**否定性**断言 —— 它极其容易被写成恒真
 *     （比如文件根本没写成功、或读错了路径）。因此每条"不含明文"都配
 *     一条**阳性对照**："文件确实存在、确实非空、确实能被解析，
 *     且里面的哈希**确实能验证正确口令**"。
 *   - 「锁定会到期」如果用 sleep 来测，测试会慢且**不确定**
 *     （README 第 14 条：时序断言要等性质，不要等固定时长）。
 *     本类提供时钟注入，过期用**确定性**方式验证。
 *   - 「相同口令两次哈希不同」是盐起作用**唯一**的直接证据。
 *
 * 判据取向：断言落在**可观测输出**上（返回的 outcome、文件内容、
 * 内存中留下的字符串），不断言私有成员。
 *
 * 依赖：只用 Qt5::Core + Qt5::Test，不需要硬件、ALSA、OpenCV。
 */
#include <QtTest/QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "managers/credential_store.h"

using accessterminal::CredentialStore;
using accessterminal::VerifyOutcome;

namespace {

/// 固定用户标识，避免各用例拼错。
const char* const kUser = "door";

/**
 * @brief 从 toJson() 里取出某个用户的哈希串（测试用）。
 *
 * 刻意**不给生产代码加 test-only 取值接口**：`toJson()` 本来就是落盘契约的
 * 一部分（"文件里到底写了什么"），用它当观测点等于顺便把"盐确实进了
 * 落盘内容"也一起验证了。若哪天 toJson() 不再输出盐，这里会直接空串失败。
 *
 * 返回空串表示该用户不存在或没有哈希。
 */
QString storedHashFromJson(const CredentialStore& store, const QString& userId)
{
    const QJsonObject root = store.toJson();
    const QJsonArray users = root.value(QStringLiteral("users")).toArray();
    for (int i = 0; i < users.size(); ++i) {
        const QJsonObject u = users.at(i).toObject();
        if (u.value(QStringLiteral("id")).toString() == userId) {
            return u.value(QStringLiteral("hash")).toString();
        }
    }
    return QString();
}

} // namespace

class TestCredentialStore : public QObject
{
    Q_OBJECT

private slots:

    void initTestCase()
    {
        // 本目标不建窗口，但 QTest 仍可能初始化 GUI 相关设施。
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    // =======================================================================
    // 哈希格式与识别
    // =======================================================================

    /**
     * 哈希串必须是 `sha256:<salt-hex>:<iterations>:<digest-hex>` 且各段长度正确。
     *
     * 格式是**落盘契约**：一旦变了，老凭据文件就读不出来（门开不了）。
     * 所以它值得被逐段钉住，而不是只断言"包含 sha256"。
     */
    void hashStringHasDocumentedShape()
    {
        const QByteArray salt = CredentialStore::makeSalt();
        const QString h = CredentialStore::hashSecret(
            QStringLiteral("5788"), salt, 1000);

        const QStringList parts = h.split(QLatin1Char(':'));
        QCOMPARE(parts.size(), 4);
        QCOMPARE(parts.at(0), QStringLiteral("sha256"));
        // 盐：16 字节 -> 32 个 hex 字符
        QCOMPARE(parts.at(1).size(), CredentialStore::kSaltBytes * 2);
        QCOMPARE(parts.at(2), QStringLiteral("1000"));
        // 摘要：SHA-256 -> 32 字节 -> 64 个 hex 字符
        QCOMPARE(parts.at(3).size(), 64);

        QVERIFY(CredentialStore::looksLikeHash(h));
    }

    /// 明文、空串、以及各种"看起来像但不是"的串都不能被当成哈希。
    void plaintextIsNotMistakenForHash()
    {
        QVERIFY(!CredentialStore::looksLikeHash(QStringLiteral("5788")));
        QVERIFY(!CredentialStore::looksLikeHash(QString()));
        QVERIFY(!CredentialStore::looksLikeHash(QStringLiteral("sha256:aa:1:bb")));
        // 段数不对
        QVERIFY(!CredentialStore::looksLikeHash(QStringLiteral("sha256:aa:1")));
        QVERIFY(!CredentialStore::looksLikeHash(QStringLiteral("md5:aa:1:bb")));
        // 迭代数不是数字
        QVERIFY(!CredentialStore::looksLikeHash(
            QStringLiteral("sha256:") + QString(32, QLatin1Char('a'))
            + QStringLiteral(":abc:") + QString(64, QLatin1Char('b'))));
    }

    /**
     * **每用户独立盐**：同一口令在不同盐下必须得到不同摘要。
     *
     * 这是"加盐"唯一有意义的直接证据。若哪天有人图省事改成全局固定盐，
     * 本用例会红 —— 而"两人同密码"就可以被一眼看出（正是盐要防的）。
     */
    void samePlaintextWithDifferentSaltsGivesDifferentDigests()
    {
        const QByteArray s1 = CredentialStore::makeSalt();
        const QByteArray s2 = CredentialStore::makeSalt();
        QVERIFY2(s1 != s2, "两次取盐不应相同（否则随机源坏了）");

        const QString h1 = CredentialStore::hashSecret(QStringLiteral("5788"), s1, 1000);
        const QString h2 = CredentialStore::hashSecret(QStringLiteral("5788"), s2, 1000);
        QVERIFY2(h1 != h2, "相同口令 + 不同盐必须得到不同摘要");

        // 阳性对照：同一口令 + **同一**盐必须可复现（否则哈希本身不确定，
        // 上面的"不同"就没有意义了）。
        const QString h1b = CredentialStore::hashSecret(QStringLiteral("5788"), s1, 1000);
        QCOMPARE(h1, h1b);
    }

    /**
     * **setSecret() 内部必须取随机盐**（两个用户同口令 -> 不同哈希）。
     *
     * 为什么单独一条：上面那条用的是 `hashSecret(plain, salt, n)` ——
     * 盐是**测试自己**传进去的，所以它只能证明"哈希函数用盐"，
     * **证明不了** `setSecret()` 会去取随机盐。故障注入实测（第 41 轮）：
     * 把 `setSecret()` 里的 `makeSalt()` 换成固定盐，上面那条断言**照样绿**。
     *
     * 落盘格式里盐是可见的，所以"两人同密码、文件里哈希相同"是运维能
     * 一眼看出的泄漏 —— 正是加盐要防的第一个场景。
     */
    void setSecretUsesAFreshSaltEachTime()
    {
        CredentialStore store;
        store.setSecret(QStringLiteral("alice"), QStringLiteral("same-pw"));
        store.setSecret(QStringLiteral("bob"), QStringLiteral("same-pw"));

        const QString ha = storedHashFromJson(store, QStringLiteral("alice"));
        const QString hb = storedHashFromJson(store, QStringLiteral("bob"));
        QVERIFY2(!ha.isEmpty(), "alice 的哈希不应为空");
        QVERIFY2(!hb.isEmpty(), "bob 的哈希不应为空");
        QVERIFY2(ha != hb,
                 "同一口令在两个用户下得到相同哈希 —— 盐没有随机化，"
                 "等于没加盐（相同口令会互相暴露）");

        // 阳性对照：两人各自的正确口令仍都能通过。
        QCOMPARE(store.verify(QStringLiteral("alice"), QStringLiteral("same-pw")),
                 VerifyOutcome::Accepted);
        QCOMPARE(store.verify(QStringLiteral("bob"), QStringLiteral("same-pw")),
                 VerifyOutcome::Accepted);

        // 同一用户重新设同一口令，盐也必须换新（否则重设口令可被察觉）。
        store.setSecret(QStringLiteral("alice"), QStringLiteral("same-pw"));
        QVERIFY2(storedHashFromJson(store, QStringLiteral("alice")) != ha,
                 "重设口令后哈希未变 —— 盐没有换新");
    }

    /**
     * **锁定期满后失败计数从零重新开始**，而不是接着旧的计数。
     *
     * 这是"到期自动解锁"那段清理逻辑**唯一可观测**的效果
     * （第 41 轮故障注入发现）：
     *   `isLockedOut()` 由 `lockedUntilMs - nowMs()` 算出来，时间一过
     *   自然就返回 false —— 所以删掉"清零"那段代码，`isLockedOut()` 与
     *   "正口令能过"**都照旧通过**。也就是说那段代码看着像摆设。
     *
     * 但它在**同一个 maxAttempts 下再错一次**时是可观测的：
     *   - 有清零：failed 从 0 开始 -> 变 1 -> 不锁；
     *   - 没清零：failed 接着旧值 -> 立刻 >= 上限 -> 又锁上。
     * 后者意味着"刚解禁就被一次手误重新锁死"，现场体验是"门再也开不开"。
     */
    void failureCountRestartsAfterLockoutExpires()
    {
        CredentialStore store;
        store.setMaxAttempts(3);
        store.setLockoutMs(1000);
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("pw"));

        store.setNowMsForTest(2000000);
        for (int i = 0; i < 3; ++i) {
            store.verify(QString::fromLatin1(kUser), QStringLiteral("bad"));
        }
        QVERIFY(store.isLockedOut(QString::fromLatin1(kUser)));
        QCOMPARE(store.failedAttempts(QString::fromLatin1(kUser)), 3);

        // 锁定期满
        store.setNowMsForTest(2001000);
        // 注意：`isLockedOut()` 是按 `lockedUntilMs - nowMs()` **算出来**的，
        // 时间一过它自然为 false；此时 `failedAttempts()` 仍报旧值 3 ——
        // 清零发生在**下一次 verify()** 里（惰性复位），不是时钟一走就复位。
        // 初版测试在这里断言 `failedAttempts() == 0` 而失败，是我把
        // "惰性复位"当成了"即时复位"。真正要守的性质在下面。
        QVERIFY(!store.isLockedOut(QString::fromLatin1(kUser)));

        // 期满后再错**一次**：计数必须从 1 起，不能被旧计数立刻再锁。
        // 这一条才是"到期清零"唯一可观测的效果：
        //   - 有清零 -> failed 变 1 -> 不锁；
        //   - 没清零 -> failed 变 4 -> 立刻 >= 上限 -> 又锁上。
        // 后者现场表现是"刚解禁就被一次手误重新锁死"。
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("bad")),
                 VerifyOutcome::Rejected);
        QCOMPARE(store.failedAttempts(QString::fromLatin1(kUser)), 1);
        QVERIFY2(!store.isLockedOut(QString::fromLatin1(kUser)),
                 "锁定期满后一次手误就把门重新锁死 —— 计数没有从零开始");

        // 阳性对照：这次错完之后正口令仍然能开门。
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("pw")),
                 VerifyOutcome::Accepted);
    }

    /// 迭代次数改变必须改变摘要（否则"迭代"是装饰）。
    void iterationCountAffectsDigest()
    {
        const QByteArray salt = CredentialStore::makeSalt();
        const QString a = CredentialStore::hashSecret(QStringLiteral("x"), salt, 1);
        const QString b = CredentialStore::hashSecret(QStringLiteral("x"), salt, 2);
        QVERIFY(a != b);
    }

    /// 迭代次数被夹到 [kMinIterations, kMaxIterations]。
    void iterationCountIsClamped()
    {
        const QByteArray salt = CredentialStore::makeSalt();
        const QString tooBig = CredentialStore::hashSecret(
            QStringLiteral("x"), salt, 99999999);
        const QString capped = CredentialStore::hashSecret(
            QStringLiteral("x"), salt, CredentialStore::kMaxIterations);
        QCOMPARE(tooBig, capped);

        const QString zero = CredentialStore::hashSecret(QStringLiteral("x"), salt, 0);
        const QString one = CredentialStore::hashSecret(QStringLiteral("x"), salt, 1);
        QCOMPARE(zero, one);
    }

    // =======================================================================
    // 校验
    // =======================================================================

    /// 正确口令通过、错误口令拒绝、未设口令一律拒绝。
    void verifyAcceptsOnlyTheRightSecret()
    {
        CredentialStore store;
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("5788")),
                 VerifyOutcome::Rejected);       // 还没设

        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("5788"));
        QVERIFY(store.hasSecret(QString::fromLatin1(kUser)));
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("5788")),
                 VerifyOutcome::Accepted);
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("5789")),
                 VerifyOutcome::Rejected);
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("")),
                 VerifyOutcome::Rejected);
    }

    /// 未知用户被拒绝（不能因为"没这个用户"就放行）。
    void unknownUserIsRejectedNotAccepted()
    {
        CredentialStore store;
        store.setSecret(QStringLiteral("alice"), QStringLiteral("pw"));
        QCOMPARE(store.verify(QStringLiteral("bob"), QStringLiteral("pw")),
                 VerifyOutcome::Rejected);
        QVERIFY(!store.hasSecret(QStringLiteral("bob")));
    }

    /// 换口令后旧口令立即失效、新口令生效。
    void changingSecretInvalidatesTheOldOne()
    {
        CredentialStore store;
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("old-pw"));
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("old-pw")),
                 VerifyOutcome::Accepted);

        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("new-pw"));
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("old-pw")),
                 VerifyOutcome::Rejected);
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("new-pw")),
                 VerifyOutcome::Accepted);
    }

    /// clearSecret 之后该用户无法通过。
    void clearedSecretCannotBeVerified()
    {
        CredentialStore store;
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("pw"));
        store.clearSecret(QString::fromLatin1(kUser));
        QVERIFY(!store.hasSecret(QString::fromLatin1(kUser)));
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("pw")),
                 VerifyOutcome::Rejected);
    }

    /// setHashedSecret 拒绝非法格式，并保持原有凭据不变。
    void setHashedSecretRejectsGarbage()
    {
        CredentialStore store;
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("keep-me"));
        QVERIFY(!store.setHashedSecret(QString::fromLatin1(kUser),
                                       QStringLiteral("not-a-hash")));
        // 阳性对照：原有口令必须仍然有效（证明上面那次失败没破坏状态）。
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("keep-me")),
                 VerifyOutcome::Accepted);
    }

    /**
     * **非十六进制字符必须让哈希串被拒**，不能被"解码成功"蒙过去。
     *
     * 为什么单独一条（第 41 轮故障注入发现）：`QByteArray::fromHex()` 遇到
     * 非法字符**不报错**，而是跳过它们、把剩下的重新配对 ——
     * `"FD0G…"` 这种串会"成功"解出一段**内容全错**的字节。
     * 于是把 `hexToBytes()` 里的十六进制校验整段删掉，只靠
     * `salt.size() == kSaltBytes` 也**照样能过**（长度同样凑得出），
     * 测试全绿。必须**专门**钉住"含非法字符即拒"。
     *
     * 构造要点：长度恰好正确，只有字符非法 —— 这样唯一能拒绝它的理由
     * 就是"逐字符校验"，而不是长度。
     */
    void hashWithNonHexCharactersIsRejected()
    {
        CredentialStore store;
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("keep-me"));

        // 构造要点（第一版想错了，靠故障注入才发现）：
        // 必须让非法字符是**多余**的，而不是替换掉合法字符。
        //   - 若把 'a' 换成 'g'（长度不变），fromHex 跳过 'g' 后只剩 31 个
        //     合法字符 -> 15 字节 -> 被**长度**判据拒绝。于是把字符校验删掉，
        //     测试**照样绿**（假绿）—— 长度判据掩盖了字符判据。
        //   - 正确构造：32 个合法字符 **加上** 2 个非法字符 = 34 字符。
        //     fromHex 跳过非法字符后正好剩 32 个 -> 16 字节 -> 长度判据通过。
        //     此时**唯一**能拒绝它的理由就是逐字符校验。
        const QString validSalt = QString(32, QLatin1Char('a'));
        const QString validDigest = QString(64, QLatin1Char('b'));
        const QString badSalt = validSalt + QStringLiteral("gz");
        const QString badDigest = validDigest + QStringLiteral("XY");
        const QString forged = QStringLiteral("sha256:") + badSalt
                               + QStringLiteral(":1000:") + badDigest;

        // 自证构造有效：跳过非法字符后**恰好**是合法长度。
        QCOMPARE(badSalt.size(), CredentialStore::kSaltBytes * 2 + 2);
        QCOMPARE(badDigest.size(), 64 + 2);
        QCOMPARE(QByteArray::fromHex(validSalt.toLatin1()).size(),
                 CredentialStore::kSaltBytes);
        QCOMPARE(QByteArray::fromHex(badSalt.toLatin1()).size(),
                 CredentialStore::kSaltBytes);   // 非法字符被跳过，长度仍对得上
        QCOMPARE(QByteArray::fromHex(badDigest.toLatin1()).size(), 32);

        QVERIFY2(!CredentialStore::looksLikeHash(forged),
                 "含非十六进制字符的哈希串被当成了合法格式 —— "
                 "fromHex 会跳过非法字符并给出内容全错的字节，"
                 "而长度判据**拦不住**它");
        QVERIFY2(!store.setHashedSecret(QString::fromLatin1(kUser), forged),
                 "含非十六进制字符的哈希串被接受了");

        // 阳性对照 1：原口令仍在（拒绝没有破坏状态）。
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("keep-me")),
                 VerifyOutcome::Accepted);

        // 阳性对照 2：**完全合法**的哈希串是会被接受的 ——
        // 否则上面两条"被拒"可能只是因为格式整体不对，而不是因为字符非法。
        const QByteArray salt = CredentialStore::makeSalt();
        const QString good = CredentialStore::hashSecret(QStringLiteral("ok-pw"),
                                                        salt, 1000);
        QVERIFY2(CredentialStore::looksLikeHash(good), "合法哈希串竟不被识别");
        QVERIFY2(store.setHashedSecret(QString::fromLatin1(kUser), good),
                 "合法哈希串竟被拒绝");
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("ok-pw")),
                 VerifyOutcome::Accepted);
    }

    /// 装入哈希串后可以用对应明文通过 —— 这是"从文件恢复凭据"的基本前提。
    void setHashedSecretRoundTrips()
    {
        const QByteArray salt = CredentialStore::makeSalt();
        const QString h = CredentialStore::hashSecret(QStringLiteral("boot-pw"), salt, 512);

        CredentialStore store;
        QVERIFY(store.setHashedSecret(QString::fromLatin1(kUser), h));
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("boot-pw")),
                 VerifyOutcome::Accepted);
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("boot-pW")),
                 VerifyOutcome::Rejected);
    }

    // =======================================================================
    // 定长比较
    // =======================================================================

    /// constantTimeEquals 的语义必须与普通相等一致（否则会出现"漏判"）。
    void constantTimeEqualsMatchesOrdinaryEquality()
    {
        QVERIFY(CredentialStore::constantTimeEquals(QStringLiteral("abc"),
                                                    QStringLiteral("abc")));
        QVERIFY(!CredentialStore::constantTimeEquals(QStringLiteral("abc"),
                                                     QStringLiteral("abd")));
        QVERIFY(!CredentialStore::constantTimeEquals(QStringLiteral("abc"),
                                                     QStringLiteral("ab")));
        QVERIFY(CredentialStore::constantTimeEquals(QString(), QString()));
        // 首字符就不同 —— 定长比较也必须给出 false（不能因"提前返回"而漏判）。
        QVERIFY(!CredentialStore::constantTimeEquals(QStringLiteral("zbc"),
                                                     QStringLiteral("abc")));
    }

    // =======================================================================
    // 锁定策略（默认关闭 = 保持现状）
    // =======================================================================

    /// **默认不锁定**：这是"不改变现场行为"的关键断言。
    void lockoutIsDisabledByDefault()
    {
        CredentialStore store;
        QCOMPARE(store.maxAttempts(), 0);
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("pw"));

        // 连错 50 次，计数照涨（可观测），但**永不**进入 LockedOut。
        for (int i = 0; i < 50; ++i) {
            QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("bad")),
                     VerifyOutcome::Rejected);
        }
        QCOMPARE(store.failedAttempts(QString::fromLatin1(kUser)), 50);
        QVERIFY(!store.isLockedOut(QString::fromLatin1(kUser)));

        // 正口令仍然照常通过 —— 现场行为与重构前一致。
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("pw")),
                 VerifyOutcome::Accepted);
        QCOMPARE(store.failedAttempts(QString::fromLatin1(kUser)), 0);
    }

    /// 启用后：达到上限即锁定，锁定期内返回 LockedOut 且**不再累加**计数。
    void reachingMaxAttemptsLocksOut()
    {
        CredentialStore store;
        store.setMaxAttempts(3);
        store.setLockoutMs(60000);
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("pw"));

        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("bad")),
                 VerifyOutcome::Rejected);
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("bad")),
                 VerifyOutcome::Rejected);
        QCOMPARE(store.failedAttempts(QString::fromLatin1(kUser)), 2);
        QVERIFY(!store.isLockedOut(QString::fromLatin1(kUser)));

        // 第 3 次达到上限 -> 本次仍是 Rejected，但从此锁定。
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("bad")),
                 VerifyOutcome::Rejected);
        QVERIFY(store.isLockedOut(QString::fromLatin1(kUser)));

        // 锁定期内：**连正确口令也被拒**（这是锁定应有的语义）。
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("pw")),
                 VerifyOutcome::LockedOut);
        // 且计数不再增长（避免锁定期被用来把计数刷爆）。
        QCOMPARE(store.failedAttempts(QString::fromLatin1(kUser)), 3);
    }

    /**
     * 锁定**到期自动解锁**，用时钟注入确定性验证。
     *
     * 不用 sleep：那会让测试变慢且不确定（机器负载一高就可能 flaky）。
     */
    void lockoutExpiresAutomatically()
    {
        CredentialStore store;
        store.setMaxAttempts(1);
        store.setLockoutMs(1000);
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("pw"));

        store.setNowMsForTest(1000000);
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("bad")),
                 VerifyOutcome::Rejected);
        QVERIFY(store.isLockedOut(QString::fromLatin1(kUser)));
        QCOMPARE(store.lockoutRemainingMs(QString::fromLatin1(kUser)), 1000);

        // 差 1ms：仍然锁定。
        store.setNowMsForTest(1000999);
        QVERIFY(store.isLockedOut(QString::fromLatin1(kUser)));
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("pw")),
                 VerifyOutcome::LockedOut);

        // 正好到期：解锁，且正口令可用、计数清零。
        store.setNowMsForTest(1001000);
        QVERIFY(!store.isLockedOut(QString::fromLatin1(kUser)));
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("pw")),
                 VerifyOutcome::Accepted);
        QCOMPARE(store.failedAttempts(QString::fromLatin1(kUser)), 0);
    }

    /// 成功一次即清零失败计数（否则偶发手误会累积成锁定）。
    void successResetsFailureCount()
    {
        CredentialStore store;
        store.setMaxAttempts(5);
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("pw"));

        store.verify(QString::fromLatin1(kUser), QStringLiteral("bad"));
        store.verify(QString::fromLatin1(kUser), QStringLiteral("bad"));
        QCOMPARE(store.failedAttempts(QString::fromLatin1(kUser)), 2);

        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("pw")),
                 VerifyOutcome::Accepted);
        QCOMPARE(store.failedAttempts(QString::fromLatin1(kUser)), 0);
        QVERIFY(!store.isLockedOut(QString::fromLatin1(kUser)));
    }

    /// 锁定时长被夹到 [0, 24h]，防止配置失误把门永久锁死。
    void lockoutDurationIsClamped()
    {
        CredentialStore store;
        store.setLockoutMs(-5);
        QCOMPARE(store.lockoutMs(), 0);

        store.setLockoutMs(999999999);
        QCOMPARE(store.lockoutMs(), 24 * 60 * 60 * 1000);
    }

    /// 负的 maxAttempts 按 0（=不锁定）处理，不是"每次都锁"。
    void negativeMaxAttemptsMeansDisabled()
    {
        CredentialStore store;
        store.setMaxAttempts(-3);
        QCOMPARE(store.maxAttempts(), 0);
    }

    // =======================================================================
    // 临时访客码（只做存储层）
    // =======================================================================

    /// 签发 -> 消费一次成功 -> 第二次失败（**一次性**）。
    void tempCodeIsSingleUse()
    {
        CredentialStore store;
        store.setNowMsForTest(500000);
        const QString code = store.issueTempCode(QStringLiteral("visitor"), 60000);
        QVERIFY(!code.isEmpty());
        QCOMPARE(code.size(), 6);
        // 必须是纯数字（现场靠键盘输入）。
        for (int i = 0; i < code.size(); ++i) {
            QVERIFY(code.at(i).isDigit());
        }
        QCOMPARE(store.activeTempCodeCount(), 1);

        QVERIFY(store.consumeTempCode(code));
        QVERIFY(!store.consumeTempCode(code));      // 已用
        QCOMPARE(store.activeTempCodeCount(), 0);
    }

    /// 过期后不能消费；到期边界要精确。
    void tempCodeExpires()
    {
        CredentialStore store;
        store.setNowMsForTest(700000);
        const QString code = store.issueTempCode(QStringLiteral("visitor"), 1000);
        QVERIFY(!code.isEmpty());

        // 差 1ms：仍然有效。
        store.setNowMsForTest(700999);
        QVERIFY(store.consumeTempCode(code));

        // 另发一个码，**在同一个基准时刻**签发，这样它的到期时刻是确定的
        // 701000。第一版测试在 700999 时刻才签发它，于是到期时刻被顺延成
        // 701999，而断言仍按 701000 判 —— 那是**测试自己算错了**，
        // 不是实现错。这里把基准时刻挪回来，让边界是精确的 701000。
        store.setNowMsForTest(700000);
        const QString code2 = store.issueTempCode(QStringLiteral("visitor"), 1000);
        QVERIFY(!code2.isEmpty());

        store.setNowMsForTest(700999);
        QVERIFY2(store.consumeTempCode(code2), "未到期的码必须可用");

        // 再来一个，验证"正好到期即失效"（<= 语义）。
        store.setNowMsForTest(700000);
        const QString code3 = store.issueTempCode(QStringLiteral("visitor"), 1000);
        store.setNowMsForTest(701000);
        QVERIFY2(!store.consumeTempCode(code3), "正好到期的码必须失效");
    }

    /// 不存在的码不能消费（否定性断言 + 阳性对照）。
    void unknownTempCodeIsRejected()
    {
        CredentialStore store;
        store.setNowMsForTest(800000);
        const QString good = store.issueTempCode(QStringLiteral("visitor"), 60000);
        // 阳性对照：先证明这个环境里"能消费"是可达的。
        QVERIFY(store.consumeTempCode(good));
        // 真断言：乱编的码不行。
        QVERIFY(!store.consumeTempCode(QStringLiteral("000000")));
        QVERIFY(!store.consumeTempCode(QString()));
    }

    /// purgeTempCodes 只清已用/已过期，未到期的保留。
    void purgeRemovesOnlyDeadCodes()
    {
        CredentialStore store;
        store.setNowMsForTest(900000);
        const QString live = store.issueTempCode(QStringLiteral("a"), 60000);
        const QString dying = store.issueTempCode(QStringLiteral("b"), 100);
        QVERIFY(store.consumeTempCode(store.issueTempCode(QStringLiteral("c"), 60000)));

        store.setNowMsForTest(900200);              // dying 已过期
        const int removed = store.purgeTempCodes();
        QVERIFY(removed >= 2);                      // 已用的 + 过期的
        QCOMPARE(store.activeTempCodeCount(), 1);
        QVERIFY(!store.consumeTempCode(dying));
        QVERIFY(store.consumeTempCode(live));       // 阳性对照：活的还在
    }

    // =======================================================================
    // 持久化：明文绝不落盘
    // =======================================================================

    /**
     * 落盘文件里**不得出现明文口令**，同时必须能读回来开门。
     *
     * 这条是本文件最重要的一条。写法上刻意做成"阳性对照 + 否定断言"：
     *   1. 文件存在、非空、是合法 JSON（否则下面的"不含"恒真）；
     *   2. 文件里的哈希**确实能验证正确口令**（证明存的是有用凭据）；
     *   3. 文件字节里**不含**明文口令，也不含它的任何非空前缀，
     *      也不含其 hex。
     */
    void persistedFileContainsNoPlaintext()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("credentials.json"));

        const QString secret = QStringLiteral("Zq7-persist-sentinel-4713");
        {
            CredentialStore store(path);
            store.setSecret(QString::fromLatin1(kUser), secret);
            QVERIFY2(store.save(), "保存失败，后面的断言将失去意义");
        }

        QFile f(path);
        QVERIFY2(f.exists(), "凭据文件未生成");
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray raw = f.readAll();
        f.close();

        // ---- 阳性对照 1：文件非空且是合法 JSON ----
        QVERIFY2(!raw.isEmpty(), "凭据文件是空的");
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        QVERIFY2(err.error == QJsonParseError::NoError,
                 qPrintable(QStringLiteral("凭据文件不是合法 JSON: ")
                            + err.errorString()));

        // ---- 阳性对照 2：读回来真的能用这个口令通过 ----
        {
            CredentialStore store2(path);
            QVERIFY2(store2.load(), "凭据文件读不回来");
            QVERIFY(store2.hasSecret(QString::fromLatin1(kUser)));
            QCOMPARE(store2.verify(QString::fromLatin1(kUser), secret),
                     VerifyOutcome::Accepted);
            // 且错口令仍然被拒（证明不是"读失败导致谁都通过"）。
            QCOMPARE(store2.verify(QString::fromLatin1(kUser), QStringLiteral("nope")),
                     VerifyOutcome::Rejected);
        }

        // ---- 真断言：明文（及其前缀、hex）绝不出现 ----
        const QString text = QString::fromUtf8(raw);
        QVERIFY2(!text.contains(secret),
                 qPrintable(QStringLiteral("凭据文件里出现了明文口令: ") + text));
        for (int n = 1; n <= secret.size(); ++n) {
            QVERIFY2(!text.contains(secret.left(n)),
                     qPrintable(QStringLiteral("凭据文件里出现了口令前缀(%1): %2")
                                    .arg(n).arg(text)));
        }
        QVERIFY2(!text.contains(QString::fromLatin1(secret.toUtf8().toHex())),
                 qPrintable(QStringLiteral("凭据文件里出现了口令 hex: ") + text));

        // 文件里应当能看见 sha256 标记（阳性对照 3：确实是本类的格式）。
        QVERIFY(text.contains(QStringLiteral("sha256")));
    }

    /// 保存 -> 读取 往返后，凭据、迭代数、锁定状态都一致。
    void saveLoadRoundTripsState()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("c.json"));

        const QByteArray salt = CredentialStore::makeSalt();
        const QString h = CredentialStore::hashSecret(QStringLiteral("round-pw"),
                                                     salt, 777);
        {
            CredentialStore store(path);
            QVERIFY(store.setHashedSecret(QString::fromLatin1(kUser), h));
            store.setMaxAttempts(7);
            store.setLockoutMs(12345);
            QVERIFY(store.save());
        }
        {
            CredentialStore store(path);
            QVERIFY(store.load());
            QCOMPARE(store.maxAttempts(), 7);
            QCOMPARE(store.lockoutMs(), 12345);
            QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("round-pw")),
                     VerifyOutcome::Accepted);
        }
    }

    /// 临时码也要能跨启动保留（否则重启后访客码失效，现场会困惑）。
    void saveLoadRoundTripsTempCodes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("c.json"));

        QString code;
        {
            CredentialStore store(path);
            store.setNowMsForTest(1000000);
            code = store.issueTempCode(QStringLiteral("visitor"), 600000);
            QVERIFY(!code.isEmpty());
            QVERIFY(store.save());
        }
        {
            CredentialStore store(path);
            store.setNowMsForTest(1000001);
            QVERIFY(store.load());
            QCOMPARE(store.activeTempCodeCount(), 1);
            QVERIFY2(store.consumeTempCode(code), "临时码应当跨重启仍然有效");
        }
    }

    /// 路径为空 = 纯内存：不读、不写、不崩。
    void emptyPathIsPureMemory()
    {
        CredentialStore store;
        QVERIFY(!store.isPersistent());
        QVERIFY(!store.save());         // 无路径 -> 明确返回 false
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("mem-pw"));
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("mem-pw")),
                 VerifyOutcome::Accepted);
    }

    /// 文件不存在时 load() 返回 false 但不告警、不破坏内存内容。
    void missingFileLeavesStoreUsable()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("nope.json"));

        CredentialStore store(path);
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("still-here"));
        QVERIFY(!store.load());
        // 阳性对照：load 失败不该清掉内存里的凭据。
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("still-here")),
                 VerifyOutcome::Accepted);
    }

    /**
     * **损坏的 JSON 不得清空凭据。**
     *
     * 这是"门忽然打不开"的直接成因：如果解析失败时把内存清空，运行时就会
     * 从"能开门"变成"一律拒绝"（或更糟：接受任意口令）。本类刻意保持
     * 内存不变，本用例钉住它。
     */
    void corruptFileDoesNotWipeCredentials()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = QDir(dir.path()).filePath(QStringLiteral("bad.json"));

        CredentialStore store(path);
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("survivor"));

        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{ this is not json at all ");
        f.close();

        QVERIFY2(!store.load(), "损坏的 JSON 不应报告加载成功");
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("survivor")),
                 VerifyOutcome::Accepted);
    }

    /// JSON 往返（不落盘）保持语义一致。
    void jsonRoundTripPreservesSemantics()
    {
        CredentialStore a;
        a.setSecret(QString::fromLatin1(kUser), QStringLiteral("json-pw"));
        a.setMaxAttempts(2);
        a.setLockoutMs(5000);

        CredentialStore b;
        QVERIFY(b.fromJson(a.toJson()));
        QCOMPARE(b.maxAttempts(), 2);
        QCOMPARE(b.lockoutMs(), 5000);
        QCOMPARE(b.verify(QString::fromLatin1(kUser), QStringLiteral("json-pw")),
                 VerifyOutcome::Accepted);
    }

    /// 空 JSON 对象不被当成"有效但空"的凭据（那会静默清空门禁）。
    void emptyJsonIsRejected()
    {
        CredentialStore store;
        store.setSecret(QString::fromLatin1(kUser), QStringLiteral("keep"));
        QVERIFY(!store.fromJson(QJsonObject()));
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("keep")),
                 VerifyOutcome::Accepted);
    }

    /// 单条记录损坏时应跳过它，而不是让整份凭据失效。
    void oneCorruptUserSkipsOnlyThatUser()
    {
        CredentialStore good;
        good.setSecret(QStringLiteral("alice"), QStringLiteral("alice-pw"));

        QJsonObject root = good.toJson();
        QJsonArray users = root.value(QStringLiteral("users")).toArray();
        QJsonObject bad;
        bad.insert(QStringLiteral("id"), QStringLiteral("mallory"));
        bad.insert(QStringLiteral("hash"), QStringLiteral("garbage"));
        users.append(bad);
        root.insert(QStringLiteral("users"), users);

        CredentialStore store;
        QVERIFY(store.fromJson(root));
        // 好的那条仍然可用。
        QCOMPARE(store.verify(QStringLiteral("alice"), QStringLiteral("alice-pw")),
                 VerifyOutcome::Accepted);
        // 坏的那条**不能被放行**（不能因为解析失败就当"没密码"）。
        QVERIFY(!store.hasSecret(QStringLiteral("mallory")));
        QCOMPARE(store.verify(QStringLiteral("mallory"), QStringLiteral("garbage")),
                 VerifyOutcome::Rejected);
    }

    /// makeSalt 长度正确、且多次调用互不相同。
    void saltGenerationIsUsable()
    {
        const QByteArray a = CredentialStore::makeSalt();
        const QByteArray b = CredentialStore::makeSalt();
        QCOMPARE(a.size(), CredentialStore::kSaltBytes);
        QCOMPARE(b.size(), CredentialStore::kSaltBytes);
        QVERIFY2(a != b, "连续两次取盐相同 —— 随机源有问题，盐形同虚设");
    }

    /// 非 ASCII 口令必须原样可用（UTF-8 往返不能丢字节）。
    void nonAsciiSecretWorks()
    {
        CredentialStore store;
        const QString secret = QString::fromUtf8("门禁-口令-8842");
        store.setSecret(QString::fromLatin1(kUser), secret);
        QCOMPARE(store.verify(QString::fromLatin1(kUser), secret),
                 VerifyOutcome::Accepted);
        QCOMPARE(store.verify(QString::fromLatin1(kUser),
                              QString::fromUtf8("门禁-口令-8843")),
                 VerifyOutcome::Rejected);
    }

    /// 空口令可以设置并校验（`PasswordManager` 的历史语义依赖这一点）。
    void emptySecretIsRepresentable()
    {
        CredentialStore store;
        store.setSecret(QString::fromLatin1(kUser), QString());
        QVERIFY(store.hasSecret(QString::fromLatin1(kUser)));
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QString()),
                 VerifyOutcome::Accepted);
        QCOMPARE(store.verify(QString::fromLatin1(kUser), QStringLiteral("x")),
                 VerifyOutcome::Rejected);
    }
};

QTEST_MAIN(TestCredentialStore)
#include "test_credential_store.moc"
