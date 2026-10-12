/**
 * @file test_access_controller.cpp
 * @brief AccessController（唯一的放行入口）的行为断言。
 *
 * 为什么必须有这一组
 * ------------------
 * 第 40 轮把"谁能进门"从 `Widget` 的四个 lambda/槽收敛到本类。收敛之后，
 * 有三件事**只有这里能验**，而且错了都是现场事故：
 *
 *   1. **静默期为 0 时必须完全不抑制** —— 这是默认值，也是"保持现状"的
 *      承诺；一旦实现成"总是抑制"或"用了未初始化的计时器"，现场会变成
 *      "第一个人进得去，后面的人再也进不去"。
 *   2. **静默期大于 0 时必须真的抑制，并且到期自动恢复** —— 这是给
 *      "门口站着的人被 3Hz 人脸识别反复开门"准备的能力（默认不启用）。
 *   3. **被抑制的请求不得触达执行器** —— 否则"抑制"只是记账，门照开。
 *
 * 测试方式：真 AccessController + 真 DoorActuator + 真 GpioLine +
 * 可编程假 libgpiod，断言落在"假实现收到的电平调用"与放行计数上。
 */
#include <QtTest/QtTest>

#include <QSignalSpy>

#include "hal/gpio.h"
#include "managers/access_controller.h"
#include "managers/door_actuator.h"

#include "fake_gpiod.h"

using accessterminal::AccessController;
using accessterminal::AccessMethod;
using accessterminal::DoorActuator;
using accessterminal::GpioLine;

namespace {

/// 组装一套"接了线的执行器 + 放行中枢"。
struct Rig {
    GpioLine line;
    DoorActuator actuator;
    AccessController controller;

    bool open()
    {
        if (!line.openOutput(QStringLiteral("gpiochip0"), 133,
                             QStringLiteral("test"), 0)) {
            return false;
        }
        actuator.setLine(&line);
        controller.setActuator(&actuator);
        return true;
    }
};

} // namespace

class TestAccessController : public QObject
{
    Q_OBJECT

private slots:
    void init() { fakegpiod::reset(); }

    // ---- 1. 默认（静默期 0）：不抑制任何请求 ----

    void zeroSilentPeriodNeverSuppresses()
    {
        Rig rig;
        QVERIFY(rig.open());
        QCOMPARE(rig.controller.silentPeriodMs(), 0);

        QVERIFY(rig.controller.request(AccessMethod::Password, QStringLiteral("密码")));
        QVERIFY2(!rig.controller.isInSilentPeriod(),
                 "静默期为 0 时不得进入静默期");
        QCOMPARE(rig.controller.silentRemainingMs(), 0);

        // 紧接着再来一次：静默期为 0 时必须照样受理
        QVERIFY2(rig.controller.request(AccessMethod::Password, QStringLiteral("密码")),
                 "静默期为 0 时第二次请求也必须被受理（保持现状）");
        QCOMPARE(rig.controller.grantedCount(), 2);
        QCOMPARE(rig.controller.suppressedCount(), 0);

        // 执行器只被"第一次脉冲"驱动（第二次落在脉冲进行中，由执行器自己拒绝），
        // 但放行计数必须是 2 —— 两件事分开记账，互不掩盖。
        QCOMPARE(fakegpiod::setValueCount(), 1);
    }

    // ---- 2. 静默期 > 0：抑制、可见、到期恢复 ----

    void silentPeriodSuppressesThenRecovers()
    {
        Rig rig;
        QVERIFY(rig.open());
        rig.controller.setSilentPeriodMs(200);

        QSignalSpy granted(&rig.controller, &AccessController::granted);
        QSignalSpy suppressed(&rig.controller, &AccessController::suppressed);

        QVERIFY(rig.controller.request(AccessMethod::Face, QStringLiteral("人脸")));
        QCOMPARE(granted.count(), 1);

        QVERIFY2(!rig.controller.request(AccessMethod::Face, QStringLiteral("人脸")),
                 "静默期内的第二次请求必须被抑制");
        QCOMPARE(suppressed.count(), 1);
        QCOMPARE(rig.controller.suppressedCount(), 1);
        const qint64 remaining = suppressed.at(0).at(2).toLongLong();
        QVERIFY2(remaining > 0 && remaining <= 200,
                 qPrintable(QStringLiteral("剩余毫秒数不合理: %1").arg(remaining)));

        // 被抑制的请求**不得**触达执行器：电平调用次数保持 1
        QCOMPARE(fakegpiod::setValueCount(), 1);

        // 到期后自动恢复
        QTRY_VERIFY_WITH_TIMEOUT(!rig.controller.isInSilentPeriod(), 3000);
        QVERIFY(rig.controller.request(AccessMethod::Face, QStringLiteral("人脸")));
        QCOMPARE(rig.controller.grantedCount(), 2);
    }

    // ---- 3. 静默期的出口兜底 ----

    void silentPeriodIsClampedAtTheExit()
    {
        AccessController c;
        c.setSilentPeriodMs(-1);
        QCOMPARE(c.silentPeriodMs(), 0);
        c.setSilentPeriodMs(0);
        QCOMPARE(c.silentPeriodMs(), 0);
        c.setSilentPeriodMs(3000);
        QCOMPARE(c.silentPeriodMs(), 3000);
        c.setSilentPeriodMs(999999);
        QCOMPARE(c.silentPeriodMs(), 60000);
    }

    // ---- 4. 执行器未接：仍然记账，但门不会动 ----

    void requestWithoutActuatorStillAccounts()
    {
        AccessController c;   // 不装配执行器
        QSignalSpy actuated(&c, &AccessController::actuated);
        QVERIFY(c.request(AccessMethod::Remote, QStringLiteral("远程")));
        QCOMPARE(c.grantedCount(), 1);
        QCOMPARE(actuated.count(), 0);       // 没有任何电气动作
        QCOMPARE(fakegpiod::setValueCount(), 0);
    }

    // ---- 5. 执行器的真实/模拟结果被转发出来 ----

    void actuatedCarriesRealActuatorFlag()
    {
        AccessController c;
        DoorActuator actuator;               // 刻意不接线 = 模拟开锁
        c.setActuator(&actuator);
        QSignalSpy actuated(&c, &AccessController::actuated);

        QVERIFY(c.request(AccessMethod::Password, QStringLiteral("密码")));
        QCOMPARE(actuated.count(), 1);
        QCOMPARE(actuated.at(0).at(0).toString(), QStringLiteral("密码"));
        QCOMPARE(actuated.at(0).at(1).toBool(), false);  // 模拟开锁，必须可见
    }

    // ---- 6. 方式名字（日志与信号都用它） ----

    void methodTextIsComplete()
    {
        QCOMPARE(AccessController::methodText(AccessMethod::Password),
                 QStringLiteral("密码"));
        QCOMPARE(AccessController::methodText(AccessMethod::Fingerprint),
                 QStringLiteral("指纹"));
        QCOMPARE(AccessController::methodText(AccessMethod::Face),
                 QStringLiteral("人脸"));
        QCOMPARE(AccessController::methodText(AccessMethod::Remote),
                 QStringLiteral("远程"));
    }

    // ==== 连续命中平滑（方案 P4：防假阳性）====
    //
    // 背景：本项目已实测确认 LBPH 在单 label 下 predictedLabel == 1 恒为真，
    // 距离阈值是**唯一的门**。一次凑巧落进阈值的误识别就足以开门。
    // "窗口内连续命中 N 次"把单帧误判变成需要连续 N 次误判。

    // ---- 7. 默认 required == 1：**必须完全不改变行为** ----

    void streakDefaultsToOneAndChangesNothing()
    {
        AccessController c;
        for (int m = 0; m < 4; ++m) {
            QCOMPARE(c.requiredStreak(static_cast<AccessMethod>(m)), 1);
            QCOMPARE(c.pendingStreak(static_cast<AccessMethod>(m)), 0);
        }
        QCOMPARE(c.pendingCount(), 0);

        // 默认下连续请求必须**全部**受理（保持现状的承诺）。
        QSignalSpy progress(&c, &AccessController::streakProgress);
        for (int i = 0; i < 5; ++i) {
            QVERIFY2(c.request(AccessMethod::Face, QStringLiteral("人脸")),
                     "required==1 时不得有任何平滑延迟");
        }
        QCOMPARE(c.grantedCount(), 5);
        QCOMPARE(progress.count(), 0);
    }

    // ---- 8. required == N：前 N-1 次暂缓，第 N 次放行 ----

    void streakRequiresConsecutiveHits()
    {
        AccessController c;
        c.setRequiredStreak(AccessMethod::Face, 3, 5000);
        QCOMPARE(c.requiredStreak(AccessMethod::Face), 3);

        QSignalSpy granted(&c, &AccessController::granted);
        QSignalSpy progress(&c, &AccessController::streakProgress);

        QVERIFY2(!c.request(AccessMethod::Face, QStringLiteral("人脸")), "第 1 次应暂缓");
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 1);
        QVERIFY2(!c.request(AccessMethod::Face, QStringLiteral("人脸")), "第 2 次应暂缓");
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 2);

        QVERIFY2(c.request(AccessMethod::Face, QStringLiteral("人脸")),
                 "第 3 次必须放行");
        QCOMPARE(c.grantedCount(), 1);
        QCOMPARE(granted.count(), 1);

        // 放行后进度清零：下一次要重新攒 3 次。
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 0);
        QCOMPARE(c.pendingCount(), 2);
        QCOMPARE(progress.count(), 2);
        QCOMPARE(progress.at(0).at(2).toInt(), 1);
        QCOMPARE(progress.at(0).at(3).toInt(), 3);
        QCOMPARE(progress.at(1).at(2).toInt(), 2);
    }

    // ---- 9. **窗口从第一次命中起算**（判别力核心）----
    //
    // 若改成"从最近一次起算"，只要命中间隔小于窗口就能无限累积：
    // 一次持续误检（或一个人干站着）迟早会攒够 N 次，门等于没锁。
    // 本用例用慢速滴灌证明窗口是硬截止。

    void streakWindowStartsAtFirstHitNotLast()
    {
        AccessController c;
        c.setRequiredStreak(AccessMethod::Face, 3, 120);   // 窗口 120ms

        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 1);

        // 每次间隔 ~90ms（小于窗口）。若窗口从**最近一次**起算，三次之后
        // 就会攒满放行；从**第一次**起算则第一次命中后 120ms 硬截止，
        // 第 3 次（约 180ms 后）必须已经重置。
        QTest::qWait(90);
        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QTest::qWait(90);
        QVERIFY2(!c.request(AccessMethod::Face, QStringLiteral("人脸")),
                 "窗口应从第一次命中起算：慢速滴灌不得攒满而放行");
        QCOMPARE(c.grantedCount(), 0);
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 1);   // 已重置为新的第 1 次

        // 阳性对照：窗口内快速连击仍然正常放行（说明不是"永远不放行"）。
        AccessController c2;
        c2.setRequiredStreak(AccessMethod::Face, 3, 5000);
        QVERIFY(!c2.request(AccessMethod::Face, QStringLiteral("人脸")));
        QVERIFY(!c2.request(AccessMethod::Face, QStringLiteral("人脸")));
        QVERIFY2(c2.request(AccessMethod::Face, QStringLiteral("人脸")),
                 "窗口内连击 3 次必须放行");
    }

    // ---- 10. 按方式独立：给人脸加严**不得**影响其它三条路径 ----
    //
    // 这是把"连续命中"做成 per-method 而不是全局开关的理由。若做成全局，
    // 运维为加严人脸而设 requiredStreak=2，会顺带让"密码输一次不开门"。

    void streakIsPerMethodAndDoesNotLeak()
    {
        AccessController c;
        c.setRequiredStreak(AccessMethod::Face, 3, 5000);

        QCOMPARE(c.requiredStreak(AccessMethod::Face), 3);
        QCOMPARE(c.requiredStreak(AccessMethod::Password), 1);
        QCOMPARE(c.requiredStreak(AccessMethod::Fingerprint), 1);
        QCOMPARE(c.requiredStreak(AccessMethod::Remote), 1);

        // 其余三条路径必须**一次就放行**，且不产生任何 streakProgress。
        QSignalSpy progress(&c, &AccessController::streakProgress);
        QVERIFY2(c.request(AccessMethod::Password, QStringLiteral("密码")), "密码应一次放行");
        QVERIFY2(c.request(AccessMethod::Fingerprint, QStringLiteral("指纹")), "指纹应一次放行");
        QVERIFY2(c.request(AccessMethod::Remote, QStringLiteral("远程")), "远程应一次放行");
        QCOMPARE(progress.count(), 0);
        QCOMPARE(c.pendingCount(), 0);
        QCOMPARE(c.grantedCount(), 3);
    }

    // ---- 11. 四种方式的进度互不串联 ----

    void streakProgressIsNotSharedAcrossMethods()
    {
        AccessController c;
        c.setRequiredStreak(AccessMethod::Face, 3, 5000);
        c.setRequiredStreak(AccessMethod::Fingerprint, 2, 5000);

        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 1);
        QCOMPARE(c.pendingStreak(AccessMethod::Fingerprint), 0);

        // 指纹第 1 次应记在指纹上，人脸的进度不受影响。
        QVERIFY(!c.request(AccessMethod::Fingerprint, QStringLiteral("指纹")));
        QCOMPARE(c.pendingStreak(AccessMethod::Fingerprint), 1);
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 1);

        // 指纹第 2 次放行；人脸仍停在第 1 次。
        QVERIFY(c.request(AccessMethod::Fingerprint, QStringLiteral("指纹")));
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 1);
        QCOMPARE(c.pendingStreak(AccessMethod::Fingerprint), 0);
    }

    // ---- 12. 出口兜底：非法阈值被夹取，不崩 ----

    void streakThresholdsAreClamped()
    {
        AccessController c;

        c.setRequiredStreak(AccessMethod::Face, 0, 0);
        QCOMPARE(c.requiredStreak(AccessMethod::Face), 1);      // 0 -> 1
        QCOMPARE(c.streakWindowMs(AccessMethod::Face), 1);      // 0 -> 1

        c.setRequiredStreak(AccessMethod::Face, -5, -100);
        QCOMPARE(c.requiredStreak(AccessMethod::Face), 1);
        QCOMPARE(c.streakWindowMs(AccessMethod::Face), 1);

        c.setRequiredStreak(AccessMethod::Face, 999, 999999);
        QCOMPARE(c.requiredStreak(AccessMethod::Face), 10);
        QCOMPARE(c.streakWindowMs(AccessMethod::Face), 60000);
    }

    // ---- 13. 改阈值必须清空已有进度 ----
    //
    // 否则"先攒 3 次、再把阈值从 5 改成 2"会**立刻**放行一次，
    // 而那一次并不是在 2 次窗口内攒出来的。

    void changingThresholdResetsProgress()
    {
        AccessController c;
        c.setRequiredStreak(AccessMethod::Face, 5, 5000);
        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 3);

        c.setRequiredStreak(AccessMethod::Face, 2, 5000);
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 0);

        // 改完阈值后必须重新攒：第 1 次暂缓、第 2 次才放行。
        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QVERIFY(c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QCOMPARE(c.grantedCount(), 1);
    }

    // ---- 14. resetStreaks() 清进度但不改阈值 ----

    void resetStreaksClearsProgressOnly()
    {
        AccessController c;
        c.setRequiredStreak(AccessMethod::Face, 3, 5000);
        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 1);

        c.resetStreaks();
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 0);
        QCOMPARE(c.requiredStreak(AccessMethod::Face), 3);   // 阈值不变

        // 重新从第 1 次开始攒。
        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 1);
    }

    // ---- 15. 攒够放行时，**执行器只被驱动一次** ----

    void streakGrantsExactlyOnePulse()
    {
        Rig rig;
        QVERIFY(rig.open());
        rig.controller.setRequiredStreak(AccessMethod::Face, 3, 5000);

        QVERIFY(!rig.controller.request(AccessMethod::Face, QStringLiteral("人脸")));
        QVERIFY(!rig.controller.request(AccessMethod::Face, QStringLiteral("人脸")));
        QCOMPARE(fakegpiod::setValueCount(), 0);   // 暂缓期间不得有任何电气动作

        QVERIFY(rig.controller.request(AccessMethod::Face, QStringLiteral("人脸")));
        QCOMPARE(fakegpiod::setValueCount(), 1);
        QCOMPARE(rig.controller.grantedCount(), 1);
    }

    // ---- 16. 暂缓期间**不进入静默期**（两个机制互相独立）----

    void pendingStreakDoesNotStartSilentPeriod()
    {
        AccessController c;
        c.setRequiredStreak(AccessMethod::Face, 3, 5000);

        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QVERIFY2(!c.isInSilentPeriod(),
                 "只是暂缓、并未放行，不该进入静默期（否则会互相污染）");
        QCOMPARE(c.silentRemainingMs(), 0);
        QCOMPARE(c.suppressedCount(), 0);   // 也不是"被抑制"
        QCOMPARE(c.pendingCount(), 1);      // 单独记账
    }

    // ---- 17. 静默期优先于连续命中：静默期内的命中不计入进度 ----
    //
    // 顺序若反过来，"静默期内的人脸命中"会被算进连续计数，
    // 使静默期实际上缩短了人脸所需的观察时间。

    void silentPeriodTakesPrecedenceOverStreak()
    {
        AccessController c;
        c.setSilentPeriodMs(3000);
        c.setRequiredStreak(AccessMethod::Face, 2, 5000);

        // 先攒满 2 次以真正放行一次（进入静默期）。
        // 注意：required==2 时**第 1 次本来就是暂缓**，所以这里先暂缓再放行。
        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QVERIFY2(c.request(AccessMethod::Face, QStringLiteral("人脸")),
                 "第 2 次应放行并进入静默期");
        QVERIFY(c.isInSilentPeriod());
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 0);   // 放行后进度清零

        // 静默期内的命中：必须记为"被抑制"，**不得**推进连续命中进度。
        // 若实现把静默期判断放在连续命中之后，这一次会被算成新的第 1 次，
        // 使静默期实际上缩短了人脸所需的观察时间。
        const int pendingBefore = c.pendingCount();
        QVERIFY(!c.request(AccessMethod::Face, QStringLiteral("人脸")));
        QCOMPARE(c.suppressedCount(), 1);
        QCOMPARE(c.pendingCount(), pendingBefore);          // 未增加
        QCOMPARE(c.pendingStreak(AccessMethod::Face), 0);   // 进度仍为 0
    }

    // ---- 18. 越界枚举不崩（methodIndex 兜底） ----

    void outOfRangeMethodIsSafe()
    {
        AccessController c;
        const AccessMethod bogus = static_cast<AccessMethod>(99);
        // 不崩、不越界写，返回值落在合法默认上。
        c.setRequiredStreak(bogus, 3, 5000);
        QCOMPARE(c.requiredStreak(bogus), 3);
        QVERIFY(AccessController::methodText(bogus) != QString());
    }
};

QTEST_MAIN(TestAccessController)
#include "test_access_controller.moc"
