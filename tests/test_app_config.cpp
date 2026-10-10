/**
 * @file test_app_config.cpp
 * @brief AppConfig 的单元测试：默认值契约 + 不可信输入的数值兜底。
 *
 * 为什么需要它：本工程的配置是可被**外部**影响的输入（JSON 文件、环境变量），
 * 而第 8、9 轮在真实代码里修掉的两个缺陷都出在这里，却一直没有可执行测试：
 *
 *   1. SIGFPE（第 8 轮）：`face_detect_interval` 被改成可配置后，
 *      `"face_detect_interval": 0` 会让 widget.cpp 的
 *      `++detectCounter % cfg.faceDetectInterval()` 除零，进程直接崩溃。
 *      环境变量写成非数字（`=abc`）也会经 QString::toInt() 变成 0，
 *      同样可达。修法是在**取值出口**统一 clamp。
 *   2. 假开关（第 9 轮）：`face_confidence_threshold` 配了却不生效
 *      （FaceManager 里硬编码 90）。本测试固定"配置值确实被访问器读出来"，
 *      至于"是否真的被消费"由 tools/verify.py 的
 *      check_config_knobs_are_used 守卫。
 *
 * 本测试只依赖 Qt5::Core，不需要 ALSA / libgpiod / OpenCV，也无需硬件。
 */
#include <QtTest/QtTest>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "config/app_config.h"

using accessterminal::AppConfig;

namespace {

/// 写一个临时 JSON 配置文件，返回其路径。
QString writeConfig(const QTemporaryDir& dir, const QByteArray& json)
{
    const QString path = dir.filePath(QStringLiteral("appconfig.json"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return QString();
    }
    f.write(json);
    f.close();
    return path;
}

/// 清掉本测试可能设置的全部环境变量，避免用例之间互相污染。
void clearEnv()
{
    const char* names[] = {
        "SMARTPRO_SERVER_IP", "SMARTPRO_SELF_IP",
        "ACCESSTERMINAL_NETWORK_SERVER_IP", "ACCESSTERMINAL_NETWORK_SELF_IP",
        "ACCESSTERMINAL_HARDWARE_CAMERA_DEVICE",
        "ACCESSTERMINAL_HARDWARE_FINGERPRINT_PORT",
        "ACCESSTERMINAL_HARDWARE_AUDIO_DEVICE",
        "ACCESSTERMINAL_HARDWARE_DOOR_GPIO_LINE",
        "ACCESSTERMINAL_RESOURCES_FACE_CASCADE",
        "ACCESSTERMINAL_RESOURCES_PCM_DIR",
        "ACCESSTERMINAL_BEHAVIOR_FRAME_INTERVAL_MS",
        "ACCESSTERMINAL_BEHAVIOR_FACE_DETECT_INTERVAL",
        "ACCESSTERMINAL_BEHAVIOR_FACE_RECOGNIZE_INTERVAL_MS",
        "ACCESSTERMINAL_BEHAVIOR_FACE_CONFIDENCE_THRESHOLD",
        "ACCESSTERMINAL_BEHAVIOR_FINGERPRINT_ID_START",
        "ACCESSTERMINAL_SECURITY_DOOR_PASSWORD",
    };
    for (const char* n : names) {
        qunsetenv(n);
    }
}

} // namespace

class TestAppConfig : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void defaultsMatchBaseline();
    void clampHelpersBoundaries();
    void zeroDetectIntervalDoesNotReachModulo();
    void garbageEnvFallsBackToSafeRange();
    void legalValuesPassThroughUnchanged();
    void outOfRangeJsonIsClamped();
    void brokenJsonKeepsDefaults();
    void legacyEnvVarsStillHonored();
    void fingerprintIdStartMatchesWireRange();
    void pcmFileJoinsDirectory();
};

void TestAppConfig::init() { clearEnv(); }
void TestAppConfig::cleanup() { clearEnv(); }

/**
 * 内置默认值必须与重构前的硬编码逐字节一致 —— 这是"不配置也能按原行为运行"
 * 的前提，也是本次重构最容易被悄悄改坏的地方（下面每个值都能在 baseline
 * 里找到出处，见注释）。
 */
void TestAppConfig::defaultsMatchBaseline()
{
    AppConfig& cfg = AppConfig::instance();
    // 指向一个必定不存在的路径，确保只使用内置默认值。
    QVERIFY(!cfg.load(QStringLiteral("/nonexistent/definitely-missing.json")));

    QCOMPARE(cfg.serverIp(), QStringLiteral("192.168.1.4"));   // appconfig.h:9
    QCOMPARE(cfg.selfIp(), QStringLiteral("192.168.1.8"));     // appconfig.h:15
    QCOMPARE(cfg.cameraDevice(), QStringLiteral("/dev/video0"));       // widget.cpp:307
    QCOMPARE(cfg.fingerprintPort(), QStringLiteral("/dev/ttyS4"));     // widget.cpp:178
    QCOMPARE(cfg.audioDevice(), QStringLiteral("hw:audiocodec"));      // audio_manager.cpp:70
    QCOMPARE(cfg.doorGpioLine(), 133);                                 // widget.cpp:172 (PE5)
    QCOMPARE(cfg.faceCascadePath(),
             QStringLiteral("/root/haarcascade_frontalface_default.xml"));  // widget.cpp:289
    QCOMPARE(cfg.pcmDir(), QStringLiteral("/tmp/pcm"));                // widget.cpp:208 等
    QCOMPARE(cfg.frameIntervalMs(), 50);                               // widget.cpp:330
    QCOMPARE(cfg.faceDetectInterval(), 10);                            // widget.cpp:1119
    QCOMPARE(cfg.faceRecognizeIntervalMs(), 2000);
    QCOMPARE(cfg.faceConfidenceThreshold(), 90.0);                     // face_manager.cpp:95
    QCOMPARE(cfg.fingerprintIdStart(), 1);
    QCOMPARE(cfg.doorPassword(), QStringLiteral("5788"));
}

/// clampInt/clampDouble 的边界语义（含"闭区间、等于边界不改变"）。
void TestAppConfig::clampHelpersBoundaries()
{
    using accessterminal::clampInt;
    using accessterminal::clampDouble;

    QCOMPARE(clampInt(5, 1, 10), 5);
    QCOMPARE(clampInt(1, 1, 10), 1);    // 等于下界：不变
    QCOMPARE(clampInt(10, 1, 10), 10);  // 等于上界：不变
    QCOMPARE(clampInt(0, 1, 10), 1);
    QCOMPARE(clampInt(-999, 1, 10), 1);
    QCOMPARE(clampInt(11, 1, 10), 10);
    QCOMPARE(clampInt(INT_MIN, 1, 10), 1);
    QCOMPARE(clampInt(INT_MAX, 1, 10), 10);

    QCOMPARE(clampDouble(0.5, 0.0, 1.0), 0.5);
    QCOMPARE(clampDouble(-1.0, 0.0, 1.0), 0.0);
    QCOMPARE(clampDouble(2.0, 0.0, 1.0), 1.0);
}

/**
 * 核心回归（第 8 轮的 SIGFPE）：检测间隔在**任何**输入下都不得 < 1。
 *
 * 用 0、负数、以及"看起来像数字但 toInt 解析为 0"的形式，模拟真实可达路径。
 */
void TestAppConfig::zeroDetectIntervalDoesNotReachModulo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // ---- JSON 形式 ----
    const QByteArray jsons[] = {
        "{\"behavior\":{\"face_detect_interval\":0}}",
        "{\"behavior\":{\"face_detect_interval\":-5}}",
        "{\"behavior\":{\"face_detect_interval\":0.4}}",   // 取整为 0
        "{\"behavior\":{\"face_detect_interval\":\"10\"}}", // 类型不符 -> 用默认
    };
    for (const QByteArray& j : jsons) {
        const QString path = writeConfig(dir, j);
        AppConfig& cfg = AppConfig::instance();
        cfg.load(path);
        QVERIFY2(cfg.faceDetectInterval() >= 1,
                 qPrintable(QStringLiteral("faceDetectInterval()=%1 会触发取模除零")
                                .arg(cfg.faceDetectInterval())));
    }

    // ---- 环境变量形式（toInt 对非法文本返回 0，是真实可达路径）----
    const char* bad[] = {"0", "-1", "abc", "", "1e3", "0x10"};
    for (const char* v : bad) {
        clearEnv();
        qputenv("ACCESSTERMINAL_BEHAVIOR_FACE_DETECT_INTERVAL", QByteArray(v));
        AppConfig& cfg = AppConfig::instance();
        cfg.load(QStringLiteral("/nonexistent/definitely-missing.json"));
        QVERIFY2(cfg.faceDetectInterval() >= 1,
                 qPrintable(QStringLiteral("env=\"%1\" -> faceDetectInterval()=%2")
                                .arg(QString::fromLatin1(v))
                                .arg(cfg.faceDetectInterval())));
    }

    // 画面刷新周期同样不得为 0/负（否则 QTimer 忙转）。
    clearEnv();
    qputenv("ACCESSTERMINAL_BEHAVIOR_FRAME_INTERVAL_MS", QByteArray("0"));
    AppConfig& cfg = AppConfig::instance();
    cfg.load(QStringLiteral("/nonexistent/definitely-missing.json"));
    QVERIFY(cfg.frameIntervalMs() >= 1);
}

/// 非法环境变量不能让任何数值字段落到危险区间。
void TestAppConfig::garbageEnvFallsBackToSafeRange()
{
    clearEnv();
    qputenv("ACCESSTERMINAL_HARDWARE_DOOR_GPIO_LINE", QByteArray("not-a-number"));
    qputenv("ACCESSTERMINAL_BEHAVIOR_FINGERPRINT_ID_START", QByteArray("nope"));
    qputenv("ACCESSTERMINAL_BEHAVIOR_FACE_CONFIDENCE_THRESHOLD", QByteArray("xx"));
    qputenv("ACCESSTERMINAL_BEHAVIOR_FACE_RECOGNIZE_INTERVAL_MS", QByteArray("?"));

    AppConfig& cfg = AppConfig::instance();
    cfg.load(QStringLiteral("/nonexistent/definitely-missing.json"));

    QVERIFY(cfg.doorGpioLine() >= 0);
    QVERIFY(cfg.fingerprintIdStart() >= 0);
    QVERIFY(cfg.faceConfidenceThreshold() >= 0.0);
    QVERIFY(cfg.faceRecognizeIntervalMs() >= 0);
}

/// 合法值必须原样通过兜底（不能因为加了下限而改变正常配置的行为）。
void TestAppConfig::legalValuesPassThroughUnchanged()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeConfig(dir,
        "{\"network\":{\"server_ip\":\"10.0.0.1\",\"self_ip\":\"10.0.0.2\"},"
        "\"hardware\":{\"camera_device\":\"/dev/video2\","
        "\"fingerprint_port\":\"/dev/ttyS9\",\"audio_device\":\"plughw:0,0\","
        "\"door_gpio_line\":200},"
        "\"resources\":{\"face_cascade\":\"/opt/cascade.xml\",\"pcm_dir\":\"/opt/pcm\"},"
        "\"behavior\":{\"frame_interval_ms\":33,\"face_detect_interval\":7,"
        "\"face_recognize_interval_ms\":1500,\"face_confidence_threshold\":77.5,"
        "\"fingerprint_id_start\":42},"
        "\"security\":{\"door_password\":\"1234\"}}");

    AppConfig& cfg = AppConfig::instance();
    QVERIFY(cfg.load(path));
    QCOMPARE(cfg.configPath(), path);
    QCOMPARE(cfg.serverIp(), QStringLiteral("10.0.0.1"));
    QCOMPARE(cfg.selfIp(), QStringLiteral("10.0.0.2"));
    QCOMPARE(cfg.cameraDevice(), QStringLiteral("/dev/video2"));
    QCOMPARE(cfg.fingerprintPort(), QStringLiteral("/dev/ttyS9"));
    QCOMPARE(cfg.audioDevice(), QStringLiteral("plughw:0,0"));
    QCOMPARE(cfg.doorGpioLine(), 200);
    QCOMPARE(cfg.faceCascadePath(), QStringLiteral("/opt/cascade.xml"));
    QCOMPARE(cfg.pcmDir(), QStringLiteral("/opt/pcm"));
    QCOMPARE(cfg.frameIntervalMs(), 33);
    QCOMPARE(cfg.faceDetectInterval(), 7);
    QCOMPARE(cfg.faceRecognizeIntervalMs(), 1500);
    QCOMPARE(cfg.faceConfidenceThreshold(), 77.5);
    QCOMPARE(cfg.fingerprintIdStart(), 42);
    QCOMPARE(cfg.doorPassword(), QStringLiteral("1234"));
}

/// 超范围值必须被夹到文档化的区间，且不影响范围内的其它字段。
void TestAppConfig::outOfRangeJsonIsClamped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeConfig(dir,
        "{\"hardware\":{\"door_gpio_line\":-1},"
        "\"behavior\":{\"frame_interval_ms\":999999,"
        "\"fingerprint_id_start\":70000,"
        "\"face_confidence_threshold\":-3.0}}");

    AppConfig& cfg = AppConfig::instance();
    cfg.load(path);
    QCOMPARE(cfg.doorGpioLine(), 0);          // 负线号夹到 0
    QCOMPARE(cfg.frameIntervalMs(), 10000);   // 上限
    QCOMPARE(cfg.fingerprintIdStart(), 65535);// 线格式为 2 字节
    QCOMPARE(cfg.faceConfidenceThreshold(), 0.0);
}

/// JSON 损坏时必须整体回退到默认值，而不是留下"半套"配置。
void TestAppConfig::brokenJsonKeepsDefaults()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray broken[] = {
        "{ this is not json",
        "[1,2,3]",           // 根不是对象
        "",
    };
    for (const QByteArray& b : broken) {
        const QString path = writeConfig(dir, b);
        AppConfig& cfg = AppConfig::instance();
        QVERIFY(!cfg.load(path));   // 未成功载入
        QCOMPARE(cfg.serverIp(), QStringLiteral("192.168.1.4"));
        QCOMPARE(cfg.pcmDir(), QStringLiteral("/tmp/pcm"));
        QCOMPARE(cfg.doorPassword(), QStringLiteral("5788"));
    }
}

/// 重构前的旧环境变量名必须仍然生效（现网启动脚本不能失效）。
void TestAppConfig::legacyEnvVarsStillHonored()
{
    clearEnv();
    qputenv("SMARTPRO_SERVER_IP", QByteArray("172.16.0.9"));
    qputenv("SMARTPRO_SELF_IP", QByteArray("172.16.0.10"));

    AppConfig& cfg = AppConfig::instance();
    cfg.load(QStringLiteral("/nonexistent/definitely-missing.json"));
    QCOMPARE(cfg.serverIp(), QStringLiteral("172.16.0.9"));
    QCOMPARE(cfg.selfIp(), QStringLiteral("172.16.0.10"));

    // 新命名必须能覆盖旧命名（优先级更高）。
    qputenv("ACCESSTERMINAL_NETWORK_SERVER_IP", QByteArray("172.16.0.11"));
    cfg.load(QStringLiteral("/nonexistent/definitely-missing.json"));
    QCOMPARE(cfg.serverIp(), QStringLiteral("172.16.0.11"));

    // 空值不生效（应保留上一步的值而非被清空）。
    clearEnv();
    qputenv("SMARTPRO_SERVER_IP", QByteArray(""));
    cfg.load(QStringLiteral("/nonexistent/definitely-missing.json"));
    QCOMPARE(cfg.serverIp(), QStringLiteral("192.168.1.4"));
}

/// 指纹 ID 用 2 字节承载，取值必须落在 [0, 65535]。
void TestAppConfig::fingerprintIdStartMatchesWireRange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const struct { const char* json; int expect; } cases[] = {
        {"{\"behavior\":{\"fingerprint_id_start\":0}}", 0},
        {"{\"behavior\":{\"fingerprint_id_start\":1}}", 1},
        {"{\"behavior\":{\"fingerprint_id_start\":65535}}", 65535},
        {"{\"behavior\":{\"fingerprint_id_start\":65536}}", 65535},
        {"{\"behavior\":{\"fingerprint_id_start\":-1}}", 0},
    };
    for (const auto& c : cases) {
        const QString path = writeConfig(dir, QByteArray(c.json));
        AppConfig& cfg = AppConfig::instance();
        cfg.load(path);
        QCOMPARE(cfg.fingerprintIdStart(), c.expect);
        QVERIFY(cfg.fingerprintIdStart() >= 0 && cfg.fingerprintIdStart() <= 65535);
    }
}

/// 提示音路径统一由 pcmDir 拼接（确保目录配置真正被用上）。
void TestAppConfig::pcmFileJoinsDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeConfig(dir,
        "{\"resources\":{\"pcm_dir\":\"/opt/pcm\"}}");
    AppConfig& cfg = AppConfig::instance();
    cfg.load(path);

    const QString joined = cfg.pcmFile(QStringLiteral("opendoor.pcm"));
    QCOMPARE(joined, QStringLiteral("/opt/pcm/opendoor.pcm"));
    QVERIFY(joined.startsWith(cfg.pcmDir()));
}

QTEST_MAIN(TestAppConfig)
#include "test_app_config.moc"
