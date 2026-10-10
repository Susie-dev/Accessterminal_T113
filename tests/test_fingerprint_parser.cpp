/**
 * @file test_fingerprint_parser.cpp
 * @brief 指纹串口帧解析器的单元测试。
 *
 * 覆盖的真实缺陷（见 src/hal/fingerprint_frame_parser.h 顶部说明）：
 *   - 一次 readAll() 收到多帧时，原实现只返回第一帧，其余字节被丢弃；
 *   - 超时返回时已收到的半包被丢弃，导致被截断的帧永远拼不齐；
 *   - 没有包头同步，噪声导致长度字段读到随机值后持续错位。
 *
 * 构建与运行：
 *   cmake -S . -B build -DACCESSTERMINAL_BUILD_TESTS=ON
 *   cmake --build build
 *   ctest --test-dir build --output-on-failure
 */
#include "hal/fingerprint_frame_parser.h"

#include <QtTest/QtTest>

using accessterminal::FingerprintFrameParser;

namespace {

/// 按与 HailinFingerprint::buildPacket() 完全相同的规则造一帧。
/// 保持独立实现（而非调用产品代码）才能真正验证解析器。
QByteArray makeFrame(quint8 packetTag, const QByteArray& payload)
{
    QByteArray pkt;
    pkt.append(char(0xEF));
    pkt.append(char(0x01));
    pkt.append(char(0xFF));
    pkt.append(char(0xFF));
    pkt.append(char(0xFF));
    pkt.append(char(0xFF));
    pkt.append(char(packetTag));
    const quint16 pktLen = quint16(payload.size() + 2); // payload + 校验和
    pkt.append(char((pktLen >> 8) & 0xFF));
    pkt.append(char(pktLen & 0xFF));
    pkt.append(payload);
    quint16 sum = 0;
    for (int i = 6; i < pkt.size(); ++i) {
        sum += quint8(pkt.at(i));
    }
    pkt.append(char((sum >> 8) & 0xFF));
    pkt.append(char(sum & 0xFF));
    return pkt;
}

/// 造一个“应答”帧：payload = 确认码 + param1 + param2
QByteArray makeAck(quint8 confirm, quint8 p1 = 0, quint8 p2 = 0)
{
    QByteArray payload;
    payload.append(char(0x00));
    payload.append(char(confirm));
    payload.append(char(p1));
    payload.append(char(p2));
    return makeFrame(0x07, payload);
}

} // namespace

class TestFingerprintParser : public QObject
{
    Q_OBJECT

private slots:
    // ---- 单帧：最基本的正确性 ----
    void parsesSingleCompleteFrame()
    {
        FingerprintFrameParser parser;
        const QByteArray frame = makeAck(0x00);
        const QList<QByteArray> got = parser.feed(frame);
        QCOMPARE(got.size(), 1);
        QCOMPARE(got.at(0), frame);
        QVERIFY(!parser.hasPartial());
        QCOMPARE(parser.bufferedBytes(), 0);
    }

    // ---- 核心缺陷 1：一次收到多帧必须全部产出，不能只返回第一帧 ----
    void parsesMultipleFramesInOneFeed()
    {
        FingerprintFrameParser parser;
        const QByteArray f1 = makeAck(0x00, 0x01);
        const QByteArray f2 = makeAck(0x00, 0x02);
        const QByteArray f3 = makeAck(0x00, 0x03);

        const QList<QByteArray> got = parser.feed(f1 + f2 + f3);
        QCOMPARE(got.size(), 3);
        QCOMPARE(got.at(0), f1);
        QCOMPARE(got.at(1), f2);
        QCOMPARE(got.at(2), f3);
        QCOMPARE(parser.bufferedBytes(), 0);
    }

    // ---- 核心缺陷 2：半包必须保留，下次喂入后拼齐 ----
    void buffersPartialFrameAcrossFeeds()
    {
        FingerprintFrameParser parser;
        const QByteArray frame = makeAck(0x00, 0xAA);
        const int split = frame.size() / 2;

        QCOMPARE(parser.feed(frame.left(split)).size(), 0);
        QVERIFY(parser.hasPartial());
        QVERIFY(parser.bufferedBytes() > 0);

        const QList<QByteArray> got = parser.feed(frame.mid(split));
        QCOMPARE(got.size(), 1);
        QCOMPARE(got.at(0), frame);
    }

    // ---- 逐字节到达：最严苛的拆包场景 ----
    void handlesByteByByteDelivery()
    {
        FingerprintFrameParser parser;
        const QByteArray frame = makeAck(0x00, 0x11, 0x22);
        QList<QByteArray> all;
        for (int i = 0; i < frame.size(); ++i) {
            all += parser.feed(frame.mid(i, 1));
        }
        QCOMPARE(all.size(), 1);
        QCOMPARE(all.at(0), frame);
    }

    // ---- 半包 + 后续多帧混在一起 ----
    void partialThenSeveralFrames()
    {
        FingerprintFrameParser parser;
        const QByteArray f1 = makeAck(0x00, 0x01);
        const QByteArray f2 = makeAck(0x00, 0x02);
        const QByteArray f3 = makeAck(0x00, 0x03);

        // 第一帧只给一半
        QCOMPARE(parser.feed(f1.left(3)).size(), 0);
        // 补齐第一帧的同时把 f2、f3 一起给进来
        const QList<QByteArray> got = parser.feed(f1.mid(3) + f2 + f3);
        QCOMPARE(got.size(), 3);
        QCOMPARE(got.at(0), f1);
        QCOMPARE(got.at(1), f2);
        QCOMPARE(got.at(2), f3);
    }

    // ---- 包头同步：前导噪声必须被丢弃，不能影响定帧 ----
    void resynchronizesPastLeadingNoise()
    {
        FingerprintFrameParser parser;
        const QByteArray frame = makeAck(0x00);
        const QByteArray noise = QByteArray::fromHex("0011223344");

        const QList<QByteArray> got = parser.feed(noise + frame);
        QCOMPARE(got.size(), 1);
        QCOMPARE(got.at(0), frame);
    }

    // ---- 包头同步：帧之间的噪声也要跳过 ----
    void skipsNoiseBetweenFrames()
    {
        FingerprintFrameParser parser;
        const QByteArray f1 = makeAck(0x00, 0x01);
        const QByteArray f2 = makeAck(0x00, 0x02);

        const QList<QByteArray> got =
            parser.feed(f1 + QByteArray::fromHex("DEADBEEF") + f2);
        QCOMPARE(got.size(), 2);
        QCOMPARE(got.at(0), f1);
        QCOMPARE(got.at(1), f2);
    }

    // ---- 伪包头（长度字段不合理）不能让解析卡死 ----
    void recoversFromBogusHeader()
    {
        FingerprintFrameParser parser;
        // 0xEF 0x01 后面跟一个荒谬的长度（0xFFFF），随后是真正的帧
        QByteArray bogus;
        bogus.append(char(0xEF));
        bogus.append(char(0x01));
        bogus.append(char(0xFF));
        bogus.append(char(0xFF));
        const QByteArray frame = makeAck(0x00, 0x55);

        const QList<QByteArray> got = parser.feed(bogus + frame);
        QCOMPARE(got.size(), 1);
        QCOMPARE(got.at(0), frame);
    }

    // ---- 缓冲区不会无限增长 ----
    void boundsUnboundedBuffer()
    {
        FingerprintFrameParser parser;
        parser.feed(QByteArray(64 * 1024, char(0x5A))); // 全是噪声，无包头
        QVERIFY(parser.bufferedBytes() <= FingerprintFrameParser::kMaxBufferedBytes);
        // 之后仍能正常定帧
        const QByteArray frame = makeAck(0x00, 0x77);
        const QList<QByteArray> got = parser.feed(frame);
        QCOMPARE(got.size(), 1);
        QCOMPARE(got.at(0), frame);
    }

    // ---- reset 必须丢弃半包（跨串口会话不能串包） ----
    void resetDiscardsPartial()
    {
        FingerprintFrameParser parser;
        parser.feed(makeAck(0x00).left(4));
        QVERIFY(parser.hasPartial());

        parser.reset();
        QVERIFY(!parser.hasPartial());
        QCOMPARE(parser.bufferedBytes(), 0);

        // reset 后的残留不应再与新数据拼成帧
        QCOMPARE(parser.feed(QByteArray::fromHex("00")).size(), 0);
    }

    // ---- empty feed 只尝试取出已完整的帧 ----
    void emptyFeedStillDrainsCompleteFrames()
    {
        FingerprintFrameParser parser;
        const QByteArray frame = makeAck(0x00);
        QCOMPARE(parser.feed(frame).size(), 1);
        QCOMPARE(parser.feed(QByteArray()).size(), 0);
    }

    // ---- 校验和：正确帧通过，被篡改的帧不通过 ----
    void checksumValidation()
    {
        const QByteArray frame = makeAck(0x00, 0x12, 0x34);
        QVERIFY(FingerprintFrameParser::checksumMatches(frame));

        QByteArray tampered = frame;
        tampered[tampered.size() - 1] = char(quint8(tampered.at(tampered.size() - 1)) ^ 0xFF);
        QVERIFY(!FingerprintFrameParser::checksumMatches(tampered));

        // 太短的一律不通过
        QVERIFY(!FingerprintFrameParser::checksumMatches(QByteArray()));
        QVERIFY(!FingerprintFrameParser::checksumMatches(frame.left(5)));
    }

    // ---- 与产品侧 buildPacket 规则自洽：校验和覆盖范围从下标 6 起 ----
    void checksumCoversFromTagField()
    {
        const QByteArray frame = makeAck(0x00, 0xAB, 0xCD);
        const QByteArray body = frame.left(frame.size() - 2);
        const quint16 expected = FingerprintFrameParser::checksumOf(body);
        const quint16 actual =
            quint16((quint8(frame.at(frame.size() - 2)) << 8) |
                    quint8(frame.at(frame.size() - 1)));
        QCOMPARE(actual, expected);
    }

    // ---- 解析出的帧长度必须与长度字段一致 ----
    void frameLengthMatchesLengthField()
    {
        FingerprintFrameParser parser;
        const QByteArray frame = makeAck(0x00, 0x01, 0x02);
        const QList<QByteArray> got = parser.feed(frame);
        QCOMPARE(got.size(), 1);

        const quint16 lenField =
            quint16((quint8(got.at(0).at(7)) << 8) | quint8(got.at(0).at(8)));
        QCOMPARE(got.at(0).size(),
                 FingerprintFrameParser::kHeaderBytes + int(lenField));
    }
};

QTEST_MAIN(TestFingerprintParser)
#include "test_fingerprint_parser.moc"
