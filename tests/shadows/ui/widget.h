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

#if !defined(ACC_FORM_PASSWD_TEST) && !defined(ACC_FORM_CALL_TEST)
#  error "影子 ui/widget.h 只能用于测试；请在测试目标上定义 ACC_FORM_PASSWD_TEST 或 ACC_FORM_CALL_TEST"
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

    // ---- 测试观测点 ----
    int notifyStopSendCalls = 0;

private:
    AudioManager* m_audioManager = nullptr;
    accessterminal::SignalingClient* m_signaling = nullptr;
};

#endif // ACCESSTERMINAL_T113_TESTS_SHADOWS_WIDGET_H
