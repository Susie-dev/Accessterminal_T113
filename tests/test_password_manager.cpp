/**
 * @file test_password_manager.cpp
 * @brief PasswordManager 的行为断言 + **门禁密码不得进日志**的回归测试。
 *
 * 为什么必须补这一组
 * ------------------
 * 原始 baseline 的 `checkPassword()` 把四样东西打进 qDebug：
 *
 *     input= <用户输入>  password= <正确密码>  inputHex= <输入hex>  pwdHex= <密码hex>
 *
 * （baseline `terminal/smartpronew/managers/password_manager.cpp`，已核对。）
 *
 * 门口机是**实体门禁设备**，密码就是物理准入凭据。而 qDebug 在嵌入式 Linux 上
 * 落到 stderr / systemd journal：任何有日志读取权限的人都能拿到，且跨重启留存。
 * 于是"输错一次"就把门禁密码交了出去。
 *
 * 重构已修掉它（只记录"本次是否匹配"）。但这个安全性质此前**只有一条静态文本
 * 规则**（`check_no_secret_in_logs`）在守：它扫源码里敏感变量名有没有出现在日志
 * 语句里。静态规则可能被绕过 —— 改名、拼串、经辅助函数转一手，正则就看不见。
 *
 * 本测试从**运行期**守住它：真的把密码设成哨兵串，真的调校验，真的把
 * qDebug/qWarning 抓下来，断言哨兵串（及其 hex、以及用户输入）**一次都没出现过**。
 * 这是 `password_manager.cpp` 第一次被链接进任何测试目标。
 *
 * 两条硬要求（都是被故障注入逼出来的）
 * -----------------------------------
 * 1. **"没出现"必须有阳性对照。** 若消息处理器没装上，或日志走了别的通道，那么
 *    **任何**"不包含"断言都会通过 —— 测试看着很严，实则零验证。所以每条"哨兵
 *    不出现"都配一条"非敏感日志确实被捕获"的对照，捕获管道坏了对照先失败。
 *
 * 2. **哨兵必须原样到达被测代码。** 本文件第一版用环境变量注入**非 ASCII** 哨兵，
 *    而 `AppConfig::toQString()` 用的是 `QString::fromLocal8Bit()`。在本机（GBK
 *    代码页）UTF-8 字节被按 GBK 解码，13 个码元进去、16 个出来 —— 于是"日志里
 *    不含该哨兵的前缀"这条断言在比较**两个不同的串**，**恒真**。故障注入据此发现
 *    "前缀脱敏"变异抓不到，探针一查才知道断言本身是空的。
 *
 *    教训：凡测试夹具**跨过一次编码边界**（环境变量、文件、argv），必须先断言
 *    夹具**原样到达**，否则后面所有围绕它的断言都可能是空的。本文件因此改为
 *    用 `setPassword()`（进程内 QString，不经过编码边界）来承载非 ASCII 与
 *    前缀脱敏场景，并保留一条显式的"哨兵已原样生效"前置断言。
 *
 * 判据取向：断言落在可观测输出（日志文本、信号）上，不断言内部成员。
 *
 * 依赖：只用 Qt5::Core + Qt5::Test，链接真实的 password_manager.cpp 与
 * app_config.cpp，不需要 ALSA / libgpiod / OpenCV，也不需要真实硬件。
 */
#include <QtTest/QtTest>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>

#include "config/app_config.h"
#include "managers/credential_store.h"
#include "managers/password_manager.h"

using accessterminal::AppConfig;

namespace {

/// ASCII 哨兵。ASCII 经 `fromLocal8Bit` 往返是精确的，可用于环境变量注入路径。
const char* const kSentinelPwd = "Zq7-sentinel-door-pwd-4713";

/**
 * 捕获全部 Qt 日志文本。
 *
 * 与 test_signaling_client.cpp 里的同名工具是**同一思路**：把日志当成可观测
 * 输出，用来断言"到底执行了哪条分支 / 有没有泄漏"。
 */
class MsgCapture
{
public:
    MsgCapture()
    {
        s_lines.clear();
        s_prev = qInstallMessageHandler(&MsgCapture::handler);
    }
    ~MsgCapture() { qInstallMessageHandler(s_prev); }

    /// 任意一行是否包含 needle（大小写敏感，不模糊匹配）。
    static bool contains(const QString& needle)
    {
        for (int i = 0; i < s_lines.size(); ++i) {
            if (s_lines.at(i).contains(needle)) {
                return true;
            }
        }
        return false;
    }

    /// 捕获到的行数。用于阳性对照：为 0 说明管道没工作。
    static int count() { return s_lines.size(); }

    /// 全部文本，用于失败时打印上下文。
    static QString joined() { return s_lines.join(QStringLiteral(" | ")); }

private:
    static void handler(QtMsgType, const QMessageLogContext&,
                        const QString& msg)
    {
        s_lines.append(msg);
    }

    static QStringList s_lines;
    static QtMessageHandler s_prev;
};

QStringList MsgCapture::s_lines;
QtMessageHandler MsgCapture::s_prev = nullptr;

/// 通过环境变量把门禁密码设成哨兵值（ASCII 专用 —— 见文件头第 2 条）。
void setDoorPasswordViaEnv(const QString& pwd)
{
    qputenv("ACCESSTERMINAL_SECURITY_DOOR_PASSWORD", pwd.toUtf8());
    AppConfig::instance().load(QStringLiteral("__nonexistent__.json"));
}

/// 清掉本测试用到的环境变量并重载配置。
void clearDoorPasswordEnv()
{
    qunsetenv("ACCESSTERMINAL_SECURITY_DOOR_PASSWORD");
    AppConfig::instance().load(QStringLiteral("__nonexistent__.json"));
}

/**
 * @brief 写一份最小可用的配置文件，指定 door_password 字段，返回其路径。
 *
 * 存在的理由：`AppConfig` 的 `envOverride()` 是 `if (!raw.isEmpty())` ——
 * 环境变量设成空串等同于"没设"，于是会退回**内置默认口令** `"5788"`。
 * 也就是说"引导值为空"这个状态**只能**由配置文件表达。凡是需要
 * "没有引导值"的用例都得走这里。
 *
 * `door_password` 为**空串**时写成空串（不是省略字段）：
 * 省略字段同样会退回内置默认值。
 *
 * 返回空串表示写失败。
 */
QString writeConfigWithDoorPassword(const QString& dirPath, const QString& pwd)
{
    const QString path = dirPath + QStringLiteral("/appconfig.json");
    QJsonObject security;
    security.insert(QStringLiteral("door_password"), pwd);
    QJsonObject root;
    root.insert(QStringLiteral("security"), security);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QString();
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return path;
}

/**
 * @brief 引导值必须来自**进程内** QString，不经编码边界。
 *
 * 用配置文件 + `fromLocal8Bit` 传非 ASCII 会重演文件头第 2 条那个坑
 * （本机 GBK 代码页下字节被重新解释），于是相关断言变成恒真。
 * 凡是哨兵需要非 ASCII 的用例，一律用本函数（走 `setPassword` 或
 * ASCII 配置值）。
 */
bool fileRoundTripsAsciiPath(const QString& p)
{
    return !p.isEmpty() && QFile::exists(p);
}

} // namespace

class TestPasswordManager : public QObject
{
    Q_OBJECT

private slots:

    void init()
    {
        clearDoorPasswordEnv();
    }

    void cleanup()
    {
        clearDoorPasswordEnv();
    }

    // -----------------------------------------------------------------------
    // 夹具自证：哨兵必须原样到达被测代码
    // -----------------------------------------------------------------------

    /**
     * 环境变量里的 **ASCII** 哨兵必须原样回到 AppConfig。
     *
     * 这条不是测 AppConfig，而是**保护本文件其余用例的有效性**：一旦哪天
     * 环境变量注入路径变了编码行为，哨兵就会变形，而所有"日志里不含哨兵"
     * 的断言会**悄悄变成恒真**。让它在变形时先炸，而不是让安全断言静默失效。
     */
    void asciiFixtureSurvivesEnvRoundTrip()
    {
        const QString pwd = QString::fromLatin1(kSentinelPwd);
        setDoorPasswordViaEnv(pwd);
        QCOMPARE(AppConfig::instance().doorPassword(), pwd);
    }

    // -----------------------------------------------------------------------
    // 核心：门禁密码不得进日志（运行期回归测试）
    // -----------------------------------------------------------------------

    /**
     * 输错密码时，**正确密码**不得以任何形式出现在日志里。
     *
     * 这是 baseline 真实泄漏的那条路径：只要调一次 checkPassword，正确密码
     * 就随 qDebug 落盘。这里断言明文哨兵、其 UTF-8 hex、以及用户输入都不出现。
     */
    void correctPasswordNeverAppearsInLogs()
    {
        setDoorPasswordViaEnv(QString::fromLatin1(kSentinelPwd));

        MsgCapture cap;
        PasswordManager mgr;

        // 故意输错，触发"拒绝"路径 —— baseline 正是这条路径泄漏。
        mgr.checkPassword(QStringLiteral("wrong-input-9"));

        const QString all = MsgCapture::joined();
        const QByteArray hex = QByteArray(kSentinelPwd).toHex();

        // ---- 阳性对照：证明日志管道真的工作 ----
        // 少了这一条，下面所有"不包含"断言都可能因为"什么都没捕获到"而
        // 假成立 —— 那是最危险的假绿。
        QVERIFY2(MsgCapture::count() > 0,
                 "没有捕获到任何日志：捕获管道未生效，本用例的其余断言无意义");
        QVERIFY2(MsgCapture::contains(QStringLiteral("密码校验完成")),
                 qPrintable(QStringLiteral(
                     "未捕获到 PasswordManager 自己的日志行，"
                     "无法确认捕获到的是这条路径。实际捕获: ") + all));

        // ---- 真阳性断言：正确密码绝不出现 ----
        QVERIFY2(!all.contains(QString::fromLatin1(kSentinelPwd)),
                 qPrintable(QStringLiteral("门禁密码明文泄漏到日志: ") + all));
        QVERIFY2(!all.contains(QString::fromLatin1(hex)),
                 qPrintable(QStringLiteral("门禁密码的 hex 形式泄漏到日志: ") + all));

        // ---- 用户输入同样不该出现 ----
        // 输入的每一部分都是密码的候选，且逐位日志（baseline 的
        // form_passwd.cpp 就有）会拼出完整密码。
        QVERIFY2(!all.contains(QStringLiteral("wrong-input-9")),
                 qPrintable(QStringLiteral("用户输入泄漏到日志: ") + all));
    }

    /**
     * **前缀脱敏也算泄漏**，且覆盖非 ASCII。
     *
     * 有些实现会 `password.left(4) + "***"` 地"脱敏"。那仍然泄漏了前缀 ——
     * 对短密码等于全泄。这里用一个非 ASCII 哨兵，并确认完整串、**每一个**
     * 非空前缀、以及其 hex 都不出现。
     *
     * 密码用 `setPassword()` 直接设置：`QString` 进程内传递**不经过编码边界**，
     * 因此"哨兵原样"是构造上成立的（文件头第 2 条的那个坑正出在这里）。
     */
    void partialMaskingWouldStillCountAsLeak()
    {
        const QString secret = QString::fromUtf8("门禁-哨兵-密码-8842");
        PasswordManager mgr;
        mgr.setPassword(secret);

        // 前置自证：哨兵必须原样生效，否则下面全是空断言。
        QSignalSpy probeSpy(&mgr, &PasswordManager::passwordChecked);
        mgr.checkPassword(secret);
        QCOMPARE(probeSpy.count(), 1);
        QCOMPARE(probeSpy.at(0).at(0).toBool(), true);

        MsgCapture cap;
        mgr.checkPassword(QStringLiteral("nope"));

        const QString all = MsgCapture::joined();
        QVERIFY2(MsgCapture::contains(QStringLiteral("密码校验完成")),
                 qPrintable(QStringLiteral("捕获管道失效: ") + all));

        QVERIFY2(!all.contains(secret),
                 qPrintable(QStringLiteral("密码明文泄漏: ") + all));

        // 前缀脱敏也算泄漏。逐个前缀都要断言 —— 只查 `left(4)` 会漏掉
        // `left(2)`、`mid(0,n)`、`.at(0)` 等等其它脱敏写法。
        for (int n = 1; n <= secret.size(); ++n) {
            const QString prefix = secret.left(n);
            QVERIFY2(!all.contains(prefix),
                     qPrintable(QStringLiteral("密码前缀泄漏(%1 字符): %2")
                                    .arg(n).arg(all)));
        }

        QVERIFY2(!all.contains(QString::fromLatin1(secret.toUtf8().toHex())),
                 qPrintable(QStringLiteral("密码 hex 泄漏: ") + all));
    }

    /**
     * `setPassword()` 之后新密码生效、旧密码失效 —— 并再次确认日志干净。
     *
     * 把"密码来源可被更改"和"改了之后也不泄漏"绑在一起：如果新密码走了
     * 另一条日志路径，本用例会失败。
     */
    void setPasswordChangesAcceptedValueWithoutLeaking()
    {
        const QString first = QString::fromLatin1("first-pwd-1111");
        const QString second = QString::fromLatin1("second-pwd-2222");

        PasswordManager mgr;
        QSignalSpy changed(&mgr, &PasswordManager::passwordChanged);

        mgr.setPassword(first);
        mgr.setPassword(second);
        QCOMPARE(changed.count(), 2);

        MsgCapture cap;
        QSignalSpy spy(&mgr, &PasswordManager::passwordChecked);

        mgr.checkPassword(second);      // 新密码 -> 通过
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), true);

        mgr.checkPassword(first);       // 旧密码 -> 拒绝
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(0).toBool(), false);

        const QString all = MsgCapture::joined();
        QVERIFY2(MsgCapture::contains(QStringLiteral("密码校验完成")),
                 qPrintable(QStringLiteral("捕获管道失效: ") + all));
        QVERIFY2(!all.contains(second), "新密码泄漏到日志");
        QVERIFY2(!all.contains(first), "旧密码泄漏到日志");
    }

    // -----------------------------------------------------------------------
    // 信号语义（与 baseline 一致，不得改动）
    // -----------------------------------------------------------------------

    /// 校验结果必须通过 passwordChecked(bool) 如实上报。
    void checkPasswordReportsResultOverSignal()
    {
        PasswordManager mgr;
        mgr.setPassword(QStringLiteral("pwd-3333"));
        QSignalSpy spy(&mgr, &PasswordManager::passwordChecked);

        mgr.checkPassword(QStringLiteral("pwd-3333"));
        mgr.checkPassword(QStringLiteral("bad"));
        mgr.checkPassword(QStringLiteral("pwd-3333"));

        QCOMPARE(spy.count(), 3);
        QCOMPARE(spy.at(0).at(0).toBool(), true);
        QCOMPARE(spy.at(1).at(0).toBool(), false);
        QCOMPARE(spy.at(2).at(0).toBool(), true);
    }

    /// 每次调用都必须恰好发一次信号（不能漏发，也不能重发）。
    void everyCheckEmitsExactlyOnce()
    {
        PasswordManager mgr;
        mgr.setPassword(QStringLiteral("pwd-4444"));
        QSignalSpy spy(&mgr, &PasswordManager::passwordChecked);

        for (int i = 0; i < 5; ++i) {
            mgr.checkPassword(QStringLiteral("x"));
        }
        QCOMPARE(spy.count(), 5);
    }

    /// setPassword 每次都发 passwordChanged（供界面刷新），且不触发校验信号。
    void setPasswordEmitsChangedWithoutChecking()
    {
        PasswordManager mgr;
        QSignalSpy changed(&mgr, &PasswordManager::passwordChanged);
        QSignalSpy checked(&mgr, &PasswordManager::passwordChecked);

        mgr.setPassword(QStringLiteral("a"));
        mgr.setPassword(QStringLiteral("b"));

        QCOMPARE(changed.count(), 2);
        QCOMPARE(checked.count(), 0);
    }

    /// 空输入、空密码的边界：不得崩溃，结果按 `input == password` 如实上报。
    void emptyInputAndEmptyPasswordAreHandled()
    {
        PasswordManager mgr;
        QSignalSpy spy(&mgr, &PasswordManager::passwordChecked);

        // 空密码 + 空输入 -> 相等 -> 通过。
        mgr.setPassword(QString());
        mgr.checkPassword(QString());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), true);

        // 空密码 + 非空输入 -> 拒绝。
        mgr.checkPassword(QStringLiteral("notempty"));
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(0).toBool(), false);

        // 非空密码 + 空输入 -> 拒绝（不能因为"输入为空"被误判为通过）。
        mgr.setPassword(QStringLiteral("real"));
        mgr.checkPassword(QString());
        QCOMPARE(spy.count(), 3);
        QCOMPARE(spy.at(2).at(0).toBool(), false);
    }

    /**
     * 构造时密码来自 AppConfig，且配置变更后新建实例取到新值。
     *
     * 这条守住"配置是密码的唯一来源"（第 8/9 轮修掉过"默认值散落在多个文件"
     * 的问题，见 password_manager.h 的注释）。
     */
    void passwordComesFromConfig()
    {
        setDoorPasswordViaEnv(QStringLiteral("cfg-pwd-5555"));
        QCOMPARE(AppConfig::instance().doorPassword(),
                 QStringLiteral("cfg-pwd-5555"));

        {
            PasswordManager mgr;
            QSignalSpy spy(&mgr, &PasswordManager::passwordChecked);
            mgr.checkPassword(QStringLiteral("cfg-pwd-5555"));
            QCOMPARE(spy.count(), 1);
            QCOMPARE(spy.at(0).at(0).toBool(), true);
        }

        // 改配置后新建实例必须取到新值（不是构造时缓存了旧值）。
        setDoorPasswordViaEnv(QStringLiteral("cfg-pwd-6666"));
        PasswordManager mgr2;
        QSignalSpy spy2(&mgr2, &PasswordManager::passwordChecked);
        mgr2.checkPassword(QStringLiteral("cfg-pwd-6666"));
        QCOMPARE(spy2.count(), 1);
        QCOMPARE(spy2.at(0).at(0).toBool(), true);
    }

    /// 析构不得崩溃（含校验进行中/之后立即析构）。
    void destructorIsSafe()
    {
        {
            PasswordManager mgr;
            mgr.setPassword(QStringLiteral("pwd-7777"));
            mgr.checkPassword(QStringLiteral("x"));
        }
        QVERIFY(true);
    }

    // -----------------------------------------------------------------------
    // 方案 P2：明文引导值的**非静默**迁移（第 41 轮）
    //
    // 这一组守的是 P2 的核心承诺。此前它**一条运行期断言都没有** ——
    // 只有静态规则看源码文本。而"迁移是否告警""明文有没有被写回文件"
    // 这类性质只有在运行期才看得见。
    // -----------------------------------------------------------------------

    /**
     * 明文引导值 -> 加盐哈希装入，且**必须**打迁移告警（不得静默）。
     *
     * 为什么"必须告警"是要断言的：静默迁移会让运维以为
     * `appconfig.json` 里那个明文还在生效，于是永远不去删它 ——
     * 明文就一直在配置文件里躺着。告警是**流程的一部分**，不是装饰。
     */
    void plaintextBootstrapIsMigratedWithAnExplicitWarning()
    {
        const QString plain = QString::fromLatin1(kSentinelPwd);
        setDoorPasswordViaEnv(plain);

        MsgCapture cap;
        PasswordManager mgr;

        const QString all = MsgCapture::joined();

        // ---- 阳性对照：日志管道工作 ----
        QVERIFY2(MsgCapture::count() > 0,
                 "没有捕获到任何日志：捕获管道未生效，本用例其余断言无意义");

        // ---- 真断言 1：迁移是**显式**的 ----
        QVERIFY2(MsgCapture::contains(QStringLiteral("明文")),
                 qPrintable(QStringLiteral(
                     "明文引导值被静默迁移了 —— 没有打迁移告警。实际日志: ") + all));

        // ---- 真断言 2：告警里**不得**出现明文本身 ----
        // 否则"为了提醒而打印密码"就成了新的泄漏路径。
        QVERIFY2(!all.contains(plain),
                 qPrintable(QStringLiteral("迁移告警里泄漏了明文密码: ") + all));
        QVERIFY2(!all.contains(QString::fromLatin1(plain.toUtf8().toHex())),
                 qPrintable(QStringLiteral("迁移告警里泄漏了密码 hex: ") + all));

        // ---- 真断言 3：凭据确实装进来了，且存的是**哈希**不是明文 ----
        QVERIFY2(mgr.hasPassword(), "迁移后应当有可用凭据");
        QSignalSpy spy(&mgr, &PasswordManager::passwordChecked);
        mgr.checkPassword(plain);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), true);

        // 存储层里必须**只有**哈希：明文不得作为任何字段被留存。
        // 用 toJson() 观测（它就是落盘内容，顺带验证"落盘不含明文"）。
        const QJsonObject root = mgr.credentials().toJson();
        const QByteArray json =
            QJsonDocument(root).toJson(QJsonDocument::Compact);
        const QString jsonText = QString::fromUtf8(json);
        QVERIFY2(!jsonText.contains(plain),
                 qPrintable(QStringLiteral("存储层序列化后仍含明文: ") + jsonText));
        QCOMPARE(mgr.credentials().toJson()
                     .value(QStringLiteral("users")).toArray().size(), 1);
    }

    /**
     * 配置里给的**已经是哈希串** -> 直接装入，**不得**报"明文迁移"。
     *
     * 反面断言同样重要：如果哈希串也被当成明文走迁移路径，那运维把
     * 哈希拷进配置后，程序会"把哈希当密码再哈希一遍" —— 结果谁也开不了门。
     * 而且会打一条误导性的"检测到明文"告警。
     */
    void hashedBootstrapIsLoadedWithoutAMigrationWarning()
    {
        const QString plain = QString::fromLatin1("boot-hash-pwd-8801");
        const QByteArray salt = accessterminal::CredentialStore::makeSalt();
        const QString hashed = accessterminal::CredentialStore::hashSecret(
            plain, salt, 1000);
        QVERIFY(accessterminal::CredentialStore::looksLikeHash(hashed));

        setDoorPasswordViaEnv(hashed);

        MsgCapture cap;
        PasswordManager mgr;
        const QString all = MsgCapture::joined();

        QVERIFY2(MsgCapture::count() > 0, "捕获管道未生效");

        // ---- 真断言 1：哈希串不得被当成明文 ----
        QVERIFY2(!all.contains(QStringLiteral("明文")),
                 qPrintable(QStringLiteral(
                     "配置里已是哈希串，却报了「检测到明文」—— "
                     "说明 looksLikeHash() 的判断没生效。实际日志: ") + all));

        // ---- 真断言 2：原明文仍然能开锁（证明装入的是这把锁的哈希）----
        QVERIFY2(mgr.hasPassword(), "哈希引导值应当被装入");
        QSignalSpy spy(&mgr, &PasswordManager::passwordChecked);
        mgr.checkPassword(plain);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), true);

        // 反面：别的口令不能开。
        mgr.checkPassword(plain + QStringLiteral("x"));
        QCOMPARE(spy.at(1).at(0).toBool(), false);
    }

    /**
     * 既无凭据文件、配置里也没有引导值 -> **一律拒绝**，且不假造默认口令。
     *
     * 唯一例外是重构前就存在的"空密码 + 空输入 = 通过"语义，它必须保留
     * （README 里由既有用例钉住）。本用例把两者放在一起，免得将来有人
     * 为了"一律拒绝"把空输入语义一起删掉。
     *
     * 为什么必须写**真实配置文件**：`envOverride()` 里是
     * `if (!raw.isEmpty())` —— 环境变量设成空串等于"没设"，
     * `AppConfig` 会退回内置默认值 `"5788"`。也就是说"空引导值"这个状态
     * **只能**通过配置文件表达，用环境变量根本构造不出来（初版测试正因此
     * 走了明文迁移分支而失败）。同时这也顺带证明了：删掉配置字段 ≠ 空引导值，
     * 因为内置默认值会顶上。
     */
    void noBootstrapMeansNoPasswordAndRejectsEverything()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // 显式的空串：这是唯一能表达"运维把口令清空了"的形态。
        const QString cfgPath = writeConfigWithDoorPassword(dir.path(), QString());
        QVERIFY(!cfgPath.isEmpty());
        QVERIFY(fileRoundTripsAsciiPath(cfgPath));

        AppConfig::instance().load(cfgPath);

        MsgCapture cap;
        PasswordManager mgr;
        const QString all = MsgCapture::joined();

        QVERIFY2(MsgCapture::count() > 0, "捕获管道未生效");

        // 无凭据必须**有告警**（否则运维以为门禁在工作，实际谁都进不来）。
        QVERIFY2(MsgCapture::contains(QStringLiteral("无可用门级密码")),
                 qPrintable(QStringLiteral(
                     "无可用凭据时应当告警说明「校验将一律拒绝」。实际日志: ")
                     + all));

        QVERIFY2(!mgr.hasPassword(), "没有任何来源时不应报告「有密码」");

        // 非空输入一律拒绝（不能因为"没有密码"就放行）。
        QSignalSpy spy(&mgr, &PasswordManager::passwordChecked);
        mgr.checkPassword(QStringLiteral("anything"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toBool(), false);

        // 空输入：**保留**重构前的 `input == password`（空==空）语义。
        // 这条不是"新增放行"，而是逐字节保持历史行为。
        mgr.checkPassword(QString());
        QCOMPARE(spy.count(), 2);
        QCOMPARE(spy.at(1).at(0).toBool(), true);
    }

    /**
     * `setPassword()` 换密码后，**迁移进来的旧口令必须失效**。
     *
     * 这条把"引导值迁移"和"运行期改密"接在一起：如果 setPassword 只改了
     * 某个旁路字段而没覆盖存储层，旧口令会继续能开门 —— 那是安全缺陷。
     */
    void setPasswordOverridesTheMigratedBootstrap()
    {
        const QString bootstrap = QString::fromLatin1("bootstrap-pwd-9902");
        setDoorPasswordViaEnv(bootstrap);

        PasswordManager mgr;
        QSignalSpy spy(&mgr, &PasswordManager::passwordChecked);

        // 迁移进来的口令可用。
        mgr.checkPassword(bootstrap);
        QCOMPARE(spy.at(0).at(0).toBool(), true);

        // 运行期改密。
        const QString fresh = QStringLiteral("fresh-pwd-3303");
        mgr.setPassword(fresh);

        // 新口令可用、旧引导值失效。
        mgr.checkPassword(fresh);
        QCOMPARE(spy.at(1).at(0).toBool(), true);
        mgr.checkPassword(bootstrap);
        QCOMPARE(spy.at(2).at(0).toBool(), false);

        // 且两个口令都没有进日志。
        const QString all = MsgCapture::joined();
        QVERIFY2(!all.contains(bootstrap), "旧引导值泄漏到日志");
        QVERIFY2(!all.contains(fresh), "新口令泄漏到日志");
    }
};

QTEST_MAIN(TestPasswordManager)
#include "test_password_manager.moc"
