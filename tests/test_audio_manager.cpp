/**
 * @file test_audio_manager.cpp
 * @brief AudioManager 的单元测试：写循环 / XRUN 恢复 / 打断 / 析构等待。
 *
 * 为什么必须测：AudioManager 是本工程出过**两个真实缺陷**的地方
 * （第 11 轮 XRUN 静默丢数据、第 12 轮 playAsync 漏等待导致 use-after-free），
 * 却一直没有可执行测试 —— 它的所有危险分支都在"设备报错 / 被打断 / 并发"
 * 上，而这些分支在真实设备上**无法按需复现**。本测试链接一份可编程的假
 * ALSA（tests/fake_alsa.cpp），用失败注入把这些从未被执行过的分支真正跑一遍。
 *
 * 注意：这里验证的是 AudioManager 的控制流，不是 ALSA 行为，也不产出可运行
 * 的播放器。
 */
#include <QtTest/QtTest>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFuture>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVector>

#include <atomic>
#include <memory>

#include "fake_alsa.h"

// 测试需要调用私有成员 waitForAllTasks()（析构函数用它做同步）。
// 仅在本文件内把 private 当作 public 解析：audio_manager.h 依赖的 Qt/标准库
// 头文件都已在上面包含（有 include guard，不会被重新解析），因此这一开关
// 影响面被限制在本头文件内，不改动任何生产代码。
#define private public
#include "managers/audio_manager.h"
#undef private

namespace {

/// 生成一段 PCM 数据：n 个 320 字节（160 帧）的块，内容可区分。
QByteArray makePcm(int chunks, char fill)
{
    QByteArray data;
    data.reserve(chunks * 320);
    for (int i = 0; i < chunks; ++i) {
        for (int b = 0; b < 320; ++b) {
            data.append(static_cast<char>(fill + (b % 7)));
        }
    }
    return data;
}

QString writePcm(const QTemporaryDir& dir, const QString& name, const QByteArray& data)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return QString();
    }
    f.write(data);
    f.close();
    return path;
}

} // namespace

class TestAudioManager : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void normalPlaybackWritesEveryChunkAndDrains();
    void negotiatesProtocolAudioFormat();
    void recoversFromXrunWithoutLosingData();
    void survivesXrunBeyondRetryLimit();
    void fatalWriteErrorDoesNotAbortRemainingChunks();
    void interruptDropsInsteadOfDraining();
    void nextPlaybackInterruptsPreviousTask();
    void destructorWaitsForAllRunningTasks();
    void openFailureIsHandledGracefully();
    void missingFileIsHandledGracefully();
    void playbackFinishedFiresEvenWhenInterrupted();
};

void TestAudioManager::init()
{
    fakealsa::reset();
}

/// 正常路径：每个数据块都写出去，并且收尾用 drain（不是 drop）。
void TestAudioManager::normalPlaybackWritesEveryChunkAndDrains()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray pcm = makePcm(5, 0x10);
    const QString path = writePcm(dir, QStringLiteral("tone.pcm"), pcm);
    QVERIFY(!path.isEmpty());

    AudioManager mgr;
    mgr.playAsync(path);
    mgr.waitForAllTasks();

    QCOMPARE(fakealsa::openCount(), 1);
    QCOMPARE(fakealsa::closeCount(), 1);
    QCOMPARE(fakealsa::drainCount(), 1);
    QCOMPARE(fakealsa::dropCount(), 0);
    // 5 块 * 160 帧
    QCOMPARE(fakealsa::framesWritten(), 5 * 160);
    QCOMPARE(static_cast<int>(fakealsa::chunks().size()), 5);

    // 写入内容必须与文件逐字节一致（不能错位、不能重复写）
    int offset = 0;
    const std::vector<std::vector<char> > got = fakealsa::chunks();
    for (size_t i = 0; i < got.size(); ++i) {
        const QByteArray expect = pcm.mid(offset, 320);
        QCOMPARE(static_cast<int>(got[i].size()), 320);
        QVERIFY(memcmp(got[i].data(), expect.constData(), 320) == 0);
        offset += 320;
    }
}

/// 协商到的硬件参数必须与协议的音频格式一致：S16_LE / 16000 / 单声道。
void TestAudioManager::negotiatesProtocolAudioFormat()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writePcm(dir, QStringLiteral("t.pcm"), makePcm(2, 1));

    AudioManager mgr;
    mgr.playAsync(path);
    mgr.waitForAllTasks();

    QCOMPARE(fakealsa::lastRate(), 16000);
    QCOMPARE(fakealsa::lastChannels(), 1);
    QCOMPARE(fakealsa::lastFormat(), 2); // SND_PCM_FORMAT_S16_LE
}

/**
 * 核心回归（第 11 轮缺陷）：XRUN 之后必须 prepare 并**重写同一包**。
 *
 * 重构前的代码遇到 -EPIPE 只打印一条警告就继续下一次循环，而 ALSA 在
 * XRUN 后**每次** writei 都会立刻返回 -EPIPE，于是本段提示音剩余数据全部
 * 被静默丢弃，且循环瞬间跑完 —— 现场表现为"提示音没响/只响了个开头"。
 */
void TestAudioManager::recoversFromXrunWithoutLosingData()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray pcm = makePcm(4, 0x20);
    const QString path = writePcm(dir, QStringLiteral("x.pcm"), pcm);

    fakealsa::setEpipeFirst(1); // 第 1 次写入失败，模拟一次 XRUN

    AudioManager mgr;
    mgr.playAsync(path);
    mgr.waitForAllTasks();

    // 关键断言：4 个块的数据**一个都不能少**
    QCOMPARE(fakealsa::framesWritten(), 4 * 160);
    QCOMPARE(static_cast<int>(fakealsa::chunks().size()), 4);
    // 且确实做了复位
    QVERIFY(fakealsa::prepareCount() >= 1);
    // 5 次调用 = 1 次失败 + 4 次成功
    QCOMPARE(fakealsa::writeiCount(), 5);
    QVERIFY(fakealsa::dropCount() == 0);

    // 内容仍然正确（说明是"重写同一包"，不是跳过了那一包）
    const std::vector<std::vector<char> > got = fakealsa::chunks();
    QVERIFY(memcmp(got[0].data(), pcm.constData(), 320) == 0);
}

/// 连续 XRUN 次数超过重试上限时，不能卡死，也不能吞掉后续所有包。
void TestAudioManager::survivesXrunBeyondRetryLimit()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray pcm = makePcm(3, 0x30);
    const QString path = writePcm(dir, QStringLiteral("y.pcm"), pcm);

    fakealsa::setEpipeFirst(100); // 全部都 EPIPE

    AudioManager mgr;
    mgr.playAsync(path);
    mgr.waitForAllTasks();

    // 3 块 * 最多 3 次重试 = 9 次调用后放弃该包，继续下一包
    QVERIFY(fakealsa::writeiCount() >= 9);
    // 不应丢失"继续处理后续包"的能力：调用次数应覆盖全部 3 块
    QVERIFY2(fakealsa::writeiCount() >= 3 * 3,
             "超过重试上限后必须继续处理后续数据块，而不是提前结束");
    QCOMPARE(fakealsa::closeCount(), 1);
}

/// 不可恢复的写错误（如 -EIO）只告警，不能让剩余提示音被跳过。
void TestAudioManager::fatalWriteErrorDoesNotAbortRemainingChunks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray pcm = makePcm(3, 0x40);
    const QString path = writePcm(dir, QStringLiteral("z.pcm"), pcm);

    fakealsa::setFatalError(true);

    AudioManager mgr;
    mgr.playAsync(path);
    mgr.waitForAllTasks();

    QCOMPARE(fakealsa::writeiCount(), 3); // 每块都尝试过
    QCOMPARE(fakealsa::closeCount(), 1);  // 句柄仍然被释放
}

/// 被打断时必须 drop（立即丢弃缓冲），而不是 drain（把尾巴播完）。
void TestAudioManager::interruptDropsInsteadOfDraining()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray pcm = makePcm(200, 0x50); // 足够长
    const QString path = writePcm(dir, QStringLiteral("long.pcm"), pcm);

    fakealsa::setWriteDelayMs(10); // 放慢，保证打断发生在播放中途

    AudioManager mgr;
    mgr.playAsync(path);
    QTest::qWait(60); // 让它写出去几块
    mgr.stop();
    mgr.waitForAllTasks();

    QVERIFY2(fakealsa::dropCount() >= 1, "被打断时应调用 snd_pcm_drop");
    QCOMPARE(fakealsa::drainCount(), 0);
    QVERIFY2(fakealsa::framesWritten() < 200 * 160,
             "被打断后不应把整段提示音播完");
}

/**
 * 推翻一个长期未被验证的论断。
 *
 * 此前我在第 12 轮的注释里写过："旧任务的 flag 已不再是 interruptFlag 指向的
 * 对象，stop() 再也打不断它"。**这个说法是错的**：旧任务捕获的是 shared_ptr
 * 的**拷贝**，它指向的是**同一个** atomic 对象；playAsync() 开头的 stop()
 * 正是把那个对象的置为 true，所以旧任务在下一次循环判断时就会退出。
 *
 * 本用例用慢速设备 + 长提示音把这个时序固定下来：A 还在播时启动 B，
 * A 必须被 drop 打断，而不是把整段播完。
 */
void TestAudioManager::nextPlaybackInterruptsPreviousTask()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray longPcm = makePcm(300, 0x60);
    const QByteArray shortPcm = makePcm(2, 0x70);
    const QString a = writePcm(dir, QStringLiteral("a.pcm"), longPcm);
    const QString b = writePcm(dir, QStringLiteral("b.pcm"), shortPcm);

    fakealsa::setWriteDelayMs(10);

    AudioManager mgr;
    mgr.playAsync(a);
    QTest::qWait(70);          // A 已开始播放，但远未播完
    const int partial = fakealsa::framesWritten();
    QVERIFY2(partial > 0 && partial < 300 * 160, "A 应当仍在播放中");

    mgr.playAsync(b);          // 应当打断 A
    mgr.waitForAllTasks();

    QVERIFY2(fakealsa::dropCount() >= 1,
             "上一次播放必须能被后续 playAsync 打断（旧 flag 与成员指向同一对象）");
    QVERIFY2(fakealsa::framesWritten() < 300 * 160 + 2 * 160,
             "A 不应把整段 300 块全部播完");
    // B 的数据必须完整写出
    QVERIFY(fakealsa::framesWritten() >= 2 * 160);
}

/**
 * 析构后不得有任务继续访问已释放的对象。
 *
 * 诚实说明本用例的**局限**（第 16 轮实测）：
 * 把 ~AudioManager() 还原成"只等待最后一个 future"后，本用例**仍然通过**。
 * 原因是 `playPcmFile()` 入口持有静态 `pcmMutex`，所有播放任务被完全串行化 ——
 * 后提交的任务必须先等前者释放锁，于是"只等最后一个"往往传递性地也等到了
 * 更早的任务。换言之：第 12 轮那处"漏等待"修复消除的是一个**窗口**
 * （任务在锁外 emit 信号期间对象被析构），而不是一个必然触发的崩溃。
 *
 * 因此这里只断言**确实成立且确实有意义**的性质：
 *   1. 析构返回时，所有已打开的设备句柄都已关闭（无泄漏）；
 *   2. 析构返回后不再有新任务开始（计数不再增长）。
 * 不做"还原修复就会失败"的强断言，以免写出一条永远为真的测试。
 */
void TestAudioManager::destructorWaitsForAllRunningTasks()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray pcm = makePcm(60, 0x80);
    const QString path = writePcm(dir, QStringLiteral("d.pcm"), pcm);

    fakealsa::setWriteDelayMs(3);

    {
        AudioManager mgr;
        // 连续多次启动，制造多个"旧任务"
        mgr.playAsync(path);
        mgr.playAsync(path);
        mgr.playAsync(path);
        QTest::qWait(20);
        // 离开作用域：析构必须阻塞到所有在跑的任务结束
    }

    // 析构返回瞬间：句柄必须已配平（不能有任务还在持有设备）
    QCOMPARE(fakealsa::openCount(), fakealsa::closeCount());

    const int openAfterDtor = fakealsa::openCount();
    const int closeAfterDtor = fakealsa::closeCount();
    QTest::qWait(120); // 给"漏网"的任务充分时间暴露自己

    QCOMPARE(fakealsa::openCount(), openAfterDtor);
    QCOMPARE(fakealsa::closeCount(), closeAfterDtor);
}

/// 打不开设备：只告警并返回，不能崩溃、不能泄漏句柄。
void TestAudioManager::openFailureIsHandledGracefully()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writePcm(dir, QStringLiteral("e.pcm"), makePcm(3, 0x90));

    fakealsa::setOpenFail(true);

    AudioManager mgr;
    mgr.playAsync(path);
    mgr.waitForAllTasks();

    QCOMPARE(fakealsa::openCount(), 0);  // 从未成功打开
    QCOMPARE(fakealsa::closeCount(), 0); // 也就没有多余的 close
    QCOMPARE(fakealsa::writeiCount(), 0);
}

/// 提示音文件不存在：应关闭已打开的句柄，不泄漏。
void TestAudioManager::missingFileIsHandledGracefully()
{
    AudioManager mgr;
    mgr.playAsync(QStringLiteral("/nonexistent/definitely-missing.pcm"));
    mgr.waitForAllTasks();

    QCOMPARE(fakealsa::openCount(), 1);
    QCOMPARE(fakealsa::closeCount(), 1); // 关键：打开后必须关闭
    QCOMPARE(fakealsa::writeiCount(), 0);
    QCOMPARE(fakealsa::drainCount(), 0);
}

/**
 * 记录既有行为：被打断时 playbackFinished 仍然会发射。
 *
 * 这不是本题考查点，而是把"信号在打断后也会到"这个事实钉住，因为
 * Widget::on_btncall_clicked() 依赖 playbackFinished(calling.pcm) 去接着播
 * conn.pcm。若将来改为"打断时不发信号"，这条用例会失败并提醒我们检查那处
 * 连接是否还需要调整。
 */
void TestAudioManager::playbackFinishedFiresEvenWhenInterrupted()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writePcm(dir, QStringLiteral("f.pcm"), makePcm(200, 0xA0));

    fakealsa::setWriteDelayMs(10);

    AudioManager mgr;
    QSignalSpy finished(&mgr, SIGNAL(playbackFinished(QString)));
    QSignalSpy started(&mgr, SIGNAL(playbackStarted(QString)));

    mgr.playAsync(path);
    QTest::qWait(60);
    mgr.stop();
    mgr.waitForAllTasks();

    QCOMPARE(started.count(), 1);
    QCOMPARE(finished.count(), 1); // 被打断也会发
    QVERIFY(fakealsa::dropCount() >= 1);
}

QTEST_MAIN(TestAudioManager)
#include "test_audio_manager.moc"
