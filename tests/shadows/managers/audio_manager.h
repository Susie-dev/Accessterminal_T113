/**
 * @file audio_manager.h
 * @brief **测试专用**的 `managers/audio_manager.h` 影子头（门口机侧，第 35 轮）。
 *
 * 为什么连 `AudioManager` 也要做成影子头
 * -------------------------------------
 * 第一版做法是在影子 `ui/widget.h` 里 `#include "fake_audio_manager.h"`，
 * 同时 `form_passwd.cpp` 自己会 `#include "managers/audio_manager.h"`（真的）。
 * 结果编译器看到**两个** `class AudioManager`：
 *
 *     src/managers/audio_manager.h:20:7: error: redefinition of 'class AudioManager'
 *
 * 因为 `ACCESSTERMINAL_SRC_ROOT` 在 include 路径里，真头照样能被找到。
 *
 * 正确做法是让**同一个路径**只有一份定义：把替身放到影子的
 * `managers/audio_manager.h`，于是 `form_passwd.cpp` 的
 * `#include "managers/audio_manager.h"` 和任何其它包含都会命中这**一份**替身。
 * 这也正是影子头机制相对"另起一个假头文件名"的优势 —— 它替换的是**路径**，
 * 而不是要求被测代码改 include。
 *
 * 替身只记录"被要求播放了什么路径"：
 *   - 真 `playAsync()` 会 `QtConcurrent::run` 去真的开 ALSA 播放
 *     `/tmp/pcm/beep.pcm`，在单元测试里既慢又依赖声卡（正是要消灭的
 *     "在我这儿能跑"），且该文件在测试机上通常不存在。
 *   - 替身让测试能断言"按数字键确实会响一声"（提示音是产品行为，不能被
 *     顺手删掉），以及"传给播放器的路径就是 AppConfig::pcmFile() 的结果"。
 *
 * 安全阀：只有定义了 `ACC_FORM_PASSWD_TEST` 的目标才能命中，否则 `#error`。
 * 本头带 Q_OBJECT，必须被显式列进 `add_executable` 的源列表让 AUTOMOC 处理。
 */
#ifndef ACCESSTERMINAL_T113_TESTS_SHADOWS_AUDIO_MANAGER_H
#define ACCESSTERMINAL_T113_TESTS_SHADOWS_AUDIO_MANAGER_H

#ifndef ACC_FORM_PASSWD_TEST
#  error "影子 managers/audio_manager.h 只能用于测试；请定义 ACC_FORM_PASSWD_TEST"
#endif

#include <QObject>
#include <QString>
#include <QStringList>

class AudioManager : public QObject
{
    Q_OBJECT

public:
    explicit AudioManager(QObject* parent = nullptr) : QObject(parent) {}

    /// 与真 `AudioManager` 同签名。
    void playAsync(const QString& pcmFilePath)
    {
        ++playAsyncCalls;
        playedPaths.append(pcmFilePath);
        emit playbackStarted(pcmFilePath);
    }

    void stop() { ++stopCalls; }

    // ---- 测试观测点 ----
    int playAsyncCalls = 0;
    int stopCalls = 0;
    QStringList playedPaths;

    /// 测试用：清空观测值（构造被测窗体期间会有非零计数）。
    void resetForTest()
    {
        playAsyncCalls = 0;
        stopCalls = 0;
        playedPaths.clear();
    }

signals:
    void playbackStarted(const QString& file);
    void playbackFinished(const QString& file);
};

#endif // ACCESSTERMINAL_T113_TESTS_SHADOWS_AUDIO_MANAGER_H
