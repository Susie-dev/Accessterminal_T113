/**
 * @file app_config.cpp
 * @brief AppConfig 实现：默认值 + JSON 文件 + 环境变量三级覆盖。
 */
#include "config/app_config.h"

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace accessterminal {

namespace {

/// 默认配置路径。部署脚本会把 config/appconfig.json 安装到这里。
const char* kDefaultConfigPath = "/etc/accessterminal/appconfig.json";

/**
 * @brief 读取字符串字段，带类型校验。
 * @return JSON 中该键存在且为字符串时返回其值，否则返回 fallback。
 */
QString jsonString(const QJsonObject& obj, const char* key, const QString& fallback)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    return v.isString() ? v.toString() : fallback;
}

/// 读取整数字段；JSON 数字为 double，此处取整。
int jsonInt(const QJsonObject& obj, const char* key, int fallback)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    return v.isDouble() ? static_cast<int>(v.toDouble()) : fallback;
}

/// 读取浮点字段。允许整数与浮点两种写法。
double jsonDouble(const QJsonObject& obj, const char* key, double fallback)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : fallback;
}

/// 读取嵌套对象；非对象时返回空对象，避免调用方反复判空。
QJsonObject jsonObject(const QJsonObject& obj, const char* key)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    return v.isObject() ? v.toObject() : QJsonObject();
}

/**
 * @brief 环境变量覆盖：存在且非空时生效。
 * @param name 环境变量名。
 * @param target 被覆盖的字段。
 */
template <typename T, typename Converter>
void envOverride(const char* name, T& target, Converter convert)
{
    const QByteArray raw = qgetenv(name);
    if (!raw.isEmpty()) {
        target = static_cast<T>(convert(raw));
    }
}

QString toQString(const QByteArray& b) { return QString::fromLocal8Bit(b); }
int toInt(const QByteArray& b) { return b.toInt(); }
double toDouble(const QByteArray& b) { return b.toDouble(); }

} // namespace

AppConfig& AppConfig::instance()
{
    static AppConfig s_instance;
    return s_instance;
}

bool AppConfig::load(const QString& configPath)
{
    // ---- 1. 内置默认值 ----
    // 默认值与重构前的硬编码保持一致，保证不配置也能按原行为运行。
    m_serverIp = QStringLiteral("192.168.1.4");
    m_selfIp = QStringLiteral("192.168.1.8");
    m_cameraDevice = QStringLiteral("/dev/video0");
    m_fingerprintPort = QStringLiteral("/dev/ttyS4");
    m_audioDevice = QStringLiteral("hw:audiocodec");
    m_doorGpioLine = 133;      // PE5，开锁继电器
    m_faceCascadePath = QStringLiteral("/root/haarcascade_frontalface_default.xml");
    m_pcmDir = QStringLiteral("/tmp/pcm");
    m_frameIntervalMs = 50;
    m_faceDetectInterval = 10;
    m_faceRecognizeIntervalMs = 2000;
    m_faceConfidenceThreshold = 90.0;
    m_fingerprintIdStart = 1;
    m_doorPassword = QStringLiteral("5788");

    // ---- 2. JSON 配置文件 ----
    m_configPath = configPath.isEmpty() ? QString::fromLatin1(kDefaultConfigPath) : configPath;

    QFile file(m_configPath);
    bool fileLoaded = false;
    if (file.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();

        if (parseError.error != QJsonParseError::NoError) {
            qWarning() << "[配置] JSON 解析失败:" << m_configPath
                       << parseError.errorString() << "，回退到默认值";
        } else if (!doc.isObject()) {
            qWarning() << "[配置] 根节点不是对象:" << m_configPath << "，回退到默认值";
        } else {
            const QJsonObject root = doc.object();

            const QJsonObject net = jsonObject(root, "network");
            m_serverIp = jsonString(net, "server_ip", m_serverIp);
            m_selfIp = jsonString(net, "self_ip", m_selfIp);

            const QJsonObject hw = jsonObject(root, "hardware");
            m_cameraDevice = jsonString(hw, "camera_device", m_cameraDevice);
            m_fingerprintPort = jsonString(hw, "fingerprint_port", m_fingerprintPort);
            m_audioDevice = jsonString(hw, "audio_device", m_audioDevice);
            m_doorGpioLine = jsonInt(hw, "door_gpio_line", m_doorGpioLine);

            const QJsonObject res = jsonObject(root, "resources");
            m_faceCascadePath = jsonString(res, "face_cascade", m_faceCascadePath);
            m_pcmDir = jsonString(res, "pcm_dir", m_pcmDir);

            const QJsonObject behavior = jsonObject(root, "behavior");
            m_frameIntervalMs = jsonInt(behavior, "frame_interval_ms", m_frameIntervalMs);
            m_faceDetectInterval = jsonInt(behavior, "face_detect_interval", m_faceDetectInterval);
            m_faceRecognizeIntervalMs =
                jsonInt(behavior, "face_recognize_interval_ms", m_faceRecognizeIntervalMs);
            m_faceConfidenceThreshold =
                jsonDouble(behavior, "face_confidence_threshold", m_faceConfidenceThreshold);
            m_fingerprintIdStart = jsonInt(behavior, "fingerprint_id_start", m_fingerprintIdStart);

            // 人脸质量门与连续命中平滑（方案 P4）。默认全部关闭，
            // 不配置时行为逐字节不变。
            const QJsonObject fq = jsonObject(behavior, "face_quality");
            m_faceMinSharpness = jsonDouble(fq, "min_sharpness", m_faceMinSharpness);
            m_faceMinBrightness = jsonDouble(fq, "min_brightness", m_faceMinBrightness);
            m_faceMaxBrightness = jsonDouble(fq, "max_brightness", m_faceMaxBrightness);
            m_faceMinContrast = jsonDouble(fq, "min_contrast", m_faceMinContrast);
            m_faceMinAreaRatio = jsonDouble(fq, "min_area_ratio", m_faceMinAreaRatio);
            m_faceRequiredStreak = jsonInt(fq, "required_streak", m_faceRequiredStreak);
            m_faceStreakWindowMs = jsonInt(fq, "streak_window_ms", m_faceStreakWindowMs);

            const QJsonObject security = jsonObject(root, "security");
            m_doorPassword = jsonString(security, "door_password", m_doorPassword);

            fileLoaded = true;
        }
    } else {
        qWarning() << "[配置] 未找到配置文件，使用内置默认值:" << m_configPath;
    }

    // ---- 3. 环境变量覆盖 ----
    applyEnvOverrides();

    qInfo().noquote() << QStringLiteral("[配置] server_ip=%1 self_ip=%2 来源=%3")
                             .arg(m_serverIp, m_selfIp, m_configPath);
    return fileLoaded;
}

void AppConfig::applyEnvOverrides()
{
    // 兼容重构前的旧环境变量名，避免现网启动脚本失效。
    const QByteArray legacyServer = qgetenv("SMARTPRO_SERVER_IP");
    if (!legacyServer.isEmpty()) {
        m_serverIp = QString::fromLocal8Bit(legacyServer);
    }
    const QByteArray legacySelf = qgetenv("SMARTPRO_SELF_IP");
    if (!legacySelf.isEmpty()) {
        m_selfIp = QString::fromLocal8Bit(legacySelf);
    }

    envOverride("ACCESSTERMINAL_NETWORK_SERVER_IP", m_serverIp, toQString);
    envOverride("ACCESSTERMINAL_NETWORK_SELF_IP", m_selfIp, toQString);
    envOverride("ACCESSTERMINAL_HARDWARE_CAMERA_DEVICE", m_cameraDevice, toQString);
    envOverride("ACCESSTERMINAL_HARDWARE_FINGERPRINT_PORT", m_fingerprintPort, toQString);
    envOverride("ACCESSTERMINAL_HARDWARE_AUDIO_DEVICE", m_audioDevice, toQString);
    envOverride("ACCESSTERMINAL_HARDWARE_DOOR_GPIO_LINE", m_doorGpioLine, toInt);
    envOverride("ACCESSTERMINAL_RESOURCES_FACE_CASCADE", m_faceCascadePath, toQString);
    envOverride("ACCESSTERMINAL_RESOURCES_PCM_DIR", m_pcmDir, toQString);
    envOverride("ACCESSTERMINAL_BEHAVIOR_FRAME_INTERVAL_MS", m_frameIntervalMs, toInt);
    envOverride("ACCESSTERMINAL_BEHAVIOR_FACE_DETECT_INTERVAL", m_faceDetectInterval, toInt);
    envOverride("ACCESSTERMINAL_BEHAVIOR_FACE_RECOGNIZE_INTERVAL_MS",
                m_faceRecognizeIntervalMs, toInt);
    envOverride("ACCESSTERMINAL_BEHAVIOR_FACE_CONFIDENCE_THRESHOLD",
                m_faceConfidenceThreshold, toDouble);
    envOverride("ACCESSTERMINAL_BEHAVIOR_FINGERPRINT_ID_START", m_fingerprintIdStart, toInt);
    envOverride("ACCESSTERMINAL_SECURITY_DOOR_PASSWORD", m_doorPassword, toQString);

    // 人脸质量门与连续命中平滑（方案 P4）。与其它项一样支持环境变量覆盖，
    // 便于现场标定而无需改文件。
    envOverride("ACCESSTERMINAL_BEHAVIOR_FACE_QUALITY_MIN_SHARPNESS",
                m_faceMinSharpness, toDouble);
    envOverride("ACCESSTERMINAL_BEHAVIOR_FACE_QUALITY_MIN_BRIGHTNESS",
                m_faceMinBrightness, toDouble);
    envOverride("ACCESSTERMINAL_BEHAVIOR_FACE_QUALITY_MAX_BRIGHTNESS",
                m_faceMaxBrightness, toDouble);
    envOverride("ACCESSTERMINAL_BEHAVIOR_FACE_QUALITY_MIN_CONTRAST",
                m_faceMinContrast, toDouble);
    envOverride("ACCESSTERMINAL_BEHAVIOR_FACE_QUALITY_MIN_AREA_RATIO",
                m_faceMinAreaRatio, toDouble);
    envOverride("ACCESSTERMINAL_BEHAVIOR_FACE_QUALITY_REQUIRED_STREAK",
                m_faceRequiredStreak, toInt);
    envOverride("ACCESSTERMINAL_BEHAVIOR_FACE_QUALITY_STREAK_WINDOW_MS",
                m_faceStreakWindowMs, toInt);
}

QString AppConfig::pcmFile(const QString& name) const
{
    // 统一由 pcmDir 拼接，避免 "/tmp/pcm/xxx.pcm" 字面量散落各处。
    return QDir(m_pcmDir).filePath(name);
}

QString AppConfig::credentialStorePath() const
{
    // 与配置文件同目录、固定文件名。配置文件未加载（m_configPath 为空）
    // 时返回空串 —— 那表示"纯内存"，不落盘。
    //
    // 特例：单元测试常用 "__nonexistent__.json" 这类不存在的相对路径来
    // 表达"用默认值"。那种路径的目录是空的，QFileInfo::absolutePath()
    // 会给出当前工作目录 —— 落盘到 CWD 是不该发生的副作用，所以对
    // **不含目录分隔符** 的路径直接返回空串（= 纯内存）。
    if (m_configPath.isEmpty()) {
        return QString();
    }
    if (!m_configPath.contains(QLatin1Char('/'))
        && !m_configPath.contains(QLatin1Char('\\'))) {
        return QString();
    }
    return QFileInfo(m_configPath).absolutePath()
           + QLatin1Char('/') + QStringLiteral("credentials.json");
}

} // namespace accessterminal
