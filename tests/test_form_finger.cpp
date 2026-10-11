/**
 * @file test_form_finger.cpp
 * @brief 指纹注册窗体（`src/ui/form_finger.cpp`）单元测试 —— 现场**真正执行**的
 *        注册状态机，以及它和常驻识别之间的**串口互斥**。
 *
 * 为什么这个文件值得写
 * --------------------
 * 门口机的指纹注册在工程里有**两份**实现：
 *
 *   A) `FingerprintManager::startEnroll(int)` —— 有测试，但**零生产调用者**；
 *   B) `Form_finger::startFingerRegisterStep()` + `doFingerCollect()`
 *      —— 内联状态机，直接读写串口，**这才是现场执行的那条**。
 *
 * 第 23 轮用静态规则 `check_live_fingerprint_enroll_sequence_is_pinned` 把 B 的
 * **命令字节序列**钉住了（那条规则的注释里就写着"B 无法用单元测试覆盖"）。
 * 但静态规则只能看**文本**，看不到下面这几类真实缺陷 —— 本文件把它们变成
 * 可失败的断言：
 *
 *   1. **串口互斥没有成对恢复。** 注册期间必须 `setSuspended(true)` 挂起常驻
 *      识别（两者抢同一个串口，会互为对方之应答）。若退出/析构路径漏了恢复，
 *      表现是**门禁指纹识别永久失效** —— 而命令序列一个字节都没变。
 *   2. **管理器的生命周期早于窗体。** 本窗口与 `FingerprintManager` 都是
 *      `Widget` 的子对象，而 **Qt 按创建顺序销毁子对象**（先建先死，非逆序；
 *      第 37 轮用探针实测确认）。生产里管理器在 widget.cpp:250 创建、窗体在
 *      widget.cpp:670 创建 —— `delete Widget` 时管理器**先**死，等窗体析构函数
 *      再对那个裸指针调 `setSuspended()`，就是 use-after-free。本文件的
 *      `Fixture` **刻意复刻这一创建顺序**，因此修复前的代码在这里会 SIGSEGV。
 *   3. **轮询定时器泄漏。** 回调里 `readPacket()` 是**阻塞读**且跑在 GUI 线程。
 *      若退出时不停表，窗口隐藏后它仍每 500ms 阻塞一次 GUI，把界面拖死。
 *   4. **延时的后继步骤未被作废。** 分步注册之间 `QTimer::singleShot(1500)`
 *      进入下一步；用户在这 1.5 秒内点退出时，仅停表不够 —— 已排队的
 *      singleShot 仍会把轮询重新拉起来（`m_pollGeneration` 就是为此存在）。
 *
 * 做法
 * ----
 * 用第 35/36 轮已经验证过的影子头方案：`tests/shadows/ui/widget.h` 提供
 * `Widget` 替身，`tests/shadows/managers/audio_manager.h` 避免真 ALSA，
 * 再用项目里**原有的**链接期替身 `tests/fake_fingerprint_device.cpp` 顶掉真串口
 * （它的 `buildPacket()` 是真实算法逐字节拷贝，报文字节是唯一必须守住的东西）。
 *
 * **关键取向**：`FingerprintManager` 用**真实实现**。互斥开关
 * `setSuspended()` 的可观测效果（停表、恢复时重启识别）就在真实现里；
 * 换成替身，"有没有成对恢复"就退化成"替身自己记了一笔"（第 36 轮教训）。
 *
 * 替身替换清单：
 *   | 用真的 | 用替身 |
 *   |---|---|
 *   | `form_finger.cpp` / `form_finger.ui`（真 uic） | `Widget`（影子头） |
 *   | **真实** `FingerprintManager`（互斥语义） | `AudioManager`（影子头） |
 *   | 真实 `QTimer` / `QPushButton` / 信号槽 | `HailinFingerprint`（链接期替身） |
 *
 * 一个必须知道的夹具细节
 * ----------------------
 * 真实 `FingerprintManager` 也在用**同一个**假串口：`startIdentify()` 每 500ms
 * 下发一条 CMD_GET_IMAGE（fingerprint_manager.cpp:118/199）。所以"窗体停表了吗"
 * 不能直接看 `fakefp::sentCount()` 的总数 —— 窗体退出时管理器正好被**恢复**并
 * 开始自己轮询，计数照涨，断言会误报。凡是要单独观测窗体流量处，先用
 * `muteResidentIdentifier()` 把常驻识别静音（第 37 轮实测踩到并修正）。
 */
#include <QApplication>
#include <QDebug>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtTest>

#include <functional>

#include "config/app_config.h"
#include "fake_fingerprint_device.h"
#include "hal/fingerprint_hailin.h"
#include "managers/audio_manager.h"
#include "managers/fingerprint_manager.h"
#include "ui/form_finger.h"
#include "ui/widget.h"

namespace {

/// 等一个条件成立，最多 timeoutMs 毫秒。**绝不**用固定时长 sleep 代替性质等待。
bool waitFor(const std::function<bool()>& pred, int timeoutMs = 4000)
{
    QElapsedTimer t;
    t.start();
    while (!pred() && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(5);
    }
    return pred();
}

} // namespace

/**
 * @brief 记录 QObject 子对象析构顺序的探针（纯记录，无副作用）。
 *
 * 用途：钉住 Qt 的一条容易被记反的规则 —— **销毁子对象是按创建顺序，
 * 先建先死**，不是很多人以为的逆序。它直接决定了
 * "Form_finger 析构时 FingerprintManager 是否已经死了"这个判断。
 */
struct TeardownProbe {
    static QStringList& log()
    {
        static QStringList s;
        return s;
    }
    static void reset() { log().clear(); }
};

/// 被记录析构的探针子对象。
struct ProbeChild : QWidget {
    QString tag;
    ProbeChild(const QString& t, QWidget* parent)
        : QWidget(parent), tag(t) {}
    ~ProbeChild() override
    {
        TeardownProbe::log() << tag + QStringLiteral(" DTOR");
    }
};

/**
 * @brief 注册环境。
 *
 * **成员声明顺序是有意义的**：C++ 按声明逆序析构，所以
 * `mainWin` 必须早于 `dev`/`mgr` 被销毁 —— dev/mgr 声明在前，才能活过
 * `mainWin` 的析构。
 *
 * 更关键的是 `fpMgr` 与 `form` 的**创建顺序**：两者都 parent 到 `mainWin`，
 * 且 `fpMgr` 先建、`form` 后建 —— 这正是生产代码的顺序
 * （widget.cpp:250 建管理器，widget.cpp:670 建窗体）。Qt 按创建顺序销毁
 * 子对象，于是 `mainWin` 析构时管理器先死、窗体后死。修复前，窗体析构函数
 * 里对已销毁管理器的 `setSuspended()` 调用就是 use-after-free —— 本夹具
 * **故意**保持这个顺序，好让该缺陷在单元测试里直接崩出来（而不是只在真机上
 * 偶发）。这与"夹具要尽量贴近生产"的原则一致，不要为了"跑得稳"把它改成
 * 栈对象或调换顺序，那会把这条路径的检出能力一起改没。
 */
struct Fixture {
    HailinFingerprint dev;
    AudioManager mgr;
    Widget mainWin;
    FingerprintManager* fpMgr = nullptr;   ///< parent = mainWin（先建）
    Form_finger* form = nullptr;           ///< parent = mainWin（后建）

    bool up()
    {
        fakefp::reset();
        mainWin.setAudioManagerForTest(&mgr);
        mainWin.setFingerprintForTest(&dev);
        mainWin.setNextFingerIdForTest(1);

        fpMgr = new FingerprintManager(&mainWin);   // 先建：生产 widget.cpp:250
        fpMgr->setDevice(&dev);
        mainWin.setFingerprintManagerForTest(fpMgr);

        form = new Form_finger(&mainWin);           // 后建：生产 widget.cpp:670
        return form != nullptr;
    }

    QPushButton* button(const char* name) const
    {
        return form->findChild<QPushButton*>(QString::fromLatin1(name));
    }

    QLabel* label() const
    {
        return form->findChild<QLabel*>(QStringLiteral("labelmess"));
    }

    /// 点下"添加指纹"。
    void clickAddFinger()
    {
        QPushButton* b = button("addfinger");
        QVERIFY(b != nullptr);
        QTest::mouseClick(b, Qt::LeftButton);
    }

    /// 点下"退出"。
    void clickQuit()
    {
        QPushButton* b = button("addquit");
        QVERIFY(b != nullptr);
        QTest::mouseClick(b, Qt::LeftButton);
    }

    /// 让假模块对接下来 n 次 readPacket 都回"成功"（resp[9]==0x00）。
    void answerSuccess(int n = 64)
    {
        for (int i = 0; i < n; ++i) {
            fakefp::pushConfirm(0x00);
        }
    }

    /// 只让**下一次** readPacket 成功，之后的都拿不到应答。
    /// 用来精确落在"轮询探到手指、但紧接着采集图像失败"这条路径上。
    void answerSuccessOnce() { fakefp::pushConfirm(0x00); }

    /**
     * @brief 把**常驻识别**静音，使此后 `fakefp::sentCount()` 只可能反映窗体
     *        自己的轮询。
     *
     * 窗体退出时会把管理器从挂起状态**恢复**，管理器随即开始自己每 500ms 一次
     * 的探测；不去管它，"窗体停表了吗"就会因为管理器的合法流量而误报失败。
     * 静音后再抽空事件队列，确保管理器的在途 timeout 与已排队的
     * `singleShot(0, ...)` 都已消化（`startIdentify()` 会先查 `m_suspended`
     * 而直接返回）。
     */
    void muteResidentIdentifier()
    {
        fpMgr->setSuspended(true);
        QTest::qWait(700);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 120);
    }

    /// 在给定毫秒窗口内，窗体侧不应再下发任何命令。
    void assertNoFurtherTraffic(int windowMs = 1300)
    {
        const int baseline = fakefp::sentCount();
        QTest::qWait(windowMs);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 120);
        QCOMPARE(fakefp::sentCount(), baseline);
    }
};

class TestFormFinger : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // ---- 界面装配 ----
    void allButtonsExist();
    void buttonsDoNotTakeFocus();

    // ---- 信号接线 ----
    void addFaceButtonEmitsAddFaceRequested();
    void addFingerButtonStartsEnrollmentNotAddFace();
    void quitButtonDoesNotEmitAddFace();
    void faceButtonClickIsNotDuplicatedByAutoConnect();

    // ---- 串口互斥（本文件最重要的性质）----
    void enrollingSuspendsTheResidentIdentifier();
    void quitResumesTheResidentIdentifier();
    void destructionResumesTheResidentIdentifier();
    void enterExitTwiceKeepsSuspensionPaired();
    void repeatedQuitDoesNotConfuseSuspension();

    // ---- 生命周期：管理器先于窗体销毁（use-after-free 的成因）----
    void managerIsDestroyedBeforeTheFormProvesTheHazardIsReal();

    // ---- 轮询定时器生命周期 ----
    void quitStopsThePollTimer();
    void destructionStopsThePollTimer();
    void repeatedAddFingerDoesNotStackPollTimers();

    // ---- 拒绝重入 ----
    void secondAddFingerClickWhileWaitingIsIgnored();

    // ---- 命令序列（与静态规则互为印证）----
    void pollSendsGetImageWith500msTimeout();
    void collectStepSendsGetImageThenGenChar();
    void fullEnrollSequenceMatchesThePinnedContract();
    void collectingSuccessPlaysTheStepTone();
    void collectFailureDoesNotAdvance();

    // ---- 失败后可重试 ----
    void failedCollectClearsWaitingSoUserCanRetry();

    // ---- 指纹库 ID ----
    void successfulEnrollIncrementsTheNextFingerId();
    void storeCharCarriesTheNextFingerIdBigEndian();

    // ---- 正对照 ----
    void fakeDeviceDeliversBytesToReadPacket();
};

void TestFormFinger::initTestCase()
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    qputenv("ACCESSTERMINAL_RESOURCES_PCM_DIR",
            QByteArrayLiteral("/tmp/pcm-for-finger-test"));
}

void TestFormFinger::init()
{
    accessterminal::AppConfig::instance().load(QString());
}

// ---------------------------------------------------------------------------
// 界面装配
// ---------------------------------------------------------------------------

void TestFormFinger::allButtonsExist()
{
    Fixture f;
    QVERIFY(f.up());
    QVERIFY(f.button("addface") != nullptr);
    QVERIFY(f.button("addfinger") != nullptr);
    QVERIFY(f.button("addquit") != nullptr);
    QVERIFY(f.label() != nullptr);
}

void TestFormFinger::buttonsDoNotTakeFocus()
{
    // 门口机是触摸屏：抢焦点会让按下态显示异常。构造函数里显式设了 NoFocus。
    Fixture f;
    QVERIFY(f.up());
    QCOMPARE(f.button("addface")->focusPolicy(), Qt::NoFocus);
    QCOMPARE(f.button("addfinger")->focusPolicy(), Qt::NoFocus);
    QCOMPARE(f.button("addquit")->focusPolicy(), Qt::NoFocus);
}

// ---------------------------------------------------------------------------
// 信号接线
// ---------------------------------------------------------------------------

void TestFormFinger::addFaceButtonEmitsAddFaceRequested()
{
    Fixture f;
    QVERIFY(f.up());
    QSignalSpy spy(f.form, &Form_finger::addFaceRequested);
    QCOMPARE(spy.count(), 0);
    f.button("addface")->click();
    QCOMPARE(spy.count(), 1);
}

void TestFormFinger::addFingerButtonStartsEnrollmentNotAddFace()
{
    // "添加指纹"与"添加人脸"是两个完全不同的动作，接线接错会让现场点错。
    Fixture f;
    QVERIFY(f.up());
    QSignalSpy faceSpy(f.form, &Form_finger::addFaceRequested);
    f.clickAddFinger();
    QCOMPARE(faceSpy.count(), 0);
    // 它应当进入注册：挂起常驻识别（下条用例细验）。
    QVERIFY2(f.fpMgr->isSuspended(),
             "点『添加指纹』后必须挂起常驻识别（两者抢同一个串口）");
}

void TestFormFinger::quitButtonDoesNotEmitAddFace()
{
    Fixture f;
    QVERIFY(f.up());
    QSignalSpy faceSpy(f.form, &Form_finger::addFaceRequested);
    f.clickQuit();
    QCOMPARE(faceSpy.count(), 0);
}

void TestFormFinger::faceButtonClickIsNotDuplicatedByAutoConnect()
{
    // 第 35 轮在 form_passwd 上发现的真实缺陷形态：.ui 里若有 <connections>
    // 块，uic 生成的 setupUi() 会调 connectSlotsByName() 自动连线，与显式
    // connect 叠加会把同一个槽跑两遍（退格一次删两位）。
    //
    // form_finger.ui 的 <connections/> 是空的，且本类没有 on_<对象名>_clicked
    // 形式的槽（槽名是 addFingerRequested / onAddQuitClicked），所以不该
    // 发生。这里直接验证：一次点击只发一次信号。
    Fixture f;
    QVERIFY(f.up());
    QSignalSpy spy(f.form, &Form_finger::addFaceRequested);
    f.button("addface")->click();
    QCOMPARE(spy.count(), 1);
    QVERIFY2(spy.count() != 2,
             "一次点击触发了两次 addFaceRequested —— 疑似 connectSlotsByName "
             "与显式 connect 叠加");
}

// ---------------------------------------------------------------------------
// 串口互斥（本文件最重要的性质）
// ---------------------------------------------------------------------------
//
// 失败模式：注册流程与常驻识别共用同一个串口。识别轮询每 500ms 下发
// CMD_GET_IMAGE 并抢读应答，会把注册流程等待的应答取走，注册随机失败；
// 反过来也一样。所以注册期间必须挂起识别，退出时**成对恢复**。
//
// 漏了恢复的后果特别严重：门禁指纹识别**永久失效**，而命令字节一个都没变
// —— 静态规则完全看不出来。

void TestFormFinger::enrollingSuspendsTheResidentIdentifier()
{
    Fixture f;
    QVERIFY(f.up());
    QVERIFY2(!f.fpMgr->isSuspended(), "初始状态不应是挂起");

    f.clickAddFinger();

    QVERIFY2(f.fpMgr->isSuspended(),
             "进入注册后必须挂起常驻识别，否则两者会抢同一个串口的应答，"
             "注册随机失败（见 FingerprintManager::setSuspended 的说明）");
}

void TestFormFinger::quitResumesTheResidentIdentifier()
{
    Fixture f;
    QVERIFY(f.up());
    f.clickAddFinger();
    QVERIFY(f.fpMgr->isSuspended());

    f.clickQuit();

    QVERIFY2(!f.fpMgr->isSuspended(),
             "点『退出』后必须恢复常驻识别。漏了这一步，门禁指纹识别会"
             "**永久失效** —— 而命令字节一个都没变，静态规则看不出来");
}

void TestFormFinger::destructionResumesTheResidentIdentifier()
{
    // 兜底路径：窗口被父窗口直接销毁，没走 onAddQuitClicked()。
    // 头文件注释明确说"若窗口是被父窗口直接销毁，也必须把识别轮询放出来，
    // 否则门口机的指纹识别会永久失效"。
    //
    // 注意本用例**手动** delete 窗体（此时管理器还活着），因此它验证的是
    // "析构里的恢复调用确实发生"。而"管理器已经先死"的那条更危险的路径由
    // destructionAfterEnrollmentIsSafe 覆盖 —— 两者不同，都要有。
    Fixture f;
    QVERIFY(f.up());
    f.clickAddFinger();
    QVERIFY(f.fpMgr->isSuspended());

    delete f.form;
    f.form = nullptr;

    QVERIFY2(!f.fpMgr->isSuspended(),
             "析构也必须恢复常驻识别（兜底路径），否则识别永久失效");
}

void TestFormFinger::enterExitTwiceKeepsSuspensionPaired()
{
    // 只在**第一次**进入注册时记录被挂起的句柄；退出时清空句柄。
    // 反复进出不应让状态错乱 —— 若句柄没被清空，"成对恢复"就会退化成
    // "第一次退出时恢复、之后再也恢复不了"。
    Fixture f;
    QVERIFY(f.up());
    f.clickAddFinger();
    QVERIFY(f.fpMgr->isSuspended());
    f.clickQuit();
    QVERIFY(!f.fpMgr->isSuspended());

    // 第二轮：句柄已被清空，守卫必须重新成立。
    f.clickAddFinger();
    QVERIFY2(f.fpMgr->isSuspended(), "第二轮注册必须重新挂起识别");
    f.clickQuit();
    QVERIFY2(!f.fpMgr->isSuspended(), "第二轮退出必须重新恢复识别");

    // 第三轮，确认不是"只有前两次能用"。
    f.clickAddFinger();
    QVERIFY(f.fpMgr->isSuspended());
    f.clickQuit();
    QVERIFY(!f.fpMgr->isSuspended());
}

void TestFormFinger::repeatedQuitDoesNotConfuseSuspension()
{
    // 连点两次『退出』：第二次没有可恢复的句柄，必须安全跳过。
    Fixture f;
    QVERIFY(f.up());
    f.clickAddFinger();
    f.clickQuit();
    QVERIFY(!f.fpMgr->isSuspended());
    f.clickQuit();
    QVERIFY2(!f.fpMgr->isSuspended(), "重复退出不应把状态弄乱");
}

// ---------------------------------------------------------------------------
// 生命周期：管理器先于窗体销毁（use-after-free 回归测试）
// ---------------------------------------------------------------------------

void TestFormFinger::managerIsDestroyedBeforeTheFormProvesTheHazardIsReal()
{
    // ========================================================================
    // 这条用例钉住的是那个 use-after-free 的**成因**，而不是 UB 本身。
    //
    // 为什么不去断言"崩溃"：我先写的就是"不点退出、直接析构，看它崩不崩"，
    // 故障注入实测**变异体 5/5 都没崩**（退出码全 0）。原因是 use-after-free
    // 属于未定义行为，而 Qt 的对象析构只是把内存还回分配器；在"刚释放、
    // 还没被复用"的窗口里，那块内存往往仍能读出原来的 vtable 与字段，
    // `setSuspended()` 走完看着一切正常。**一条在缺陷代码上不失败的回归测试
    // 是没有价值的**，所以它不能作为守卫（本仓库的既有约定）。
    //
    // 于是改为钉住可确定复现的那半：**Qt 按创建顺序销毁子对象（先建先死）**。
    // 生产代码里管理器在 widget.cpp:250 创建、窗体在 widget.cpp:670 创建，
    // 所以 `delete Widget` 时管理器先死 —— 这正是悬垂指针得以成立的前提。
    // 一旦这个顺序变了（或有人以为它是逆序），本用例立刻失败，提醒复核
    // form_finger 的守卫是否还成立。
    //
    // 至于"守卫本身有没有写对"（成员必须是 QPointer 而不是裸指针），那是一个
    // **声明层面的文本性质**，由 verify.py 的静态规则
    // check_form_finger_suspended_manager_is_guarded 逐字钉住，并有故障注入
    // 自检（把 QPointer 改回裸指针必须被抓到）。两者分工：本用例守"前提"，
    // 静态规则守"做法"。
    // ========================================================================
    TeardownProbe::reset();
    {
        QWidget root;
        // 复刻生产创建顺序：管理器先建，窗体后建。
        new ProbeChild(QStringLiteral("manager"), &root);   // widget.cpp:250
        new ProbeChild(QStringLiteral("form"), &root);      // widget.cpp:670
        QCOMPARE(TeardownProbe::log().size(), 0);           // 还没销毁
    }

    const QStringList& log = TeardownProbe::log();
    QCOMPARE(log.size(), 2);
    const int iMgr = log.indexOf(QStringLiteral("manager DTOR"));
    const int iForm = log.indexOf(QStringLiteral("form DTOR"));
    QVERIFY2(iMgr >= 0, "管理器应当被销毁");
    QVERIFY2(iForm >= 0, "窗体应当被销毁");
    QVERIFY2(iMgr < iForm,
             "Qt 应当**先**销毁先创建的子对象（管理器），后创建者（窗体）最后死。"
             "若此断言失败，说明销毁顺序已变，form_finger 里『窗体析构时管理器"
             "可能已经死了』这一判断需要重新复核。");
    QVERIFY2(iForm == log.size() - 1,
             "窗体应当是最后一个被销毁的子对象（这正是它析构时管理器已死的原因）");
}

// ---------------------------------------------------------------------------
// 轮询定时器生命周期
// ---------------------------------------------------------------------------

void TestFormFinger::quitStopsThePollTimer()
{
    // 轮询回调里的 readPacket() 是**阻塞读**且跑在 GUI 线程上。窗口只是
    // close() 隐藏（Form_finger 没有 WA_DeleteOnClose，对象仍活着），
    // 若不停表，它会继续每 500ms 阻塞一次 GUI，把界面拖死。
    Fixture f;
    QVERIFY(f.up());
    f.clickAddFinger();
    // 等到确实开始轮询（有命令下发）。
    QVERIFY2(waitFor([&f] { return fakefp::sentCount() > 0; }),
             "点『添加指纹』后应当开始轮询探手指");

    f.clickQuit();

    // 退出会把常驻识别恢复起来，它自己也会发命令 —— 先静音，否则计数
    // 混入管理器的合法流量，断言会误报（见文件头"夹具细节"）。
    f.muteResidentIdentifier();
    f.assertNoFurtherTraffic();
}

void TestFormFinger::destructionStopsThePollTimer()
{
    // 析构必须停表。**不能**用"删掉之后等一会儿看计数涨没涨"来判（第一版就是
    // 这么写的，故障注入实测**抓不到**把析构里 stopFingerPoll 删掉的变异）：
    // 析构函数会把常驻识别**恢复**（那是它的职责），而识别自己每 500ms 也发
    // 一条 CMD_GET_IMAGE —— 计数变化分不清是"窗体还在轮询"还是"识别被恢复"。
    //
    // 正确做法：析构前先挂起常驻识别，析构后**立刻再挂起**（把析构触发的那次
    // 延后恢复变成空操作，manager 用 singleShot(0,...) 避免重入）。此后该假
    // 串口上不可能再出现流量，除非窗体自己的定时器还活着 —— 那正是要抓的。
    Fixture f;
    QVERIFY(f.up());
    f.fpMgr->setSuspended(true);      // 静音常驻识别
    QTest::qWait(700);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 150);

    f.clickAddFinger();               // 窗体开始轮询（识别已被挂起）
    QVERIFY(waitFor([&f] { return fakefp::sentCount() > 0; }));

    delete f.form;
    f.form = nullptr;

    f.fpMgr->setSuspended(true);      // 把析构触发的那次恢复压回去

    const int base = fakefp::sentCount();
    QTest::qWait(1300);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 150);
    QCOMPARE(fakefp::sentCount(), base);
}

void TestFormFinger::repeatedAddFingerDoesNotStackPollTimers()
{
    // 复用同一个成员定时器：重复进入 startFingerRegisterStep() 不应叠加。
    // 但 addFingerRequested() 有 waitingFinger 守卫，第二次点击会被忽略 ——
    // 所以点两次，断言命令下发**速率**没有翻倍（即只有一个定时器在跑）。
    //
    // 此处常驻识别处于挂起状态（点『添加指纹』时挂起的），不会发命令，
    // 因此 sentCount() 只反映窗体自己的轮询，不需要静音。
    Fixture f;
    QVERIFY(f.up());
    // 不回任何应答，让轮询一直空转（每 500ms 一条 CMD_GET_IMAGE）。
    f.clickAddFinger();
    f.clickAddFinger();   // 应被 waitingFinger 守卫挡住

    QVERIFY(waitFor([&f] { return fakefp::sentCount() >= 1; }));
    QTest::qWait(1150);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 120);
    const int n = fakefp::sentCount();

    // 一只定时器 500ms 一轮：约 2.2 秒里最多 5 条。两只叠加会到 ~10 条。
    // 用上界判定，不依赖精确时序（等性质，不等固定时长）。
    QVERIFY2(n <= 7,
             qPrintable(QStringLiteral(
                 "轮询定时器疑似叠加了：约 1150 ms 内下发 %1 条命令").arg(n)));
}

// ---------------------------------------------------------------------------
// 拒绝重入
// ---------------------------------------------------------------------------

void TestFormFinger::secondAddFingerClickWhileWaitingIsIgnored()
{
    // addFingerRequested() 开头 `if (waitingFinger) return;`（防重复点击）。
    // 观测：第二次点击不应重新播放第一步的采集提示音。
    Fixture f;
    QVERIFY(f.up());
    f.clickAddFinger();
    QVERIFY(f.fpMgr->isSuspended());

    f.mgr.resetForTest();
    f.clickAddFinger();     // 应被忽略

    QCOMPARE(f.mgr.playAsyncCalls, 0);
}

// ---------------------------------------------------------------------------
// 命令序列（与静态规则互为印证）
// ---------------------------------------------------------------------------

void TestFormFinger::pollSendsGetImageWith500msTimeout()
{
    // 轮询探手指：CMD_GET_IMAGE(0x01)，readPacket(500)。
    // 静态规则 check_live_fingerprint_enroll_sequence_is_pinned 钉住了
    // "B:0x01 T:500" 这个序列文本；这里跑一次，验证它真的下发到串口。
    Fixture f;
    QVERIFY(f.up());
    f.clickAddFinger();

    QVERIFY2(waitFor([&f] { return fakefp::sentCount() >= 1; }),
             "应当下发探手指命令");

    // 第一条命令的 payload 应当是 0x01。
    QCOMPARE(fakefp::sentPayload(0), QByteArray(1, char(0x01)));
    QVERIFY(fakefp::readTimeouts().contains(500));
}

void TestFormFinger::collectStepSendsGetImageThenGenChar()
{
    // 探到手指后：先 GET_IMAGE(2000) 再 GEN_CHAR(step)(2000)。
    // 这是与 baseline 逐字节一致的序列（第 23 轮实测：两处各 9 个 append）。
    Fixture f;
    QVERIFY(f.up());
    f.answerSuccess();      // 轮询与采集都能拿到"成功"
    f.clickAddFinger();

    // 等到 GEN_CHAR 下发（payload 以 0x02 开头）。
    QVERIFY2(waitFor([&f] {
                 for (int i = 0; i < fakefp::sentCount(); ++i) {
                     const QByteArray p = fakefp::sentPayload(i);
                     if (p.size() >= 1 && quint8(p.at(0)) == 0x02) {
                         return true;
                     }
                 }
                 return false;
             }),
             "探到手指后应当下发 GEN_CHAR(0x02)");

    // 第一条 GEN_CHAR 的第二个字节应当是 step=1（BufferID）。
    for (int i = 0; i < fakefp::sentCount(); ++i) {
        const QByteArray p = fakefp::sentPayload(i);
        if (p.size() >= 2 && quint8(p.at(0)) == 0x02) {
            QCOMPARE(int(quint8(p.at(1))), 1);
            break;
        }
    }

    // 采集与生成特征必须给 2000ms（500 是轮询专用）。
    QVERIFY2(fakefp::readTimeouts().contains(2000),
             "采集/生成特征必须用 2000ms 超时");
}

void TestFormFinger::fullEnrollSequenceMatchesThePinnedContract()
{
    // ========================================================================
    // 整条注册流程的**完整线上字节序列**断言。
    //
    // 为什么需要它（第 37 轮，故障注入实测发现的三个假绿）：
    // 原先的断言是分散的、存在性的 —— "某处出现过 0x02"、"某处超时是 2000"。
    // 故障注入证明这类断言抓不到下面三种退化：
    //   - 把采集前的 GET_IMAGE 从 0x01 改成 0x03（不存在的命令）：没抓到，
    //     因为断言只找 0x02；
    //   - 把采集超时从 2000 改成 500：没抓到，因为 `contains(2000)` 被轮询
    //     之后那些 2000 满足了；
    //   - 干脆不下发 GEN_CHAR：没抓到，因为断言是"等到出现 0x02 就返回"，
    //     而那正是被删掉的东西 —— 典型的把"等待"当成"断言"。
    //
    // 改成**按顺序、逐条、精确比对整条序列**之后，上面三种都被抓到，
    // 顺序本身也被钉住（命令顺序错了，模块行为完全不同）。
    //
    // 序列来源：第 37 轮用**探针实测**（临时把 sentPayload 与 readTimeouts
    // 打出来，跑完即删），不是读代码猜的。与静态规则
    // check_live_fingerprint_enroll_sequence_is_pinned 钉住的契约一致。
    //
    // **注意**：每一步实际有**两条** GET_IMAGE —— 一条来自"检测手指"轮询
    // （500ms 超时），一条来自 doFingerCollect() 自己再采集一次（2000ms 超时）。
    // 这是与 baseline 逐字节一致的既有行为，不是重复下发。
    // 实测序列（共 11 条）：
    //   01 | 01 02 01 | 01 02 02 | 01 02 03 | 05 | 06 01 12 34
    //   ^轮询 ^步骤1     ^步骤2     ^步骤3    ^合并  ^存储
    //   超时：500 | 2000 2000 | 2000 2000 | 2000 2000 | 2000 | 2000
    // ========================================================================
    Fixture f;
    QVERIFY(f.up());
    f.mainWin.setNextFingerIdForTest(0x1234);
    f.answerSuccess(256);
    f.clickAddFinger();

    QVERIFY2(waitFor([&f] {
                 for (int i = 0; i < fakefp::sentCount(); ++i) {
                     const QByteArray p = fakefp::sentPayload(i);
                     if (p.size() >= 1 && quint8(p.at(0)) == 0x06) return true;
                 }
                 return false;
             }, 20000),
             "注册应当走完并下发 STORE_CHAR(0x06)");

    QStringList gotPayloads;
    for (int i = 0; i < fakefp::sentCount(); ++i) {
        const QByteArray p = fakefp::sentPayload(i);
        QString hex;
        for (int k = 0; k < p.size(); ++k) {
            if (k) hex += QLatin1Char(' ');
            hex += QStringLiteral("%1").arg(quint8(p.at(k)), 2, 16,
                                            QLatin1Char('0'));
        }
        gotPayloads << hex;
    }
    const QStringList wantPayloads = QStringList()
        << QStringLiteral("01")                 // 轮询探手指(500ms)
        << QStringLiteral("01")                 // 步骤1 采集
        << QStringLiteral("02 01")              // 步骤1 生成特征 BufferID=1
        << QStringLiteral("01")                 // 步骤2 轮询探手指
        << QStringLiteral("01")                 // 步骤2 采集
        << QStringLiteral("02 02")              // 步骤2 生成特征 BufferID=2
        << QStringLiteral("01")                 // 步骤3 轮询探手指
        << QStringLiteral("01")                 // 步骤3 采集
        << QStringLiteral("02 03")              // 步骤3 生成特征 BufferID=3
        << QStringLiteral("05")                 // 合并模板
        << QStringLiteral("06 01 12 34");       // 存储：BufferID=1, id=0x1234 大端
    QCOMPARE(gotPayloads, wantPayloads);

    // 超时序列也要精确：每次"检测手指"轮询用 500，其余一律 2000。
    const QList<int> wantTimeouts = QList<int>()
        << 500 << 2000 << 2000
        << 500 << 2000 << 2000
        << 500 << 2000 << 2000
        << 2000 << 2000;
    QCOMPARE(fakefp::readTimeouts(), wantTimeouts);
}

void TestFormFinger::collectingSuccessPlaysTheStepTone()
{
    // 第 1 步采集成功播放 fingerone.pcm（路径跟随 AppConfig 的 PCM 目录）。
    Fixture f;
    QVERIFY(f.up());
    f.answerSuccess();
    f.mgr.resetForTest();
    f.clickAddFinger();

    QVERIFY2(waitFor([&f] {
                 for (int i = 0; i < f.mgr.playedPaths.size(); ++i) {
                     if (f.mgr.playedPaths.at(i).endsWith(
                             QStringLiteral("fingerone.pcm"))) {
                         return true;
                     }
                 }
                 return false;
             }),
             "第 1 次采集成功后应播放 fingerone.pcm");

    // 提示音路径必须来自 AppConfig 的 pcm 目录（不是散落的字面量）。
    const QString pcmDir =
        accessterminal::AppConfig::instance().pcmDir();
    for (int i = 0; i < f.mgr.playedPaths.size(); ++i) {
        const QString p = f.mgr.playedPaths.at(i);
        if (p.endsWith(QStringLiteral("fingerone.pcm"))) {
            QVERIFY2(p.startsWith(pcmDir),
                     qPrintable(QStringLiteral(
                         "提示音路径应位于配置的 PCM 目录 %1，实际 %2")
                                    .arg(pcmDir, p)));
        }
    }
}

void TestFormFinger::collectFailureDoesNotAdvance()
{
    // 采集图像失败时不得下发 GEN_CHAR —— 采集都没成功，谈不上生成特征。
    // 这里只让**第一次** readPacket（轮询）成功，之后的采集拿不到应答，
    // 于是精确落在"探到手指、采集失败"这条路径上。
    Fixture f;
    QVERIFY(f.up());
    f.answerSuccessOnce();
    f.clickAddFinger();

    QVERIFY2(waitFor([&f] { return fakefp::sentCount() >= 1; }),
             "应当先下发探手指命令");
    // 等它把采集也走完（采集 readPacket 返回空 => 失败）。
    QTest::qWait(400);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 120);

    for (int i = 0; i < fakefp::sentCount(); ++i) {
        const QByteArray p = fakefp::sentPayload(i);
        QVERIFY2(!(p.size() >= 1 && quint8(p.at(0)) == 0x02),
                 "采集失败时不应下发 GEN_CHAR");
    }
}

// ---------------------------------------------------------------------------
// 失败后可重试
// ---------------------------------------------------------------------------

void TestFormFinger::failedCollectClearsWaitingSoUserCanRetry()
{
    // 失败路径必须把 waitingFinger 清掉，否则用户再点"添加指纹"会被
    // `if (waitingFinger) return;` 永久挡住 —— 现场表现是"点了没反应"。
    Fixture f;
    QVERIFY(f.up());
    f.answerSuccessOnce();      // 探到手指，但采集失败
    f.clickAddFinger();
    QVERIFY(waitFor([&f] { return fakefp::sentCount() >= 1; }));
    // 跑到采集失败那条路径（handle 失败会清 waitingFinger）。
    QTest::qWait(400);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 120);

    // 再点一次：若 waitingFinger 没被清，这次点击会被忽略、不再下发命令。
    const int before = fakefp::sentCount();
    f.clickAddFinger();

    QVERIFY2(waitFor([&f, before] {
                 return fakefp::sentCount() > before;
             }),
             "采集失败后必须允许用户重新发起注册（waitingFinger 要被清掉），"
             "否则现场表现为『点了添加指纹没反应』");
}

// ---------------------------------------------------------------------------
// 指纹库 ID
// ---------------------------------------------------------------------------

void TestFormFinger::successfulEnrollIncrementsTheNextFingerId()
{
    // 存储成功（0x06 收到 0x00）后必须 incrementNextFingerId()，
    // 否则下一个用户会覆盖刚注册的模板（同一个 ID 被反复写）。
    Fixture f;
    QVERIFY(f.up());
    QVERIFY(f.mainWin.nextFingerIdForTest() == 1);
    f.answerSuccess(256);       // 三步全部成功
    f.clickAddFinger();

    QVERIFY2(waitFor([&f] {
                 return f.mainWin.nextFingerIdForTest() > 1;
             }, 15000),
             "注册成功后指纹库 ID 必须递增，否则下一个用户会覆盖刚注册的模板");
}

void TestFormFinger::storeCharCarriesTheNextFingerIdBigEndian()
{
    // STORE_CHAR(0x06) 的 payload 是 (0x06, BufferID=1, id_hi, id_lo)。
    // 字节序写错会让模板存到错误的槽位。
    Fixture f;
    QVERIFY(f.up());
    f.mainWin.setNextFingerIdForTest(0x1234);
    f.answerSuccess(256);
    f.clickAddFinger();

    QVERIFY2(waitFor([&f] {
                 for (int i = 0; i < fakefp::sentCount(); ++i) {
                     const QByteArray p = fakefp::sentPayload(i);
                     if (p.size() >= 4 && quint8(p.at(0)) == 0x06) {
                         return true;
                     }
                 }
                 return false;
             }, 15000),
             "三段采集成功后应当下发 STORE_CHAR(0x06)");

    for (int i = 0; i < fakefp::sentCount(); ++i) {
        const QByteArray p = fakefp::sentPayload(i);
        if (p.size() >= 4 && quint8(p.at(0)) == 0x06) {
            QCOMPARE(int(quint8(p.at(1))), 1);        // BufferID
            QCOMPARE(int(quint8(p.at(2))), 0x12);     // id 高字节
            QCOMPARE(int(quint8(p.at(3))), 0x34);     // id 低字节
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 正对照
// ---------------------------------------------------------------------------

void TestFormFinger::fakeDeviceDeliversBytesToReadPacket()
{
    // 上面所有"设备收到了什么 / 读到什么"的断言都依赖假串口真的在工作。
    // 若替身坏了，那些断言会真空成立。这里直接验证替身本身。
    fakefp::reset();
    HailinFingerprint dev(QStringLiteral("/dev/null"));
    fakefp::pushConfirm(0x00);
    const QByteArray got = dev.readPacket(100);
    QVERIFY2(got.size() >= 12,
             "假设备应当交付一个完整应答帧（正对照）");
    QCOMPARE(int(quint8(got.at(9))), 0x00);

    // buildPacket 是真实算法：包头 0xEF01、地址 FFFFFFFF、包标识 0x01。
    const QByteArray pkt = dev.buildPacket(QByteArray(1, char(0x01)));
    QCOMPARE(int(quint8(pkt.at(0))), 0xEF);
    QCOMPARE(int(quint8(pkt.at(1))), 0x01);
    QCOMPARE(int(quint8(pkt.at(6))), 0x01);   // PKT_CMD
}

QTEST_MAIN(TestFormFinger)
#include "test_form_finger.moc"
