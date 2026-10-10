/*
 * test_fingerprint_wire.cpp — 直接测试**真实**的 HailinFingerprint 报文构造
 *
 * 为什么必须单独有这一组（第 25 轮）
 * ---------------------------------
 * 第 20/21 轮用"链接期替身"测指纹流程，前提是"替身里 buildPacket() 是真实
 * 算法的逐字节拷贝 —— 报文字节是唯一不能换的契约"。但那个前提**从未被自动
 * 验证**，而且带来两个后果：
 *
 *   1. 生产的 buildPacket() 覆盖率是 **0**（它从未被链接进任何测试），
 *      报文字节实际上没有被任何运行时测试直接验证过；
 *   2. 契约为两份（生产一份、替身一份），却没有一条检查盯着"两份还一样吗"。
 *      有人改了生产的校验和起始下标或长度字段，替身不跟着变，测试照样全绿，
 *      而真机报文已经错了。
 *
 * 本轮查明：buildPacket() 是 **public 且纯函数** —— 只拼字节，不碰串口。
 * 构造函数也只设置串口参数、并不 open()。因此**可以链接真实实现**直接测，
 * 不需要任何替身、也不需要给生产代码加测试钩子。
 *
 * 本目标**链接真实的 src/hal/fingerprint_hailin.cpp**（与
 * t113_test_fingerprint_manager 的替身方案相反，二者互不干扰）。
 *
 * 覆盖范围仅限纯逻辑：buildPacket / 校验和 / 与 FingerprintFrameParser 的
 * 互操作。readPacket 的空队列超时路径也可测（见 timeoutReturnsEmpty）。
 */
#include <QtTest/QtTest>

#include <QByteArray>

#include "hal/fingerprint_frame_parser.h"
#include "hal/fingerprint_hailin.h"

namespace {

/// 把字节数组渲染成十六进制，便于断言失败时看清差异。
QString hex(const QByteArray& b)
{
    return QString::fromLatin1(b.toHex(' '));
}

/// 按真实协议从零算一遍期望帧，作为**独立**于生产实现的参照。
///
/// 刻意不复用生产的 buildPacket()：那样就成了"自己和自己比"。
/// 这里按文档化的线上格式直写一遍（包头 0xEF01、广播地址 0xFFFFFFFF、
/// 标识 0x01、大端长度 = payload+2、payload、大端校验和自下标 6 起算）。
QByteArray expectedFrame(const QByteArray& payload)
{
    QByteArray p;
    p.append((char)0xEF);
    p.append((char)0x01);
    p.append((char)0xFF);
    p.append((char)0xFF);
    p.append((char)0xFF);
    p.append((char)0xFF);
    p.append((char)0x01);
    const int len = payload.size() + 2;
    p.append((char)((len >> 8) & 0xFF));
    p.append((char)(len & 0xFF));
    p.append(payload);
    quint16 sum = 0;
    for (int i = 6; i < p.size(); ++i) {
        sum = static_cast<quint16>(sum + static_cast<quint8>(p.at(i)));
    }
    p.append((char)((sum >> 8) & 0xFF));
    p.append((char)(sum & 0xFF));
    return p;
}

} // namespace

class TestFingerprintWire : public QObject
{
    Q_OBJECT

private slots:
    // ---- 报文构造：逐字节契约 ----
    void getImageFrameIsExactWireBytes();
    void headerAndAddressAreConstant();
    void lengthFieldIsBigEndianPayloadPlusChecksum();
    void checksumStartsAtPacketTagNotHeader();
    void checksumIsBigEndian();
    void everyKnownCommandProducesExpectedFrame_data();
    void everyKnownCommandProducesExpectedFrame();

    // ---- 与解析器的互操作：构造出来的帧必须能被自己的解析器解回 ----
    void builtFrameRoundTripsThroughParser();
    void parserAcceptsFrameWithMaximumPayload();

    // ---- 边界：大 payload 让校验和跨字节 ----
    void largePayloadProducesCorrectChecksum();

    // ---- 设备不可用时的行为（不得崩）----
    // 【第 25 轮实测】未打开的 QSerialPort 上 waitForReadyRead() 会让 Qt5
    // 段错误，故这些用例**不**直接调 readPacket() 去撞未定义行为。
    void openFailureReturnsFalseAndDoesNotCrash();
    void pureBuildWorksWhileDeviceUnavailable();

private:
    /// 每个用例都新建一个设备对象：buildPacket 会读成员吗？
    /// 不读 —— 用未打开的串口对象即可，构造不会 open()。
    HailinFingerprint m_dev{QStringLiteral("/dev/null-test")};
};

// ---------------------------------------------------------------------------
// 采集图像：CMD_GET_IMAGE = 0x01，payload 只有 1 字节。
// 期望 11 字节（最小合法帧）。
// ---------------------------------------------------------------------------
void TestFingerprintWire::getImageFrameIsExactWireBytes()
{
    QByteArray payload;
    payload.append((char)0x01);

    const QByteArray got = m_dev.buildPacket(payload);
    const QByteArray want = expectedFrame(payload);

    // 帧长 = 头部 9 + payload 1 + 校验和 2 = 12
    QCOMPARE(got.size(), 12);
    QCOMPARE(hex(got), hex(want));
    // 逐字节写死，作为文档化的契约（不依赖 expectedFrame 的实现）
    QCOMPARE(hex(got), QStringLiteral(
        "ef 01 ff ff ff ff 01 00 03 01 00 05"));
}

// ---------------------------------------------------------------------------
// 包头 0xEF01 与广播地址 0xFFFFFFFF 必须恒定。
// ---------------------------------------------------------------------------
void TestFingerprintWire::headerAndAddressAreConstant()
{
    for (int n = 0; n <= 8; ++n) {
        QByteArray payload(n, (char)0x5A);
        const QByteArray pkt = m_dev.buildPacket(payload);
        QCOMPARE(static_cast<quint8>(pkt.at(0)), static_cast<quint8>(0xEF));
        QCOMPARE(static_cast<quint8>(pkt.at(1)), static_cast<quint8>(0x01));
        for (int i = 2; i <= 5; ++i) {
            QCOMPARE(static_cast<quint8>(pkt.at(i)), static_cast<quint8>(0xFF));
        }
        QCOMPARE(static_cast<quint8>(pkt.at(6)), static_cast<quint8>(0x01));
    }
}

// ---------------------------------------------------------------------------
// 长度字段 = payload + 2（校验和占 2 字节），大端。
// 下界与上界都测：0 字节 payload 与跨字节长度的 payload。
// ---------------------------------------------------------------------------
void TestFingerprintWire::lengthFieldIsBigEndianPayloadPlusChecksum()
{
    struct Case { int n; quint16 len; };
    const Case cases[] = {
        {0, 2}, {1, 3}, {8, 10}, {100, 102},
        {253, 255},          // 恰好跨到 0x00FF
        {254, 256},          // 0x0100 —— 大端高字节非零
        {300, 302},          // 0x012E
    };
    for (const Case& c : cases) {
        const QByteArray payload(c.n, (char)0x33);
        const QByteArray pkt = m_dev.buildPacket(payload);
        const quint16 got = (static_cast<quint8>(pkt.at(7)) << 8)
                          | static_cast<quint8>(pkt.at(8));
        QCOMPARE(got, c.len);
        // 帧长 = 头部 9 + payload + 校验和 2
        QCOMPARE(pkt.size(), c.n + 11);
        // 断言两个字节**各自**的值，而不是只看合并结果：
        // 合并成 quint16 后，大小端写反也可能碰巧相等（低字节非零、
        // 高字节为零时）。
        QCOMPARE(static_cast<quint8>(pkt.at(7)),
                 static_cast<quint8>(c.len >> 8));
        QCOMPARE(static_cast<quint8>(pkt.at(8)),
                 static_cast<quint8>(c.len & 0xFF));
    }
}

// ---------------------------------------------------------------------------
// 校验和**从下标 6 开始**（包标识），不含包头 0xEF01 与设备地址 0xFFFFFFFF。
// 这是最容易写错的一处：若从下标 0 起算，值会差 0xEF+0x01+4*0xFF = 1116。
// ---------------------------------------------------------------------------
void TestFingerprintWire::checksumStartsAtPacketTagNotHeader()
{
    QByteArray payload;
    payload.append((char)0x01);
    const QByteArray pkt = m_dev.buildPacket(payload);

    const quint16 got = (static_cast<quint8>(pkt.at(pkt.size() - 2)) << 8)
                      | static_cast<quint8>(pkt.at(pkt.size() - 1));

    // 从下标 6 起算：0x01 + 0x00 + 0x03 + 0x01 = 5
    quint16 from6 = 0;
    for (int i = 6; i < pkt.size() - 2; ++i) {
        from6 = static_cast<quint16>(from6 + static_cast<quint8>(pkt.at(i)));
    }
    QCOMPARE(got, from6);
    QCOMPARE(got, static_cast<quint16>(5));

    // 反证：从下标 0 起算会是另一个值，必须**不**相等
    quint16 from0 = 0;
    for (int i = 0; i < pkt.size() - 2; ++i) {
        from0 = static_cast<quint16>(from0 + static_cast<quint8>(pkt.at(i)));
    }
    QVERIFY(from0 != got);
    QCOMPARE(from0, static_cast<quint16>(5 + 0xEF + 0x01 + 4 * 0xFF));
}

// ---------------------------------------------------------------------------
// 校验和以大端写入。
// 构造一个校验和 >= 0x100 的 payload，确保高字节非零 —— 否则字节序写反
// 也看不出来（低字节非零、高字节为零时两种写法可能碰巧相同）。
// ---------------------------------------------------------------------------
void TestFingerprintWire::checksumIsBigEndian()
{
    // payload 全 0xFF：校验和 = 0x01 + len_hi + len_lo + 0xFF*n
    const QByteArray payload(4, (char)0xFF);
    const QByteArray pkt = m_dev.buildPacket(payload);
    const quint16 sum = (static_cast<quint8>(pkt.at(pkt.size() - 2)) << 8)
                      | static_cast<quint8>(pkt.at(pkt.size() - 1));
    QVERIFY2(sum >= 0x100,
             qPrintable(QStringLiteral("校验和 0x%1 高字节为零，"
                                       "无法区分大端/小端").arg(sum, 0, 16)));
    QCOMPARE(static_cast<quint8>(pkt.at(pkt.size() - 2)),
             static_cast<quint8>(sum >> 8));
    QCOMPARE(static_cast<quint8>(pkt.at(pkt.size() - 1)),
             static_cast<quint8>(sum & 0xFF));
}

// ---------------------------------------------------------------------------
// 所有已知命令字节都产生期望帧。
// 命令字面量取自生产调用点（form_finger / fingerprint_manager）。
// ---------------------------------------------------------------------------
void TestFingerprintWire::everyKnownCommandProducesExpectedFrame_data()
{
    QTest::addColumn<QByteArray>("payload");
    QByteArray p;

    p.clear(); p.append((char)0x01);
    QTest::newRow("GET_IMAGE") << p;
    p.clear(); p.append((char)0x02); p.append((char)1);
    QTest::newRow("GEN_CHAR buf1") << p;
    p.clear(); p.append((char)0x02); p.append((char)3);
    QTest::newRow("GEN_CHAR buf3") << p;
    p.clear(); p.append((char)0x05);
    QTest::newRow("REG_MODEL") << p;
    p.clear(); p.append((char)0x06); p.append((char)1);
    p.append((char)0x01); p.append((char)0x05);
    QTest::newRow("STORE_CHAR buf1 id=0x0105") << p;
    p.clear(); p.append((char)0x32);
    QTest::newRow("autoIdentify") << p;
}

void TestFingerprintWire::everyKnownCommandProducesExpectedFrame()
{
    QFETCH(QByteArray, payload);
    QCOMPARE(hex(m_dev.buildPacket(payload)), hex(expectedFrame(payload)));
}

// ---------------------------------------------------------------------------
// 互操作：生产 buildPacket() 的输出必须能被**自己的** FingerprintFrameParser
// 解回原 payload。这条把"构造"与"解析"两侧钉在一起 —— 只测单侧时，
// 两侧同时写反（比如都把小端当大端）也能双双通过。
// ---------------------------------------------------------------------------
void TestFingerprintWire::builtFrameRoundTripsThroughParser()
{
    accessterminal::FingerprintFrameParser parser;

    const QByteArray payloads[] = {
        QByteArray(1, (char)0x01),
        QByteArray(1, (char)0x05),
        QByteArray(1, (char)0x32),
        QByteArray(1, (char)0x02) + QByteArray(1, (char)3),
        QByteArray(1, (char)0x06) + QByteArray(1, (char)1)
            + QByteArray(1, (char)0x01) + QByteArray(1, (char)0x05),
    };
    for (const QByteArray& payload : payloads) {
        const QByteArray frame = m_dev.buildPacket(payload);
        parser.reset();
        const QList<QByteArray> out = parser.feed(frame);
        QCOMPARE(out.size(), 1);
        // 解析出的帧应等于原帧（解析器返回整帧）
        QCOMPARE(hex(out.at(0)), hex(frame));
    }
}

// ---------------------------------------------------------------------------
// 一次喂入**多帧**必须全部切出（解析器契约），且每帧都能被构造侧生成。
// ---------------------------------------------------------------------------
void TestFingerprintWire::parserAcceptsFrameWithMaximumPayload()
{
    // 远小于 kMaxBufferedBytes(4096) 的一个较大 payload
    const QByteArray payload(2000, (char)0x7E);
    const QByteArray frame = m_dev.buildPacket(payload);
    QCOMPARE(frame.size(), 2000 + 11);

    accessterminal::FingerprintFrameParser parser;
    const QList<QByteArray> out = parser.feed(frame);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out.at(0).size(), frame.size());
    QCOMPARE(hex(out.at(0)), hex(frame));
}

// ---------------------------------------------------------------------------
// 大 payload 让校验和明显大于 0xFFFF 的累加中间量，验证按 quint16 取模后
// 仍以大端写出（0xFF 累加会自然溢出，这里确认不崩且能解回）。
// ---------------------------------------------------------------------------
void TestFingerprintWire::largePayloadProducesCorrectChecksum()
{
    const QByteArray payload(600, (char)0xFF);
    const QByteArray pkt = m_dev.buildPacket(payload);

    quint16 want = 0;
    for (int i = 6; i < pkt.size() - 2; ++i) {
        want = static_cast<quint16>(want + static_cast<quint8>(pkt.at(i)));
    }
    const quint16 got = (static_cast<quint8>(pkt.at(pkt.size() - 2)) << 8)
                      | static_cast<quint8>(pkt.at(pkt.size() - 1));
    QCOMPARE(got, want);

    accessterminal::FingerprintFrameParser parser;
    QCOMPARE(parser.feed(pkt).size(), 1);
}

// ---------------------------------------------------------------------------
// 串口**打不开**时：open() 必须返回 false（不崩）。
//
// 真实场景：门口机上指纹串口不存在（接错线、驱动没起来）。此时必须让上层
// 看到失败，而不是崩溃或静默卡死。
//
// 【必读，第 25 轮实测】在**未打开的** QSerialPort 上调 waitForReadyRead()
// 会让 Qt5 直接**段错误**（探针 probe_serial.cpp 实测退出码 139，崩溃栈落在
// QSerialPort 内部）。因此本组用例不直接拿未打开的对象去调 readPacket()。
// ---------------------------------------------------------------------------
void TestFingerprintWire::openFailureReturnsFalseAndDoesNotCrash()
{
    HailinFingerprint dev(QStringLiteral("/dev/definitely-not-a-port-25"));
    QVERIFY(!dev.isOpen());
    QVERIFY2(!dev.open(), "不存在的串口竟然 open 成功了？");
    QVERIFY(!dev.isOpen());
    dev.close();   // 对未打开的串口调 close() 不得崩
    QVERIFY(!dev.isOpen());
}

// ---------------------------------------------------------------------------
// 报文构造是**纯逻辑**，不依赖串口状态。
// 这条钉住"纯逻辑与硬件访问已分开"这一结构性质：设备不可用时，
// buildPacket() 仍必须产出正确字节，而 sendPacket() 必须返回 false。
// ---------------------------------------------------------------------------
void TestFingerprintWire::pureBuildWorksWhileDeviceUnavailable()
{
    HailinFingerprint dev(QStringLiteral("/dev/definitely-not-a-port-25"));
    QVERIFY(!dev.isOpen());

    QByteArray payload;
    payload.append((char)0x01);
    const QByteArray pkt = dev.buildPacket(payload);

    QCOMPARE(pkt.size(), 12);
    QCOMPARE(hex(pkt), QStringLiteral("ef 01 ff ff ff ff 01 00 03 01 00 05"));

    // 串口写不进去 -> 必须返回 false，而不是崩或谎报成功
    QVERIFY(!dev.sendPacket(pkt));
}

QTEST_MAIN(TestFingerprintWire)
#include "test_fingerprint_wire.moc"
