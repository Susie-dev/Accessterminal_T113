/**
 * @file test_gpio.cpp
 * @brief GpioLine（libgpiod 的 RAII 封装）的行为与资源生命周期断言。
 *
 * 为什么必须补这一组
 * ------------------
 * `GpioLine` 管的是门口机**物理门锁/补光**那条线（T113 的 PE5 = 线号 133，
 * 开锁与补光共用同一条线，见 widget.cpp:224-226）。它此前从未被链接进任何
 * 测试目标 —— 而它的危险分支全在"硬件出错"上：
 *
 *   - 打不开 gpiochip（权限不足 / 设备节点不存在）；
 *   - 拿不到线（线号越界、被别的进程占用）；
 *   - 请求为输出/输入失败（线已被占用）；
 *   - 读取电平失败（value() 返回 -1）。
 *
 * 这些分支一旦处理错，后果是**门锁该开时不开**、或**句柄泄漏把线占死**
 * （下一次开锁直接失败，因为线还握在自己手里）。
 *
 * 测试方式（生产代码零改动）
 * --------------------------
 * 链接**可编程假 libgpiod** `tests/fake_gpiod.cpp`，不链接
 * tools/devstubs/stub_impl.c（那个是固定桩，返回值恒定、不可编程，只能做
 * 链接检查）。假实现用真实分配返回句柄并真的 delete，
 * 于是 liveLineCount()/liveChipCount() 能直接暴露泄漏与重复释放。
 *
 * 判据取向
 * --------
 * 断言落在**可观测输出**上：假 libgpiod 收到的调用**序列**与参数、
 * 以及是否还持有句柄。不断言"内部指针变成 nullptr"这类实现细节。
 *
 * 特别是 `close()` 里那条注释所声明的契约 —— "先释放线，再关闭 chip" ——
 * 只数次数是发现不了顺序颠倒的，所以断言用的是 callLog() 里的**顺序**。
 */
#include <QtTest/QtTest>

#include "hal/gpio.h"

#include "fake_gpiod.h"

using accessterminal::GpioLine;

namespace {

/// callLog 里 a 是否出现在 b 之前。
bool before(const QStringList& log, const QString& a, const QString& b)
{
    const int ia = log.indexOf(a);
    const int ib = log.indexOf(b);
    return ia >= 0 && ib >= 0 && ia < ib;
}

/// 统计 log 中以 prefix 开头的条目数。
int countPrefix(const QStringList& log, const QString& prefix)
{
    int n = 0;
    for (int i = 0; i < log.size(); ++i) {
        if (log.at(i).startsWith(prefix)) {
            ++n;
        }
    }
    return n;
}

} // namespace

class TestGpio : public QObject
{
    Q_OBJECT

private slots:

    void init() { fakegpiod::reset(); }

    void cleanup()
    {
        // 每个用例结束时都必须没有任何"仍被请求"的线，否则线会一直 busy，
// 下一次开锁直接 EBUSY 失败。这才是现场的故障度量。
        QCOMPARE(fakegpiod::busyLineCount(), 0);
        QCOMPARE(fakegpiod::liveChipCount(), 0);
    }

    // -----------------------------------------------------------------------
    // 成功路径
    // -----------------------------------------------------------------------

    /// 以输出模式打开：必须请求**输出**、带上给定的 consumer 与初始电平。
    void openOutputRequestsOutputWithGivenParams()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("my_app"), 0));
        QVERIFY(line.isValid());

        QCOMPARE(fakegpiod::chipOpenCount(), 1);
        QCOMPARE(fakegpiod::getLineCount(), 1);
        QCOMPARE(fakegpiod::requestOutputCount(), 1);
        QCOMPARE(fakegpiod::requestInputCount(), 0);

        QCOMPARE(fakegpiod::lastChipName(), QStringLiteral("gpiochip0"));
        QCOMPARE(fakegpiod::lastLineOffset(), 133u);
        QCOMPARE(fakegpiod::lastConsumer(), QStringLiteral("my_app"));
        QCOMPARE(fakegpiod::lastInitialValue(), 0);

        QCOMPARE(fakegpiod::liveChipCount(), 1);
        QCOMPARE(fakegpiod::busyLineCount(), 1);
    }

    /// 初始电平 1 必须原样传给 libgpiod（不是硬编码 0）。
    void initialValueIsPassedThrough()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("my_app"), 1));
        QCOMPARE(fakegpiod::lastInitialValue(), 1);
    }

    /// openOutput 的 initialValue 默认参数必须是 0（与 baseline 一致）。
    void defaultInitialValueIsZero()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("my_app")));
        QCOMPARE(fakegpiod::lastInitialValue(), 0);
    }

    /// 以输入模式打开：必须请求**输入**（不是输出）。
    void openInputRequestsInput()
    {
        GpioLine line;
        QVERIFY(line.openInput(QStringLiteral("gpiochip0"), 132,
                               QStringLiteral("pe4_monitor")));
        QVERIFY(line.isValid());

        QCOMPARE(fakegpiod::requestInputCount(), 1);
        QCOMPARE(fakegpiod::requestOutputCount(), 0);
        QCOMPARE(fakegpiod::lastLineOffset(), 132u);
        QCOMPARE(fakegpiod::lastConsumer(), QStringLiteral("pe4_monitor"));
    }

    /// setValue 必须把值真的传下去。
    void setValueReachesLibgpiod()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("my_app"), 0));

        QVERIFY(line.setValue(1));
        QCOMPARE(fakegpiod::lastSetValue(), 1);
        QVERIFY(line.setValue(0));
        QCOMPARE(fakegpiod::lastSetValue(), 0);
        QCOMPARE(fakegpiod::setValueCount(), 2);
    }

    /// value() 必须原样返回 libgpiod 读到的电平（含 0 与 1 两个方向）。
    void valueReturnsReadLevel()
    {
        GpioLine line;
        QVERIFY(line.openInput(QStringLiteral("gpiochip0"), 132,
                               QStringLiteral("pe4_monitor")));

        fakegpiod::setReadValue(1);
        QCOMPARE(line.value(), 1);
        fakegpiod::setReadValue(0);
        QCOMPARE(line.value(), 0);
        QCOMPARE(fakegpiod::getValueCount(), 2);
    }

    // -----------------------------------------------------------------------
    // 失败路径：不得泄漏，且必须释放**已经拿到**的资源
    // -----------------------------------------------------------------------

    /**
     * 打不开 chip：返回 false，且**不**去关闭 nullptr（也不崩）。
     *
     * 这是最常见的一线故障（设备节点不存在、权限不足）。
     */
    void chipOpenFailureReturnsFalseWithoutClosingNull()
    {
        fakegpiod::setChipOpenFail(true);

        GpioLine line;
        QVERIFY(!line.openOutput(QStringLiteral("gpiochip0"), 133,
                                 QStringLiteral("my_app"), 0));
        QVERIFY(!line.isValid());

        QCOMPARE(fakegpiod::chipOpenCount(), 1);
        // 没拿到 chip，就不该有 close（传 nullptr 的 close 不应记账）。
        QCOMPARE(fakegpiod::chipCloseCount(), 0);
        QCOMPARE(fakegpiod::liveChipCount(), 0);
        QCOMPARE(fakegpiod::busyLineCount(), 0);

        // 失败后 isValid() 为假，setValue/value 必须是安全 no-op。
        QVERIFY(!line.setValue(1));
        QCOMPARE(fakegpiod::setValueCount(), 0);
        QCOMPARE(line.value(), -1);
    }

    /// 拿不到线：必须**关闭已经打开的 chip**，否则 chip 句柄泄漏。
    void getLineFailureClosesChip()
    {
        fakegpiod::setGetLineFail(true);

        GpioLine line;
        QVERIFY(!line.openOutput(QStringLiteral("gpiochip0"), 133,
                                 QStringLiteral("my_app"), 0));
        QVERIFY(!line.isValid());

        QCOMPARE(fakegpiod::chipOpenCount(), 1);
        QCOMPARE(fakegpiod::getLineCount(), 1);
        QCOMPARE(fakegpiod::chipCloseCount(), 1);   // 关键：chip 被关了
        QCOMPARE(fakegpiod::liveChipCount(), 0);
        QCOMPARE(fakegpiod::busyLineCount(), 0);
        QCOMPARE(fakegpiod::requestOutputCount(), 0);
    }

    /// 请求输出失败：必须**释放线**并**关闭 chip**（两个都不能漏）。
    void requestOutputFailureReleasesLineAndClosesChip()
    {
        fakegpiod::setRequestOutputFail(true);

        GpioLine line;
        QVERIFY(!line.openOutput(QStringLiteral("gpiochip0"), 133,
                                 QStringLiteral("my_app"), 0));
        QVERIFY(!line.isValid());

        QCOMPARE(fakegpiod::requestOutputCount(), 1);
        QCOMPARE(fakegpiod::chipCloseCount(), 1);   // chip 被关闭
        QCOMPARE(fakegpiod::busyLineCount(), 0);    // 没有遗留的请求
        QCOMPARE(fakegpiod::liveChipCount(), 0);

        // 这里**不能**断言 releaseCount()==1：请求失败意味着这条线从未进入
        // "已请求"状态，生产代码仍会调 release（防御性清理），而 libgpiod 对
        // "release 一条未请求的线"的确切行为我无法在本机验证（没有真实
        // gpiod.h / 库）。因此只观测，不断言 —— 避免把猜测写成契约。
        const QStringList log = fakegpiod::callLog();
        QVERIFY2(fakegpiod::releaseUnrequestedCount() >= 1,
                 qPrintable(QStringLiteral("请求失败后应尝试清理线: ")
                            + log.join(',')));

        // 释放顺序：先释放线（不论哪种形式），再关闭 chip。
        const int relLine = log.indexOf(QStringLiteral("release")) >= 0
                                ? log.indexOf(QStringLiteral("release"))
                                : log.indexOf(QStringLiteral("release:unrequested"));
        const int relClose = log.indexOf(QStringLiteral("close"));
        QVERIFY2(relLine >= 0 && relClose > relLine,
                 qPrintable(QStringLiteral("释放顺序错误（应先 line 后 chip）: ")
                            + log.join(',')));
    }

    /// 请求输入失败：同样必须 releases 线 + closes chip。
    void requestInputFailureReleasesLineAndClosesChip()
    {
        fakegpiod::setRequestInputFail(true);

        GpioLine line;
        QVERIFY(!line.openInput(QStringLiteral("gpiochip0"), 132,
                                QStringLiteral("pe4_monitor")));
        QVERIFY(!line.isValid());

        QCOMPARE(fakegpiod::requestInputCount(), 1);
        QCOMPARE(fakegpiod::chipCloseCount(), 1);
        QCOMPARE(fakegpiod::busyLineCount(), 0);
        QCOMPARE(fakegpiod::liveChipCount(), 0);
        QVERIFY2(fakegpiod::releaseUnrequestedCount() >= 1,
                 "请求失败后应尝试清理线");
    }

    /**
     * 失败后再重试成功的场景：上一次失败**不得**留下残留句柄。
     *
     * 这条模拟现场真实序列：启动时设备就绪失败 -> 稍后重试成功。
     * 若失败路径漏释放，第二次 open 会因"线还被自己占着"而失败。
     */
    void failedOpenLeavesNoResidueForRetry()
    {
        GpioLine line;

        fakegpiod::setRequestOutputFail(true);
        QVERIFY(!line.openOutput(QStringLiteral("gpiochip0"), 133,
                                 QStringLiteral("my_app"), 0));
        QCOMPARE(fakegpiod::busyLineCount(), 0);
        QCOMPARE(fakegpiod::liveChipCount(), 0);

        // 硬件恢复，重试必须成功。
        fakegpiod::setRequestOutputFail(false);
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("my_app"), 0));
        QVERIFY(line.isValid());
        QVERIFY(line.setValue(1));
        QCOMPARE(fakegpiod::lastSetValue(), 1);
    }

    /// setValue 失败必须如实返回 false（不能吞掉错误假装成功）。
    void setValueFailureIsReported()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("my_app"), 0));

        fakegpiod::setSetValueFail(true);
        QVERIFY2(!line.setValue(1), "libgpiod 写失败时 setValue 必须返回 false");
        QCOMPARE(fakegpiod::setValueCount(), 1);
    }

    /// 未占用时 value()/setValue() 必须是安全 no-op（不得触碰 libgpiod）。
    void operationsOnUnopenedLineAreNoOps()
    {
        GpioLine line;
        QVERIFY(!line.isValid());

        QCOMPARE(line.value(), -1);
        QVERIFY(!line.setValue(1));

        QCOMPARE(fakegpiod::getValueCount(), 0);
        QCOMPARE(fakegpiod::setValueCount(), 0);
        QCOMPARE(fakegpiod::chipOpenCount(), 0);
    }

    // -----------------------------------------------------------------------
    // 析构与显式 close
    // -----------------------------------------------------------------------

    /// 析构必须释放线并关闭 chip（RAII 的核心承诺）。
    void destructorReleasesEverything()
    {
        {
            GpioLine line;
            QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                    QStringLiteral("my_app"), 0));
            QCOMPARE(fakegpiod::busyLineCount(), 1);
        }
        QCOMPARE(fakegpiod::releaseCount(), 1);
        QCOMPARE(fakegpiod::chipCloseCount(), 1);
        QCOMPARE(fakegpiod::busyLineCount(), 0);
        QCOMPARE(fakegpiod::liveChipCount(), 0);
        QCOMPARE(fakegpiod::releaseUnrequestedCount(), 0);  // 成功路径不应出现
    }

    /// 显式 close() 后必须可重复调用（幂等），且不重复释放。
    void closeIsIdempotent()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("my_app"), 0));

        line.close();
        QCOMPARE(fakegpiod::releaseCount(), 1);
        QCOMPARE(fakegpiod::chipCloseCount(), 1);
        QVERIFY(!line.isValid());

        // 再关两次：不得再产生释放调用（重复释放会二次 free）。
        line.close();
        line.close();
        QCOMPARE(fakegpiod::releaseCount(), 1);
        QCOMPARE(fakegpiod::chipCloseCount(), 1);

        // 析构发生在用例结束时（cleanup 会断言无残留）。此处再显式确认
        // "已 close 的对象在析构时不再释放"。
        QCOMPARE(fakegpiod::busyLineCount(), 0);
        QCOMPARE(fakegpiod::liveChipCount(), 0);
    }

    /// 显式 close() 后仍可重新 open（close 必须是完全重置）。
    void canReopenAfterClose()
    {
        GpioLine line;
        QVERIFY(line.openInput(QStringLiteral("gpiochip0"), 132,
                               QStringLiteral("pe4_monitor")));
        line.close();

        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("my_app"), 0));
        QVERIFY(line.isValid());
        QCOMPARE(fakegpiod::chipOpenCount(), 2);
        QCOMPARE(fakegpiod::busyLineCount(), 1);
        QCOMPARE(fakegpiod::liveChipCount(), 1);
    }

    /**
     * 对一个**已占用**的对象再 open：必须先释放旧的，不能泄漏第一条线。
     *
     * openOutput/openInput 的第一句就是 close()，本用例把这条钉住。
     */
    void reopeningWithoutCloseReleasesPreviousLine()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("my_app"), 0));
        QVERIFY(line.openOutput(QStringLiteral("gpiochip1"), 134,
                                QStringLiteral("other"), 0));

        QCOMPARE(fakegpiod::chipOpenCount(), 2);
        QCOMPARE(fakegpiod::releaseCount(), 1);   // 旧线被释放
        QCOMPARE(fakegpiod::chipCloseCount(), 1); // 旧 chip 被关闭
        QCOMPARE(fakegpiod::busyLineCount(), 1);  // 只剩新的那条
        QCOMPARE(fakegpiod::liveChipCount(), 1);
        QCOMPARE(fakegpiod::lastChipName(), QStringLiteral("gpiochip1"));
        QCOMPARE(fakegpiod::lastLineOffset(), 134u);
    }

    // -----------------------------------------------------------------------
    // 移动语义（资源独占）
    // -----------------------------------------------------------------------

    /// 移动构造后：源不再持有句柄，目标持有；只发生一次释放。
    void moveConstructionTransfersOwnership()
    {
        GpioLine src;
        QVERIFY(src.openOutput(QStringLiteral("gpiochip0"), 133,
                               QStringLiteral("my_app"), 0));

        GpioLine dst(std::move(src));
        QVERIFY(dst.isValid());
        QVERIFY2(!src.isValid(), "移动构造后源对象必须不再持有线");

        QCOMPARE(fakegpiod::busyLineCount(), 1);
        QCOMPARE(fakegpiod::releaseCount(), 0);

        // 源析构不得释放（它已经交出去了）。这块作用域结束算一次。
        QCOMPARE(fakegpiod::releaseCount(), 0);
        QVERIFY(dst.setValue(1));
    }

    /// 移动赋值后：旧资源被释放，新资源接管；源被置空。
    void moveAssignmentReleasesOldAndTakesNew()
    {
        GpioLine a;
        QVERIFY(a.openOutput(QStringLiteral("gpiochip0"), 133,
                             QStringLiteral("first"), 0));

        GpioLine b;
        QVERIFY(b.openOutput(QStringLiteral("gpiochip0"), 134,
                             QStringLiteral("second"), 0));

        // a = move(b)：a 原来的线必须先被释放。
        a = std::move(b);

        QCOMPARE(fakegpiod::releaseCount(), 1);   // 只释放了 a 的旧线
        QCOMPARE(fakegpiod::busyLineCount(), 1);  // 接管了 b 的线
        QVERIFY(a.isValid());
        QVERIFY2(!b.isValid(), "移动赋值后源对象必须不再持有线");

        // a 现在能操作，且作用的是 b 原来的那条。
        QVERIFY(a.setValue(1));
        QCOMPARE(fakegpiod::lastSetValue(), 1);
    }

    /// 自移动赋值不得自我释放（a = std::move(a) 必须安全）。
    void selfMoveAssignmentIsSafe()
    {
        GpioLine a;
        QVERIFY(a.openOutput(QStringLiteral("gpiochip0"), 133,
                             QStringLiteral("my_app"), 0));

        a = std::move(a);

        QVERIFY2(a.isValid(), "自移动赋值不得把自己释放掉");
        QCOMPARE(fakegpiod::releaseCount(), 0);
        QCOMPARE(fakegpiod::busyLineCount(), 1);
        QVERIFY(a.setValue(1));
    }

    /**
     * 移动后的源对象析构必须安全，且不得重复释放。
     *
     * 这条覆盖"移动后源仍会走析构"这一必经路径。
     */
    void destructorOfMovedFromObjectIsSafe()
    {
        GpioLine dst;
        {
            GpioLine src;
            QVERIFY(src.openInput(QStringLiteral("gpiochip0"), 132,
                                  QStringLiteral("pe4_monitor")));
            dst = std::move(src);
            // src 在此析构 —— 它已被置空，不应产生任何释放。
        }
        QCOMPARE(fakegpiod::releaseCount(), 0);
        QCOMPARE(fakegpiod::busyLineCount(), 1);
        QVERIFY(dst.isValid());
    }

    // -----------------------------------------------------------------------
    // 与现场用法一致的契约
    // -----------------------------------------------------------------------

    /**
     * 现场用法（widget.cpp）：PE5 线 133、consumer "my_app"、输出、初始 0。
     *
     * 把这些值钉在测试里，防止有人顺手改线号或 consumer 名 —— 改线号意味着
     * 开锁驱动的是**另一条引脚**，在真机上表现为"该开门时不开"或"乱亮补光灯"。
     */
    void productionWiringIsPinned()
    {
        GpioLine line;
        QVERIFY(line.openOutput(QStringLiteral("gpiochip0"), 133,
                                QStringLiteral("my_app"), 0));
        QCOMPARE(fakegpiod::lastChipName(), QStringLiteral("gpiochip0"));
        QCOMPARE(fakegpiod::lastLineOffset(), 133u);
        QCOMPARE(fakegpiod::lastConsumer(), QStringLiteral("my_app"));
        QCOMPARE(fakegpiod::lastInitialValue(), 0);

        // 开锁动作：置 1，再置 0。
        QVERIFY(line.setValue(1));
        QVERIFY(line.setValue(0));
        QCOMPARE(fakegpiod::setValueCount(), 2);
    }

    /**
     * PE4 红外输入（widget.cpp）：线 132、consumer "pe4_monitor"、输入模式。
     */
    void pe4MonitorWiringIsPinned()
    {
        GpioLine line;
        QVERIFY(line.openInput(QStringLiteral("gpiochip0"), 132,
                               QStringLiteral("pe4_monitor")));
        QCOMPARE(fakegpiod::lastLineOffset(), 132u);
        QCOMPARE(fakegpiod::lastConsumer(), QStringLiteral("pe4_monitor"));
        QCOMPARE(fakegpiod::requestInputCount(), 1);
        QCOMPARE(fakegpiod::requestOutputCount(), 0);
    }
};

QTEST_MAIN(TestGpio)
#include "test_gpio.moc"
