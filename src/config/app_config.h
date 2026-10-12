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
    /// LBPH 距离阈值，越小越严格。下限 0：距离恒 >= 0，负阈值等价于
    /// “谁都不通过”，是没有意义的配置。
    ///
    /// 上限 150 是**安全上界**，不是随手取的数（第 38 轮实测）：
    ///   - LBPH（create() 默认参数，8x8 网格）的距离在 ~256 处**饱和**，
    ///     阈值 >= 256 时任何图像 —— 包括完全陌生的脸 —— 都判“通过”；
    ///   - 同一身份（含抖动）实测 <= ~103；完全不同的图像实测 >= ~170；
    ///   - 150 落在两者之间：既挡住 `1e9` 与“900（多写一个 0）”这类
    ///     **会静默让门锁形同虚设**的误配，又给现场标定留出余量。
    /// 收紧上限相对原实现（1e9）是**行为变化**，第 38 轮按独立决定执行；
    /// 默认值 90 仍在区间内，故未配置时的行为逐字节不变。
    /// 判别边界与探针方法见 tools/verify_face_gate_rule.py。
    double faceConfidenceThreshold() const { return clampDouble(m_faceConfidenceThreshold, 0.0, 150.0); }
    /// 指纹起始 ID。协议用 2 字节承载，限定到 [0, 65535] 与线格式一致。
    int fingerprintIdStart() const { return clampInt(m_fingerprintIdStart, 0, 65535); }

    /// 管理员开锁密码（默认值沿用重构前的 "5788"）。
    ///
    /// **第 41 轮语义变更**：这个值现在只作为**首次初始化的引导值**。
    /// 运行时校验用的是 `CredentialStore` 里的加盐哈希（见 PasswordManager）。
    /// 它可以是一个 `sha256:<salt>:<iterations>:<digest>` 哈希串（直接装入），
    /// 也可以是明文（会被加盐哈希后装入，并打**显式的迁移告警**）。
    /// 首次启动之后，可以从 appconfig.json 里删掉本字段 —— 已哈希的凭据
    /// 存在 credentialStorePath() 里，删掉仍能开门。
    QString doorPassword() const { return m_doorPassword; }

    /**
     * @brief 凭据存储文件路径（加盐哈希落在这里，**不含明文**）。
     *
     * 默认取配置文件的同目录、文件名 `credentials.json`：
     * 部署路径 `/etc/accessterminal/appconfig.json` → `/etc/accessterminal/credentials.json`。
     * 配置文件路径为空（未加载）时返回空串 = **纯内存**，不落盘。
     *
     * 与 `door_password` 分开存放的理由：`appconfig.json` 是运维手工编辑的
     * 文件，而凭据文件是程序托管的密钥材料，两者生命周期与权限要求不同。
     */
    QString credentialStorePath() const;

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
