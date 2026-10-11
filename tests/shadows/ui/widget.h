/**
 * @file widget.h
 * @brief **测试专用**的 `ui/widget.h` 影子头（门口机侧，第 35/36 轮）。
 *
 * 做法与室内机第 33/34 轮一致：命名为真头的名字（`ui/widget.h`），用
 * `target_include_directories(... BEFORE PRIVATE ...)` 插到真
 * `src/ui/widget.h` 之前。
 *
 * 为什么必须替掉真 `ui/widget.h`
 * ------------------------------
 * 两个被测窗体会沿父链找主窗口，再调用它的成员：
 *
 *   - `src/ui/form_passwd.cpp` 的 `playKeySound()`：
 *         QWidget* w = parentWidget();
 *         while (w && !qobject_cast<class Widget*>(w)) w = w->parentWidget();
 *         if (widget) widget->audioManager()->playAsync(path);
 *
 *   - `src/ui/form_call.cpp` 的停止按钮 lambda（**stopsend 的发源地**）：
 *         QWidget* w = parentWidget();
 *         while (w && !qobject_cast<Widget*>(w)) w = w->parentWidget();
 *         if (widget) {
 *             widget->audioManager()->stop();
 *             widget->notifyPeerStopSend();      // -> TCP 8887 "stopsend\n"
 *         }
 *
 * 要真正执行这两段（而不是把测试退化成"只看源码文本"），就得有 `Widget` 的
 * 完整定义和这两个成员。而真 `ui/widget.h` 会拖进 `<opencv2/core.hpp>`、
 * `<opencv2/objdetect.hpp>`、十几个管理器前置声明和 `ui_widget.h` —— 那是给
 * 整个主程序用的门面，不是这两个窗体测试需要的。
 *
 * 为什么必须带 Q_OBJECT（第 35 轮实测，两个方向都试过）
 * ---------------------------------------------------
 * **不带** Q_OBJECT：`qobject_cast<class Widget*>(w)` 直接**编译失败**：
 *
 *     qobject.h:522:5: error: static assertion failed:
 *         qobject_cast requires the type to have a Q_OBJECT macro
 *
 * Qt 5.15 的 `qobject_cast` 对没有 Q_OBJECT 的类型是**静态断言**拒绝的，
 * 不会退回 `dynamic_cast`（实测确认）。
 *
 * **带** Q_OBJECT：必须让 AUTOMOC 真的处理本头。第一版只把它放进 include
 * 目录，链接期就报 `undefined reference to Widget::staticMetaObject` ——
 * AUTOMOC **不看 include 目录**。唯一可靠的办法是把本头文件**显式列进**
 * 每个用到它的 `add_executable` 源列表（`tests/CMakeLists.txt` 里已这么写）。
 *
 * 安全阀：只有定义了其中一个窗体测试宏的目标才能命中本影子，否则直接
 * `#error`。产线目标一旦误命中就会拿替身跑，而**编译链接一切正常** ——
 * 那是最危险的一类错误。
 */
#ifndef ACCESSTERMINAL_T113_TESTS_SHADOWS_WIDGET_H
#define ACCESSTERMINAL_T113_TESTS_SHADOWS_WIDGET_H

#if !defined(ACC_FORM_PASSWD_TEST) && !defined(ACC_FORM_CALL_TEST) \
    && !defined(ACC_FORM_FINGER_TEST)
#  error "影子 ui/widget.h 只能用于测试；请在测试目标上定义 ACC_FORM_PASSWD_TEST / ACC_FORM_CALL_TEST / ACC_FORM_FINGER_TEST"
#endif

#include <QString>
#include <QWidget>

// 注意：这里包含的是**影子** managers/audio_manager.h（靠 include 路径顺序
// 命中）。必须让被测窗体的 .cpp 与影子 widget.h 看到**同一个** AudioManager
// 定义，否则会出现 "redefinition of 'class AudioManager'" —— 见该影子头的
// 文件头说明。同理，这是必须替的第二个路径（第 35 轮的教训）。
#include "managers/audio_manager.h"

// 真 `SignalingClient`（不是影子）：`notifyPeerStopSend()` 要真的把 stopsend
// 写到 TCP 上，所以这里必须用真实实现。它的 .cpp 由测试目标直接链接。
#include "core/net/signaling_client.h"

// ---------------------------------------------------------------------------
// 指纹注册窗体（t113_test_form_finger）需要的成员。
//
// **必须宏门控**（第 37 轮实测的编译失败）：`hal/fingerprint_hailin.h` 会拉进
// `<QSerialPort>`，而只有本目标链接了 `Qt5::SerialPort`；form_passwd /
// form_call 两个目标没有链接它，无条件包含会让那两个目标编译失败。
// ---------------------------------------------------------------------------
#ifdef ACC_FORM_FINGER_TEST
#include "hal/fingerprint_hailin.h"
#include "managers/fingerprint_manager.h"
#endif

/**
 * @brief `Widget` 的替身：只保留这两个窗体测试真正会走到的成员。
 */
class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget* parent = nullptr) : QWidget(parent) {}

    /// 与真 `Widget` 同签名（真实现见 src/ui/widget.h:106）。
    AudioManager* audioManager() const { return m_audioManager; }

    /**
     * @brief 与真 `Widget::notifyPeerStopSend()` 同签名。
     *
     * 真实现（src/ui/widget.cpp:599）是：
     *
     *     void Widget::notifyPeerStopSend()
     *     {
     *         if (m_signaling) {
     *             m_signaling->send(accessterminal::protocol::Signal::StopSend);
     *         }
     *     }
     *
     * 替身做同样的事，另外把调用次数记下来，好让测试能断言"点一次只发一次"
     * （第 35 轮在 form_passwd 上发现的 `connectSlotsByName` + 显式 connect
     * 叠加会把槽跑两遍，这类"多发一次"的缺陷必须能测出来）。
     *
     * **诚实说明**：这 3 行是真实现的**副本**。真 `Widget::notifyPeerStopSend()`
     * 的函数体没有被本测试执行（`widget.cpp` 因依赖 OpenCV 与整个主程序门面
     * 而无法链接）。真实现本身由静态规则
     * `check_form_call_stopsend_is_wired` 钉住，那个规则也有故障注入自检。
     */
    void notifyPeerStopSend()
    {
        ++notifyStopSendCalls;
        if (m_signaling) {
            m_signaling->send(accessterminal::protocol::Signal::StopSend);
        }
    }

    /// 测试用：装上可观测的 `AudioManager` 替身（真 `Widget` 由构造函数创建）。
    void setAudioManagerForTest(AudioManager* mgr) { m_audioManager = mgr; }

    /// 测试用：装上真实 `SignalingClient`，让 stopsend 真的走到 TCP 上。
    void setSignalingClientForTest(accessterminal::SignalingClient* client)
    {
        m_signaling = client;
    }

#ifdef ACC_FORM_FINGER_TEST
    // ---- 指纹相关（与真 src/ui/widget.h 同签名）----
    //
    // 真实现（src/ui/widget.cpp:243 / :250-252）：
    //     m_fingerprint        = new HailinFingerprint(cfg.fingerprintPort(), this);
    //     m_fingerprintManager = new FingerprintManager(this);
    //     m_fingerprintManager->setDevice(m_fingerprint);
    //     m_nextFingerId       = cfg.fingerprintIdStart();
    //
    // 本替身不建真对象：指纹测试要自己控制"管理器先建、窗体后建"的**创建
    // 顺序**（那正是 use-after-free 的成因），所以由夹具注入。

    /// 与真 `Widget::fingerprint()` 同签名（真实现见 src/ui/widget.h:107）。
    HailinFingerprint* fingerprint() const { return m_fingerprint; }

    /// 与真 `Widget::fingerprintManager()` 同签名（真实现见 src/ui/widget.h:105）。
    FingerprintManager* fingerprintManager() const { return m_fingerprintManager; }

    /// 与真 `Widget::nextFingerId()` 同签名（真实现见 src/ui/widget.h:110）。
    int nextFingerId() const { return m_nextFingerId; }

    /// 与真 `Widget::incrementNextFingerId()` 同签名（src/ui/widget.h:111）。
    ///
    /// 真实现是 `++m_nextFingerId;`（并持久化到配置）。这里只改内存值 ——
    /// 本测试只关心"注册成功后 ID 有没有前进"，不关心落盘。
    void incrementNextFingerId() { ++m_nextFingerId; }

    // ---- 测试用注入点 ----
    void setFingerprintForTest(HailinFingerprint* dev) { m_fingerprint = dev; }
    void setFingerprintManagerForTest(FingerprintManager* mgr)
    {
        m_fingerprintManager = mgr;
    }
    void setNextFingerIdForTest(int id) { m_nextFingerId = id; }
    int nextFingerIdForTest() const { return m_nextFingerId; }

    // 夹具直接读写这几个（public 是刻意的：它们就是被观测的状态）。
    HailinFingerprint* m_fingerprint = nullptr;
    FingerprintManager* m_fingerprintManager = nullptr;
    int m_nextFingerId = 1;
#endif

    // ---- 测试观测点 ----
    int notifyStopSendCalls = 0;

private:
    AudioManager* m_audioManager = nullptr;
    accessterminal::SignalingClient* m_signaling = nullptr;
};

#endif // ACCESSTERMINAL_T113_TESTS_SHADOWS_WIDGET_H
