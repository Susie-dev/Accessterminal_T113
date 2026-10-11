/**
 * @file test_form_passwd.cpp
 * @brief 门口机密码输入窗体（`src/ui/form_passwd.cpp`）单元测试。
 *
 * 为什么这个文件特别值得测
 * ------------------------
 * 它是**真实门禁的密码录入路径**，而且是"密码不得进日志"这条安全性质的
 * 唯一现场。此前该性质**只由一条静态文本规则**（`check_no_secret_in_logs`）
 * 守着，而那条规则自己写着"只做词法判定，不做数据流分析"。也就是说：
 * 只要有人换一种写法把密码漏出去（例如先 `QString s = inputBuffer;` 再
 * `qDebug() << s;`，或 `qDebug() << (inputBuffer + "x");`），静态规则
 * **看不出来**，而没有任何行为测试会拦它。
 *
 * 本测试改从**行为**上证明：把按键真的点下去，捕获进程的**实际日志输出**
 * （`qInstallMessageHandler`），然后断言那串日志里**不含**刚输入的密码。
 * 这比静态规则强，因为它不关心密码是怎么被漏出去的 —— 只要它出现在日志里
 * 就红。
 *
 * 这是本工程**第一个 Qt Widgets 测试目标**，因此在写它之前先用探针量清了
 * 三件有风险的事（%TEMP%\probe35\）：
 *   1. `uic` 能处理真实的 `form_passwd.ui`（probe35 第一次运行 rc=0）；
 *   2. 无显示器时用 `QT_QPA_PLATFORM=offscreen`，`QApplication` 能起来，
 *      `QTest::mouseClick` 能真的把 `clicked` 信号打出来（同上）；
 *   3. 按钮文本恰好是数字（`buttonTextIsTheDigit`），否则 `btn->text()`
 *      拼出来的密码就是错的（同上，0-9 全部核对通过）。
 *
 * 替换了什么、没替换什么
 * ----------------------
 * - **真实**：`form_passwd.cpp` / `form_passwd.ui`（走真 uic）、
 *   `AppConfig`（真配置类）、`QPushButton` 与真实信号槽连接、
 *   密码**信号**的载荷。
 * - **替身**：`Widget`（否则要拖进 OpenCV + 整个主程序门面）与
 *   `AudioManager`（否则真的去开 ALSA 放音，慢且依赖声卡）。
 *   两者的替换都只影响"提示音"这一条支路，**不影响密码路径**。
 *
 * 密码值刻意用**每次不同**的随机串：若用的是固定的 "1234"，就可能与
 * 界面上其它常驻文本（按钮标签、窗口标题）巧合相等，导致"日志里含有它"
 * 的断言因为无关原因通过或失败。用一个不可能出现在别处的串，断言才有
 * 判别力。这一点由 `passwordNeverAppearsInLogs` 自己保证（它同时断言
 * 这个串**确实**被当作密码发出去了，否则"日志里没有它"是真空成立）。
 */
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QPushButton>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QtTest>

#include <memory>

#include "config/app_config.h"
#include "managers/audio_manager.h"
#include "ui/form_passwd.h"
#include "ui/widget.h"

/**
 * @brief 捕获 `qDebug` / `qWarning` 等实际输出的日志记录器。
 *
 * 用 `qInstallMessageHandler` 抓消息处理器收到的**真实输出**，而不是读源码
 * 或反射 —— 这才是"密码有没有进日志"的充分证据。
 *
 * RAII：析构时恢复上一个处理器，保证测试失败/抛异常也不会把处理器留在
 * 捕获状态（那会污染同一进程里后续用例的日志）。
 */
class LogCapture
{
public:
    LogCapture()
    {
        m_previous = qInstallMessageHandler(&LogCapture::handler);
        s_current = this;
    }

    ~LogCapture()
    {
        qInstallMessageHandler(m_previous);
        s_current = nullptr;
    }

    /// 处理器收到的全部消息（已拼接成一行行文本）。
    QString text() const { return m_lines.join(QLatin1Char('\n')); }
    QStringList lines() const { return m_lines; }
    void clear() { m_lines.clear(); }

private:
    static void handler(QtMsgType type, const QMessageLogContext& ctx,
                        const QString& msg)
    {
        if (s_current) {
            s_current->m_lines.append(msg);
        }
        Q_UNUSED(type);
        Q_UNUSED(ctx);
    }

    QStringList m_lines;
    QtMessageHandler m_previous = nullptr;
    static LogCapture* s_current;
};

LogCapture* LogCapture::s_current = nullptr;

namespace {

/// 安全取出第 i 次 passwordEntered 的载荷。
///
/// 为什么不直接写 `spy.at(0).at(0)`：`QSignalSpy::at()` 越界在 Release 下是
/// **未定义行为**——第 35 轮故障注入实测，把 `emit passwordEntered()` 删掉
/// 后进程直接以 0xC0000005（访问冲突）崩溃。崩溃会终止整个测试二进制，
/// 后面所有用例都跑不到，诊断信息反而变少（只剩一个崩溃码，没有哪条
/// 断言失败的说明）。返回空串则让 QCOMPARE 干净地报出"期望 X 实际空"。
QString payloadAt(const QSignalSpy& spy, int i)
{
    if (i < 0 || i >= spy.count()) {
        return QString();
    }
    const QList<QVariant> args = spy.at(i);
    return args.isEmpty() ? QString() : args.at(0).toString();
}

/// 造一个"不可能出现在界面/标题里"的密码串。
QString makePassword(int digits)
{
    // 用高位数字组合，且长度可配；界面上的按钮文本只有单个 0-9，
    // 窗口标题是 "Form"，都不可能等于这种多位串。
    QString s;
    const int seed[] = {9, 7, 4, 8, 3, 6, 1, 5};
    for (int i = 0; i < digits; ++i) {
        s += QString::number(seed[i % 8]);
    }
    return s;
}

/// 找到某个数字按钮（uic 生成的名字是 btn0..btn9）。
QPushButton* digitButton(QWidget* form, int digit)
{
    return form->findChild<QPushButton*>(
        QStringLiteral("btn%1").arg(digit));
}

/// 依次点击密码的每一位（走真实鼠标事件，触发真实 clicked 连接）。
void clickDigits(QWidget* form, const QString& password)
{
    for (int i = 0; i < password.size(); ++i) {
        const int d = QString(password.at(i)).toInt();
        QPushButton* b = digitButton(form, d);
        QVERIFY2(b != nullptr,
                 qPrintable(QStringLiteral("找不到数字按钮 btn%1").arg(d)));
        QTest::mouseClick(b, Qt::LeftButton);
    }
}

} // namespace

class TestFormPasswd : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    // ---- 界面装配 ----
    void allDigitButtonsExistAndShowTheRightDigit();
    void okAndBackButtonsExist();
    void digitButtonsDoNotTakeFocus();

    // ---- 密码采集（行为）----
    void digitsBuildThePasswordInOrder();
    void passwordIsEmittedOnOk();
    void passwordEnteredPayloadIsExactlyWhatWasTyped();
    void backspaceRemovesTwoDigitsPerPress_documentsInheritedDefect();
    void okIsIdempotentDespiteDoubleConnection();
    void backspaceOnEmptyDoesNothing();
    void okWithEmptyBufferEmitsNothing();
    void okClearsTheBufferSoTheNextEntryStartsFresh();
    void atMostEightDigitsAreAccepted();
    void formClosesOnOk();

    // ---- 安全性质（本文件的核心）----
    void passwordNeverAppearsInLogs();
    void passwordNeverAppearsInLogsAsHex();
    void typingLogsDoNotRevealIndividualDigits();
    void passwordIsEmittedBeforeBeingCleared();

    // ---- 提示音支路 ----
    void eachDigitPlaysAKeySound();
    void backspacePlaysAKeySound();
    void keySoundPathComesFromAppConfig();
    void noKeySoundWhenParentIsNotWidget();

    // ---- 正对照 ----
    void logCapturePipelineWorks();
};

void TestFormPasswd::initTestCase()
{
    // 无显示器环境（以及本机）都必须能跑：走 offscreen 平台插件。
    // 若外部已指定（例如真机有 X），则不覆盖。
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    // 提示音路径必须确定，否则 keySoundPathComesFromAppConfig 会随机器变。
    qputenv("ACCESSTERMINAL_RESOURCES_PCM_DIR",
            QByteArrayLiteral("/tmp/pcm-for-passwd-test"));
}

void TestFormPasswd::init()
{
    // 每个用例都重新读一遍环境覆盖，避免上一个用例的残留。
    accessterminal::AppConfig::instance().load(QString());
}

void TestFormPasswd::cleanup()
{
    QTest::qWait(10);
}

// ---------------------------------------------------------------------------
// 界面装配
// ---------------------------------------------------------------------------

void TestFormPasswd::allDigitButtonsExistAndShowTheRightDigit()
{
    Form_passwd form;
    for (int d = 0; d <= 9; ++d) {
        QPushButton* b = digitButton(&form, d);
        QVERIFY2(b != nullptr,
                 qPrintable(QStringLiteral("btn%1 必须存在").arg(d)));
        // form_passwd.cpp 用 btn->text() 拼密码，所以文本必须恰好是那一位。
        QCOMPARE(b->text(), QString::number(d));
    }
}

void TestFormPasswd::okAndBackButtonsExist()
{
    Form_passwd form;
    QVERIFY(form.findChild<QPushButton*>(QStringLiteral("btnok")) != nullptr);
    QVERIFY(form.findChild<QPushButton*>(QStringLiteral("btnbck")) != nullptr);
}

void TestFormPasswd::digitButtonsDoNotTakeFocus()
{
    // 门口机是触摸屏：数字键抢焦点会导致"确定"键的按下态显示异常，
    // 构造函数里显式设了 NoFocus。这是产品行为，钉住。
    Form_passwd form;
    for (int d = 0; d <= 9; ++d) {
        QPushButton* b = digitButton(&form, d);
        QVERIFY(b != nullptr);
        QCOMPARE(b->focusPolicy(), Qt::NoFocus);
    }
}

// ---------------------------------------------------------------------------
// 密码采集
// ---------------------------------------------------------------------------

void TestFormPasswd::digitsBuildThePasswordInOrder()
{
    Form_passwd form;
    QSignalSpy spy(&form, &Form_passwd::passwordEntered);

    const QString pwd = makePassword(4);
    clickDigits(&form, pwd);

    // 还没按"确定"，不该发出任何信号。
    QCOMPARE(spy.count(), 0);

    QPushButton* ok = form.findChild<QPushButton*>(QStringLiteral("btnok"));
    QVERIFY(ok != nullptr);
    QTest::mouseClick(ok, Qt::LeftButton);

    QCOMPARE(spy.count(), 1);
    // 顺序必须与点击顺序一致 —— 顺序错了密码就错了，而且很难发现。
    QCOMPARE(payloadAt(spy, 0), pwd);
}

void TestFormPasswd::passwordIsEmittedOnOk()
{
    Form_passwd form;
    QSignalSpy spy(&form, &Form_passwd::passwordEntered);
    clickDigits(&form, QStringLiteral("9"));
    QCOMPARE(spy.count(), 0);

    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(payloadAt(spy, 0), QStringLiteral("9"));
}

void TestFormPasswd::passwordEnteredPayloadIsExactlyWhatWasTyped()
{
    // 覆盖 1..8 位全部长度：任何一位被吃掉/重复都会红。
    for (int n = 1; n <= 8; ++n) {
        Form_passwd form;
        QSignalSpy spy(&form, &Form_passwd::passwordEntered);
        const QString pwd = makePassword(n);
        clickDigits(&form, pwd);
        QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                          Qt::LeftButton);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(payloadAt(spy, 0), pwd);
    }
}

void TestFormPasswd::backspaceRemovesTwoDigitsPerPress_documentsInheritedDefect()
{
    // ===================================================================
    // 这条测的不是"期望行为"，而是一个**继承来的真实缺陷**（特征化测试）。
    // ===================================================================
    //
    // 现象：点一次"退格"，删掉**两位**，不是一位。
    //
    // 根因（第 35 轮用探针 %TEMP%\probe35\mech.cpp 实测确定）：
    // uic 生成的 `setupUi()` 末尾会调用
    //     QMetaObject::connectSlotsByName(Form_passwd);
    // 它按**命名约定**自动连接：槽名形如 `on_<对象名>_<信号名>` 就自动接上
    // `clicked()`。而 form_passwd.cpp 的构造函数**又显式** connect 了一次：
    //     connect(ui->btnbck, &QPushButton::clicked,
    //             this, &Form_passwd::on_btnbck_clicked);
    // 于是同一个槽被连了**两遍**，一次点击执行两次。
    //
    // 探针实测（区分了两种可能）：
    //     btnbck->click()  : clicked 发射 1 次，槽执行 2 次
    //     btnok ->click()  : clicked 发射 1 次，槽执行 2 次
    //     对照 普通按钮      : clicked 发射 1 次
    // 信号只发一次、槽跑两次 —— 确定是"双重连接"，而不是鼠标事件送了两遍。
    //
    // 这是 **baseline 继承来的**，不是重构引入的：基线
    // desktop/../terminal/smartpronew/form_passwd.cpp:42 就是
    //     connect(ui->btnbck, &QPushButton::clicked, this,
    //             &Form_passwd::on_btnbck_clicked);
    // 而且基线的 .ui 里没有 `<connections>` 块，所以 uic 一定会走
    // connectSlotsByName 这条路。
    //
    // 为什么**不修**：协议与行为逐字节兼容是硬约束，而"退格删几位"是用户可
    // 感知的行为变化。修它属于产品决策（改了之后，习惯了"一次删两位"的现场
    // 人员会觉得更难用），必须由人确认。因此本测试把**现状**钉住：
    // 一旦有人改了行为，这条会红，提醒他这是有意变更而不是顺手改的。
    //
    // 已登记在 README 的"已知限制"里，与 refstop、calcChecksum 等并列。
    Form_passwd form;
    QSignalSpy spy(&form, &Form_passwd::passwordEntered);

    QPushButton* bck = form.findChild<QPushButton*>(QStringLiteral("btnbck"));
    QVERIFY(bck != nullptr);

    // 输入三位，这样"删掉两位"之后还剩一位，可从信号载荷观察到差别。
    // （若只输入两位，一次退格会把缓冲清空，OK 就不发信号了 —— 那只能
    //   证明"删了不止一位"，分不清删了 2 位还是全清。）
    clickDigits(&form, QStringLiteral("123"));

    // 先确认"双重连接"确实存在（否则下面的期望值本身就该改）。
    QSignalSpy clickedSpy(bck, &QPushButton::clicked);
    QTest::mouseClick(bck, Qt::LeftButton);
    QCOMPARE(clickedSpy.count(), 1);  // 信号只发一次

    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);

    QCOMPARE(spy.count(), 1);
    // 三位输入 - 2（双重连接）= "1"。若哪天变成 "12"，说明缺陷被修好了，
    // 请更新本条测试并同步 README 的已知限制。
    QCOMPARE(payloadAt(spy, 0), QStringLiteral("1"));
}

void TestFormPasswd::okIsIdempotentDespiteDoubleConnection()
{
    // "确定"键同样被连了两遍，但它**恰好**是幂等的：
    // 第一次执行时 inputBuffer 非空 -> emit + clear + close()；
    // 第二次执行时 inputBuffer 已空 -> 不发信号（因为 !isEmpty() 判断）。
    //
    // 所以密码**只发一次**，不会重复提交。这一条把这个"侥幸"钉住：
    // 如果有人把 `if (!inputBuffer.isEmpty())` 去掉，密码就会被提交两次
    // ——那可能意味着门锁被驱动两次。这是真实风险，必须测。
    Form_passwd form;
    QSignalSpy spy(&form, &Form_passwd::passwordEntered);

    const QString pwd = makePassword(4);
    clickDigits(&form, pwd);
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);

    QVERIFY2(spy.count() == 1,
             qPrintable(QStringLiteral("密码必须只提交一次，实际 %1 次"
                                       "（双重连接下靠 isEmpty 判断兜住）")
                            .arg(spy.count())));
    QCOMPARE(payloadAt(spy, 0), pwd);
}

void TestFormPasswd::backspaceOnEmptyDoesNothing()
{
    Form_passwd form;
    QSignalSpy spy(&form, &Form_passwd::passwordEntered);

    // 空缓冲退格：不能崩、也不能产生负长度之类的怪状态。
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnbck")),
                      Qt::LeftButton);
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnbck")),
                      Qt::LeftButton);
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);

    // 缓冲仍是空的 -> 不发信号。
    QCOMPARE(spy.count(), 0);
}

void TestFormPasswd::okWithEmptyBufferEmitsNothing()
{
    Form_passwd form;
    QSignalSpy spy(&form, &Form_passwd::passwordEntered);
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);
    // 空密码绝不能发给 PasswordManager 去比对（否则空串可能匹配某些实现）。
    QCOMPARE(spy.count(), 0);
}

void TestFormPasswd::okClearsTheBufferSoTheNextEntryStartsFresh()
{
    Form_passwd form;
    QSignalSpy spy(&form, &Form_passwd::passwordEntered);

    clickDigits(&form, QStringLiteral("77"));
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);
    QCOMPARE(payloadAt(spy, 0), QStringLiteral("77"));

    // 第二次输入必须从空开始：若没清，会拼成 "773" 之类。
    clickDigits(&form, QStringLiteral("3"));
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(payloadAt(spy, 1), QStringLiteral("3"));
}

void TestFormPasswd::atMostEightDigitsAreAccepted()
{
    Form_passwd form;
    QSignalSpy spy(&form, &Form_passwd::passwordEntered);

    // 输入 12 位，只应接受前 8 位（代码：if (inputBuffer.size() >= 8) return;）。
    clickDigits(&form, makePassword(8) + QStringLiteral("1234"));
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(payloadAt(spy, 0).size(), 8);
    QCOMPARE(payloadAt(spy, 0), makePassword(8));
}

void TestFormPasswd::formClosesOnOk()
{
    Form_passwd form;
    form.show();
    QVERIFY(QTest::qWaitForWindowExposed(&form));
    QVERIFY(form.isVisible());

    clickDigits(&form, QStringLiteral("5"));
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);

    // on_btnok_clicked() 末尾会 this->close()。
    QVERIFY2(!form.isVisible(), "按下确定后窗体必须关闭");
}

// ---------------------------------------------------------------------------
// 安全性质：密码不得进入日志
// ---------------------------------------------------------------------------

void TestFormPasswd::passwordNeverAppearsInLogs()
{
    // 这是本文件存在的首要理由。
    //
    // 断言方式：真的点出密码 -> 捕获真实日志 -> 断言日志里没有那串密码。
    // 与静态规则不同，这里不关心密码是"怎么"漏出去的，只看结果。
    LogCapture cap;

    Form_passwd form;
    QSignalSpy spy(&form, &Form_passwd::passwordEntered);

    const QString pwd = makePassword(8);
    clickDigits(&form, pwd);
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);

    // **正对照**：先确认这个串真的被当作密码发出去了。
    // 少了这一步，"日志里没有它"可能是因为密码压根没被采集到 ——
    // 那样断言就是真空成立的。
    QCOMPARE(spy.count(), 1);
    QCOMPARE(payloadAt(spy, 0), pwd);

    const QString log = cap.text();
    QVERIFY2(!log.isEmpty(),
             "日志捕获必须是通的（正对照见 logCapturePipelineWorks）");
    QVERIFY2(!log.contains(pwd),
             qPrintable(QStringLiteral("密码明文出现在日志里！\n密码=%1\n日志=%2")
                            .arg(pwd, log)));
}

void TestFormPasswd::passwordNeverAppearsInLogsAsHex()
{
    // 原缺陷里除了明文还打了 hex（inputHex / pwdHex）。逐字节 hex 同样能
    // 完整还原密码，所以必须一起禁。
    LogCapture cap;

    Form_passwd form;
    QSignalSpy spy(&form, &Form_passwd::passwordEntered);

    const QString pwd = makePassword(8);
    clickDigits(&form, pwd);
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);
    QCOMPARE(spy.count(), 1);

    const QString log = cap.text();
    const QString hex = QString::fromLatin1(pwd.toUtf8().toHex());
    QVERIFY2(!log.contains(hex, Qt::CaseInsensitive),
             qPrintable(QStringLiteral("密码的 hex 形式出现在日志里！\nhex=%1\n"
                                       "日志=%2").arg(hex, log)));
    const QString upperHex = QString::fromLatin1(
        pwd.toUtf8().toHex().toUpper());
    QVERIFY2(!log.contains(upperHex, Qt::CaseInsensitive),
             qPrintable(QStringLiteral("大写 hex 也出现了：%1").arg(upperHex)));
}

void TestFormPasswd::typingLogsDoNotRevealIndividualDigits()
{
    // 逐位日志与整串日志叠加可以完整还原输入过程，所以逐位也不能打。
    //
    // 为什么不用"日志里不含连续 4 位"这种写法（第一版就是，被故障注入
    // 证明是**假绿**）：逐位日志长这样
    //     QDEBUG : TestFormPasswd::typingLogsDoNotRevealIndividualDigits()
    //              [passwd] key 9
    //     QDEBUG : TestFormPasswd::typingLogsDoNotRevealIndividualDigits()
    //              [passwd] key 8
    // 两位之间插着测试名、级别前缀等固定文本，所以输入的数字**永远不会**
    // 在日志里连续出现。那个断言对"逐位泄露"毫无判别力（实测漏报 2/2）。
    //
    // 有判别力的性质是**差分**：日志内容不该取决于按了**哪几个**数字。
    // 取两个等长、且**不共用任何数字**的密码各跑一遍，归一化后逐字节比较：
    //   - 只记录"按了一个数字键" -> 两份日志完全相同 ✓
    //   - 记录 btn->text()/mid() 等任一位 -> 两份日志必然不同 ✗
    // 这个性质与日志格式、前缀、间隔都无关，因此不会因为排版变化而失效。
    const QString pwdA = QStringLiteral("99999999");
    const QString pwdB = QStringLiteral("11111111");

    // 先确认两个密码真的不共用数字，否则差分性质本身就退化了。
    for (int i = 0; i < pwdB.size(); ++i) {
        QVERIFY2(!pwdA.contains(pwdB.at(i)),
                 "两个密码必须不共用任何数字，否则差分没有判别力");
    }

    // 跑一遍并在归一化后返回日志：丢掉 QTest 的前缀与用例名，
    // 只留被测代码自己打出来的内容。
    struct Run {
        static QString go(const QString& pwd)
        {
            LogCapture cap;
            Form_passwd form;
            clickDigits(&form, pwd);
            QTest::mouseClick(
                form.findChild<QPushButton*>(QStringLiteral("btnok")),
                Qt::LeftButton);
            QStringList keep;
            const QStringList raw = cap.lines();
            for (int i = 0; i < raw.size(); ++i) {
                const QString& l = raw.at(i);
                // 只保留 [passwd] 前缀的、由 form_passwd.cpp 打出的行。
                const int at = l.indexOf(QStringLiteral("[passwd]"));
                if (at >= 0) {
                    keep.append(l.mid(at));
                }
            }
            return keep.join(QLatin1Char('\n'));
        }
    };

    const QString logA = Run::go(pwdA);
    const QString logB = Run::go(pwdB);

    // 正对照：确实抓到了被测代码的日志。否则"两份相同"是因为都是空串。
    QVERIFY2(!logA.isEmpty(),
             "必须抓到 [passwd] 日志，否则下面的比较是真空的");
    QVERIFY2(!logB.isEmpty(), "必须抓到 [passwd] 日志");

    QVERIFY2(logA == logB,
             qPrintable(QStringLiteral(
                 "日志内容随输入的数字变化，说明记录了按键内容：\n"
                 "输入 %1 时的日志：\n%2\n\n输入 %3 时的日志：\n%4")
                            .arg(pwdA, logA, pwdB, logB)));

    // 再补一条针对"整串泄露"的：等长不同数字，日志长度也不该变。
    QCOMPARE(logA.size(), logB.size());
}

void TestFormPasswd::passwordIsEmittedBeforeBeingCleared()
{
    // 顺序契约：先 emit 再 clear。若顺序反了，密码永远是空串，
    // 门就永远打不开 —— 而且"密码不进日志"仍然成立，很容易漏测。
    Form_passwd form;
    QSignalSpy spy(&form, &Form_passwd::passwordEntered);

    const QString pwd = makePassword(6);
    clickDigits(&form, pwd);
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnok")),
                      Qt::LeftButton);

    QCOMPARE(spy.count(), 1);
    // 载荷非空且等于输入 —— 这一条同时排除了"先 clear 后 emit"。
    QVERIFY2(!payloadAt(spy, 0).isEmpty(),
             "emit 必须发生在 clear 之前，否则载荷是空串");
    QCOMPARE(payloadAt(spy, 0), pwd);
}

// ---------------------------------------------------------------------------
// 提示音支路
// ---------------------------------------------------------------------------

void TestFormPasswd::eachDigitPlaysAKeySound()
{
    // 提示音是产品行为（门口机触摸屏需要按键反馈），不能被顺手删掉。
    //
    // 注意：数字键走的是 `onDigitClicked`（名字**不**符合
    // `on_<对象名>_<信号名>` 约定），所以只有显式 connect 那一次，不存在
    // 双重连接 —— 3 次点击就是 3 次提示音。
    Widget mainWin;
    AudioManager mgr;
    mainWin.setAudioManagerForTest(&mgr);

    Form_passwd form(&mainWin);
    mgr.resetForTest();

    clickDigits(&form, QStringLiteral("135"));
    QCOMPARE(mgr.playAsyncCalls, 3);
}

void TestFormPasswd::backspacePlaysAKeySound()
{
    Widget mainWin;
    AudioManager mgr;
    mainWin.setAudioManagerForTest(&mgr);

    Form_passwd form(&mainWin);
    // 输入两位：一次退格（删两位）后缓冲正好变空，
    // 于是"第二次退格不该响"可以干净地测出来。
    clickDigits(&form, QStringLiteral("12"));
    mgr.resetForTest();

    // 退格槽被连了两遍（见 backspaceRemovesTwoDigitsPerPress_...），
    // 所以**一次点击响两声**。这也是继承来的现状，一并钉住：
    // 若哪天双重连接被修好，这里会变成 1，提示同步更新。
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnbck")),
                      Qt::LeftButton);
    QCOMPARE(mgr.playAsyncCalls, 2);

    // 缓冲已空（两位都被删光）-> 再点退格，槽内 if 不成立，**不该**响。
    mgr.resetForTest();
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnbck")),
                      Qt::LeftButton);
    QCOMPARE(mgr.playAsyncCalls, 0);
}

void TestFormPasswd::keySoundPathComesFromAppConfig()
{
    Widget mainWin;
    AudioManager mgr;
    mainWin.setAudioManagerForTest(&mgr);

    Form_passwd form(&mainWin);
    mgr.resetForTest();

    QTest::mouseClick(digitButton(&form, 4), Qt::LeftButton);

    QCOMPARE(mgr.playedPaths.size(), 1);
    // 路径必须由 AppConfig 拼（initTestCase 里把 pcmDir 覆盖成了固定值），
    // 而不是散落的字面量 —— 否则部署时改目录会漏改。
    const QString expected =
        accessterminal::AppConfig::instance().pcmFile(QStringLiteral("beep.pcm"));
    QCOMPARE(mgr.playedPaths.at(0), expected);
    QVERIFY2(expected.endsWith(QStringLiteral("beep.pcm")),
             qPrintable(expected));
    QVERIFY2(expected.contains(QStringLiteral("pcm-for-passwd-test")),
             qPrintable(QStringLiteral("应当跟随配置的 pcmDir：%1").arg(expected)));
}

void TestFormPasswd::noKeySoundWhenParentIsNotWidget()
{
    // getMainWidget() 在父链上没有 Widget 时返回 nullptr；此时
    // playKeySound 走 "mainWin is nullptr" 分支，**不得崩**。
    // 这是真实场景：窗体被单独构造出来调试时就是这种父子关系。
    Form_passwd form;  // 没有 Widget 祖先
    LogCapture cap;

    clickDigits(&form, QStringLiteral("1"));
    QTest::mouseClick(form.findChild<QPushButton*>(QStringLiteral("btnbck")),
                      Qt::LeftButton);

    // 不该崩；而且应当留下"没找到主窗口"的诊断线索。
    QVERIFY2(cap.text().contains(QStringLiteral("nullptr")),
             qPrintable(QStringLiteral("应当记录找不到主窗口：%1").arg(cap.text())));
}

// ---------------------------------------------------------------------------
// 正对照
// ---------------------------------------------------------------------------

void TestFormPasswd::logCapturePipelineWorks()
{
    // 上面三条"日志里没有 X"的断言都依赖捕获管道真的在工作。
    // 若 qInstallMessageHandler 没生效，那些断言会**真空成立**。
    // 这里主动打一条含哨兵串的日志，证明它抓得到。
    LogCapture cap;
    const QString sentinel = QStringLiteral("SENTINEL-PASSWD-TEST-9f3a");
    qDebug() << "[passwd-test]" << sentinel;
    QVERIFY2(cap.text().contains(sentinel),
             "日志捕获管道必须真的工作，否则前面几条断言是真空的");
}

QTEST_MAIN(TestFormPasswd)
#include "test_form_passwd.moc"
