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
};

QTEST_MAIN(TestAccessController)
#include "test_access_controller.moc"
