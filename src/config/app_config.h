/**
 * @file app_config.h
 * @brief 应用配置：加载 JSON 配置文件，并支持环境变量覆盖。
 *
 * 取代原先散落在各处的硬编码常量（IP、端口、设备节点、音频资源路径）。
 *
 * 优先级（由低到高）：
 *   1. 代码内置默认值
 *   2. JSON 配置文件（默认 /etc/accessterminal/appconfig.json）
 *   3. 环境变量（便于现场调试与容器化部署，无需改文件）
 *
 * 环境变量命名规则为 ACCESSTERMINAL_<段>_<字段> 大写，例如：
 *   ACCESSTERMINAL_NETWORK_SERVER_IP / ACCESSTERMINAL_HARDWARE_FINGERPRINT_PORT
 */
#ifndef ACCESSTERMINAL_CONFIG_APP_CONFIG_H
#define ACCESSTERMINAL_CONFIG_APP_CONFIG_H

#include <QString>

namespace accessterminal {

/**
 * @brief 把配置中的整数值限制到 [lo, hi]。
 *
 * 配置来源（JSON 文件、环境变量）都是不可信输入：
 *   - 环境变量用 QString::toInt() 解析，**非法文本返回 0**（如
 *     ACCESSTERMINAL_BEHAVIOR_FACE_DETECT_INTERVAL=abc → 0）；
 *   - JSON 里也可以直接写 0 或负数。
 *
 * 这些值若被直接当作除数使用就会触发 SIGFPE（除零）导致进程崩溃。
 * 例如 widget.cpp 的 `++detectCounter % cfg.faceDetectInterval()`：
 * 重构前那里是硬编码常量 `const int detect_interval = 10;`，改写为
 * 可配置后 0 值就变得可达了。因此所有数值型配置在**取值出口**统一兜底，
 * 合法配置的行为与重构前逐字节一致。
 */
inline int clampInt(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/// 同上，用于浮点配置。
inline double clampDouble(double value, double lo, double hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/**
 * @brief 全局只读配置单例。
 *
 * 在 main() 中调用 load() 完成一次性初始化；运行期仅通过 const 访问器读取。
 */
class AppConfig
{
public:
    /// 取得全局实例。
    static AppConfig& instance();

    /**
     * @brief 加载配置。
     * @param configPath 配置文件路径；为空则使用默认路径。
     * @return 配置文件存在且解析成功返回 true；文件不存在时使用默认值并返回 false
     *         （不算致命错误，仍然可用）。
     */
    bool load(const QString& configPath = QString());

    /// 配置文件实际使用路径（便于日志与排查）。
    const QString& configPath() const { return m_configPath; }

    // ---- 网络 ----
    QString serverIp() const { return m_serverIp; }   ///< 室内机（服务端）IP
    QString selfIp() const { return m_selfIp; }       ///< 本机（门口机）IP

    // ---- 硬件设备节点 ----
    QString cameraDevice() const { return m_cameraDevice; }         ///< 如 /dev/video0
    QString fingerprintPort() const { return m_fingerprintPort; }   ///< 如 /dev/ttyS4
    QString audioDevice() const { return m_audioDevice; }           ///< ALSA 设备名
    /// 开锁继电器与补光灯共用的 GPIO 线号（硬件上为同一引脚 PE5）。
    /// 负线号会被 gpio 请求接口拒绝（错误码不直观），下限 0 使失败更明确。
    int doorGpioLine() const { return clampInt(m_doorGpioLine, 0, 1024); }

    // ---- 资源路径 ----
    QString faceCascadePath() const { return m_faceCascadePath; }   ///< Haar 分类器
    QString pcmDir() const { return m_pcmDir; }                     ///< 提示音目录

    // ---- 语音提示文件（拼接 pcmDir，避免全文散落字面量）----
    QString pcmFile(const QString& name) const;

    // ---- 行为参数 ----
    //
    // 所有数值型访问器都在出口做范围兜底：配置是不可信输入，而 frame
    // 周期 / 检测间隔 / 阈值一旦为 0 或负数，会在使用处引发除零（SIGFPE）、
    // 定时器忙转或识别永远失败。合法值（默认值、示例配置）经兜底后不变，
    // 因此与重构前行为一致。

    /// 画面刷新周期（毫秒）。下限 1ms，避免 QTimer 被 0/负值变成忙循环。
    int frameIntervalMs() const { return clampInt(m_frameIntervalMs, 1, 10000); }
    /// 每 N 帧检测一次人脸。必须 >= 1，否则取模会除零。
    int faceDetectInterval() const { return clampInt(m_faceDetectInterval, 1, 100000); }
    /// 人脸识别冷却时间（毫秒），下限 0（0 表示不冷却，属合法配置）。
    int faceRecognizeIntervalMs() const { return clampInt(m_faceRecognizeIntervalMs, 0, 3600000); }
    /// LBPH 距离阈值，越小越严格；负数会让所有比对都“成功”，故下限 0。
    double faceConfidenceThreshold() const { return clampDouble(m_faceConfidenceThreshold, 0.0, 1e9); }
    /// 指纹起始 ID。协议用 2 字节承载，限定到 [0, 65535] 与线格式一致。
    int fingerprintIdStart() const { return clampInt(m_fingerprintIdStart, 0, 65535); }

    /// 管理员开锁密码（默认值沿用重构前的 "5788"）。
    QString doorPassword() const { return m_doorPassword; }

private:
    AppConfig() = default;
    AppConfig(const AppConfig&) = delete;
    AppConfig& operator=(const AppConfig&) = delete;

    /// 从环境变量覆盖单字段。
    void applyEnvOverrides();

    QString m_configPath;
    QString m_serverIp;
    QString m_selfIp;
    QString m_cameraDevice;
    QString m_fingerprintPort;
    QString m_audioDevice;
    int m_doorGpioLine = 0;
    QString m_faceCascadePath;
    QString m_pcmDir;
    int m_frameIntervalMs = 50;
    int m_faceDetectInterval = 10;
    int m_faceRecognizeIntervalMs = 2000;
    double m_faceConfidenceThreshold = 90.0;
    int m_fingerprintIdStart = 1;
    QString m_doorPassword;
};

} // namespace accessterminal

#endif // ACCESSTERMINAL_CONFIG_APP_CONFIG_H
