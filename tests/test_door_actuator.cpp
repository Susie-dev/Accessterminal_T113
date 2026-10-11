/**
 * @file test_door_actuator.cpp
 * @brief DoorActuator（唯一的开锁执行器）的行为断言。
 *
 * 为什么必须有这一组
 * ------------------
 * 第 40 轮之前，本项目里**没有"开锁"这条代码路径**：`Widget::m_auxLine`
 * （PE5）只有 5 处使用，全是补光；认证成功侧只播音/弹窗。同时
 * `app_config.h` 却写着"开锁继电器与补光灯共用 PE5"。也就是说：
 *   - 若 PE5 是继电器 → 现状是"采集人脸时给锁上电、识别成功时把锁关掉"；
 *   - 若 PE5 只是灯 → 认证成功根本不开门。
 * 两种解释都是功能缺陷，且没有任何规则守着"补光"与"开锁"的语义区分。
 *
 * 本组测试把执行器的三条硬约束钉住（都是"错了会出安全事故"的那种）：
 *   1. **不许假装开门**：板端未接执行器时必须走模拟开锁并让调用方看得见
 *      （realActuator=false），而不是静默地"看起来成功"；
 *   2. **不许阻塞**：旧工程用同步 msleep(1000) 把 GUI 冻住 1 秒，这里断言
 *      unlock() 立即返回；
 *   3. **不许重复驱动**：脉冲进行中再请求不能产生第二次物理脉冲。
 *
 * 测试方式（生产代码零改动）
 * --------------------------
 * 链接可编程假 libgpiod `tests/fake_gpiod.cpp`，用**真实**的 `GpioLine`
 * 与**真实**的 `DoorActuator`，断言落在假实现收到的调用序列与参数上。
 */
#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QSignalSpy>

#include "hal/gpio.h"
#include "managers/door_actuator.h"

#include "fake_gpiod.h"

using accessterminal::DoorActuator;
using accessterminal::GpioLine;

namespace {

/// 假 libgpiod 记录里 "set:" 出现的次数（即真正写了几次电平）。
int setValueCalls()
{
    return fakegpiod::setValueCount();
}

} // namespace

class TestDoorActuator : public QObject
{
    Q_OBJECT

private slots:
    void init() { fakegpiod::reset(); }

    // ---- 1. 未接执行器 = 模拟开锁，且绝不假装成功 ----

    void unwiredUnlockIsSimulatedAndVisible()
    {
        DoorActuator act;
        QSignalSpy pulsed(&act, &DoorActuator::pulsed);
        act.setLine(nullptr);

        QVERIFY(!act.isWired());
        const bool drove = act.unlock(QStringLiteral("密码"));

        QVERIFY2(!drove, "未接执行器时 unlock() 不得报告成功");
        QCOMPARE(act.pulseCount(), 0);
        QCOMPARE(pulsed.count(), 1);
        const QList<QVariant> args = pulsed.takeFirst();
        QCOMPARE(args.at(0).toString(), QStringLiteral("密码"));
        QCOMPARE(args.at(2).toBool(), false); // realActuator=false
        // 一个电平都不许写：这正是"补光被误当门锁"的防线
        QCOMPARE(setValueCalls(), 0);
    }

    // ---- 2. 接了执行器 = 真实的非阻塞脉冲 ----

    void wiredUnlockPulsesThenReleases()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("test"), 0));
        QVERIFY(line.isValid());

        DoorActuator act;
        act.setLine(&line);
        act.setDurationMs(30);
        QVERIFY(act.isWired());

        QSignalSpy pulsed(&act, &DoorActuator::pulsed);
        QSignalSpy released(&act, &DoorActuator::released);

        const bool drove = act.unlock(QStringLiteral("人脸"));
        QVERIFY2(drove, "接了执行器时 unlock() 必须报告真实驱动");
        QCOMPARE(act.pulseCount(), 1);
        QCOMPARE(pulsed.count(), 1);
        QCOMPARE(pulsed.at(0).at(2).toBool(), true); // realActuator=true
        QCOMPARE(setValueCalls(), 1);
        QCOMPARE(fakegpiod::lastSetValue(), 1);
        QVERIFY(act.isPulsing());

        // 拉低由定时器完成，不需要调用方等待
        QTRY_COMPARE_WITH_TIMEOUT(released.count(), 1, 5000);
        QCOMPARE(released.at(0).at(0).toString(), QStringLiteral("人脸"));
        QCOMPARE(setValueCalls(), 2);
        QCOMPARE(fakegpiod::lastSetValue(), 0);
        QVERIFY(!act.isPulsing());

        // 顺序断言：先拉高、后拉低（只数次数发现不了顺序颠倒）
        const QStringList log = fakegpiod::callLog();
        QVERIFY(log.indexOf(QStringLiteral("set:1")) <
                log.indexOf(QStringLiteral("set:0")));
    }

    void unlockDoesNotBlockTheCaller()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("test"), 0));
        DoorActuator act;
        act.setLine(&line);
        act.setDurationMs(1000); // 旧工程在这一秒上是 msleep，GUI 会冻住

        QElapsedTimer timer;
        timer.start();
        QVERIFY(act.unlock(QStringLiteral("指纹")));
        const qint64 elapsed = timer.elapsed();

        QVERIFY2(elapsed < 50, qPrintable(QStringLiteral("unlock() 阻塞了 %1ms，"
                                                         "必须立即返回").arg(elapsed)));
    }

    // ---- 3. 脉冲进行中不重复驱动 ----

    void repeatedRequestDuringPulseDoesNotDriveTwice()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("test"), 0));
        DoorActuator act;
        act.setLine(&line);
        act.setDurationMs(300);

        QVERIFY(act.unlock(QStringLiteral("密码")));
        const bool second = act.unlock(QStringLiteral("密码"));

        QVERIFY2(!second, "脉冲进行中的重复请求必须被拒绝");
        QCOMPARE(act.pulseCount(), 1);
        QCOMPARE(setValueCalls(), 1); // 只写了一次高电平

        QSignalSpy released(&act, &DoorActuator::released);
        QTRY_COMPARE_WITH_TIMEOUT(released.count(), 1, 5000);
    }

    // ---- 4. 写失败不谎报成功 ----

    void setValueFailureIsNotReportedAsReal()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("test"), 0));
        DoorActuator act;
        act.setLine(&line);
        act.setDurationMs(30);

        fakegpiod::setSetValueFail(true); // 注入：写电平失败
        QSignalSpy pulsed(&act, &DoorActuator::pulsed);

        const bool drove = act.unlock(QStringLiteral("远程"));

        QVERIFY2(!drove, "写 GPIO 失败时不得报告成功");
        QCOMPARE(act.pulseCount(), 0);
        QCOMPARE(pulsed.count(), 1);
        QCOMPARE(pulsed.at(0).at(2).toBool(), false);
        QVERIFY(!act.isPulsing());
    }

    // ---- 5. 出口兜底：脉冲宽度夹取 ----

    void durationIsClampedAtTheExit()
    {
        DoorActuator act;
        act.setDurationMs(0);
        QCOMPARE(act.durationMs(), 1);
        act.setDurationMs(-5);
        QCOMPARE(act.durationMs(), 1);
        act.setDurationMs(500);
        QCOMPARE(act.durationMs(), 500);
        act.setDurationMs(999999);
        QCOMPARE(act.durationMs(), 10000);
    }

    // ---- 6. isWired 反映线的真实状态 ----

    void isWiredFollowsLineState()
    {
        DoorActuator act;
        act.setLine(nullptr);
        QVERIFY(!act.isWired());

        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("test"), 0));
        act.setLine(&line);
        QVERIFY(act.isWired());

        line.close();
        QVERIFY2(!act.isWired(), "线被释放后不得再认为是接好的");
        QVERIFY(!act.unlock(QStringLiteral("密码")));
    }
};

QTEST_MAIN(TestDoorActuator)
#include "test_door_actuator.moc"
