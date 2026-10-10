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
    int doorGpioLine() const { return m_doorGpioLine; }

    // ---- 资源路径 ----
    QString faceCascadePath() const { return m_faceCascadePath; }   ///< Haar 分类器
    QString pcmDir() const { return m_pcmDir; }                     ///< 提示音目录

    // ---- 语音提示文件（拼接 pcmDir，避免全文散落字面量）----
    QString pcmFile(const QString& name) const;

    // ---- 行为参数 ----
    int frameIntervalMs() const { return m_frameIntervalMs; }         ///< 画面刷新周期
    int faceDetectInterval() const { return m_faceDetectInterval; }   ///< 每 N 帧检测一次
    int faceRecognizeIntervalMs() const { return m_faceRecognizeIntervalMs; }
    double faceConfidenceThreshold() const { return m_faceConfidenceThreshold; }
    int fingerprintIdStart() const { return m_fingerprintIdStart; }

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
