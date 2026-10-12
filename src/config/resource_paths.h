/**
 * @file resource_paths.h
 * @brief 运行时资源的路径解析（人脸分类器等）。
 *
 * 为什么需要这个模块（第 44 轮，方案 P5）
 * --------------------------------------
 * 重构后的实现里，人脸分类器的路径在**三处**互不一致：
 *   1. `app_config.cpp` 的默认值      -> /root/haarcascade_frontalface_default.xml
 *   2. 根 `CMakeLists.txt` 的 install -> <prefix>/share/accessterminal/
 *   3. `deploy/install.sh`            -> $PREFIX/share/
 * 于是在"按文档全新部署"的机器上，**默认配置指向的文件根本不存在**，
 * `cv::CascadeClassifier::load()` 静默返回 false，人脸识别就一直是关的。
 * 更糟的是它**不报错**：界面正常、日志只有一行"分类器加载 失败"，
 * 现场很容易把它当成"摄像头坏了"或"人站得不对"。
 *
 * 本模块的做法是**不去猜哪个路径才是"唯一正确"的**（那需要改现场部署），
 * 而是接受"这三个位置都可能是对的"这个事实：给出一份**有序候选清单**，
 * 谁先存在就用谁，并在回退时明确告警。
 *
 * 保守性（重要）
 * --------------
 * 配置里显式写明的路径**永远是第一候选**。因此：
 *   - 配置指向的文件存在时，行为与重构前**逐字节一致**（首个候选即命中，
 *     不走任何回退分支）；
 *   - 只有当它不存在时才会继续找 —— 而那正是重构前会**静默失败**的情形。
 * 也就是说本模块只可能"把原本失败的情况变成成功"，不会把成功的变失败。
 */
#ifndef ACCESSTERMINAL_CONFIG_RESOURCE_PATHS_H
#define ACCESSTERMINAL_CONFIG_RESOURCE_PATHS_H

#include <QString>
#include <QStringList>

namespace accessterminal {

/// 人脸分类器文件名（Haar cascade）。
QString cascadeFileName();

/**
 * 按优先级给出人脸分类器的候选路径（**纯函数**，不访问文件系统）。
 *
 * @param configured 配置里显式写明的路径；为空表示"没配"
 * @param configPath 当前加载的配置文件路径（用于推导同目录/相对前缀）
 * @return 去重后的候选清单；第一项是 @p configured（若非空）
 *
 * 之所以做成纯函数：路径构造的**顺序与去重**是最容易出错的部分
 * （例如把 `/usr/share` 与 `/usr/local/share` 写反、或把同一个路径列两遍），
 * 而它完全可以脱离文件系统来测。真正需要磁盘的部分只在
 * resolveCascadePath() 里的一行 `isFile()`。
 */
QStringList cascadeSearchPaths(const QString& configured,
                               const QString& configPath);

/**
 * 返回第一个**实际存在**的候选路径；都不存在则返回空串。
 *
 * 调用方拿到空串时应报错并给出 cascadeSearchPaths() 的完整清单 ——
 * 只打印"加载失败"是重构前的老毛病（无从查起）。
 */
QString resolveCascadePath(const QString& configured,
                           const QString& configPath);

} // namespace accessterminal

#endif // ACCESSTERMINAL_CONFIG_RESOURCE_PATHS_H
