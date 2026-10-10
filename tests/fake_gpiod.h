/*
 * fake_gpiod.h — 供单元测试使用的**可编程**假 libgpiod。
 *
 * 为什么需要假实现
 * ----------------
 * `GpioLine` 管的是门口机**物理门锁/补光**那条线（T113 的 PE5 = 线号 133，
 * 开锁与补光共用同一条线）。它的危险分支全在"硬件出错"上：
 *
 *   - 打不开 gpiochip（权限不足 / 设备节点不存在）；
 *   - 拿不到线（线号越界）；
 *   - 请求为输出/输入失败（线已被占用）；
 *   - 读取电平失败（value() 返回 -1）。
 *
 * 真实 libgpiod 上这些都无法按需制造，开发机也没有 gpiochip 设备。因此链接
 * 一份假 libgpiod，把那些从未被执行过的分支真正跑一遍。
 *
 * 内存所有权模型（必须与 libgpiod 1.x 一致，第一版就错在这里）
 * ------------------------------------------------------------
 * libgpiod 1.x 里**线对象由 chip 拥有**：`gpiod_chip_get_line()` 返回的是
 * `&chip->lines[offset]`（chip 内部的数组元素），不是新分配的对象；
 * `gpiod_line_release()` 只清除"已请求"状态，**不释放内存**；
 * `gpiod_chip_close()` 才释放 chip 及其全部线对象。
 *
 * 本文件第一版让 get_line 各自 new、release 各自 delete —— 那与真实 ABI 不符，
 * 于是生产代码在"请求失败"路径里先 release 再 close 时，假实现报出
 * `release:unknown`。**是夹具错了，不是生产代码错了。** 现已按上面的模型重写。
 *
 * 由此得到的核心度量是 `busyLineCount()`：**仍持有有效请求的线数**。
 * 这才是现场真正的故障度量 —— 一条被泄漏的请求会让线一直处于 busy，
 * 下一次开锁直接失败（EBUSY）。只数"对象个数"是发现不了它的。
 *
 * 并且本假实现会让**对已 busy 的线再次请求返回失败**，因此"泄漏了一条请求"
 * 能被**端到端**抓到（重试会真的失败），而不是靠事后数数。
 *
 * 与 tools/devstubs/stub_impl.c 的区别
 * ------------------------------------
 * 那个是**固定**桩，只为让主机端完成链接检查（返回值恒定、不可编程）。
 * 本文件是**可编程**假实现：能注入失败、记录调用序列与参数。二者用途不同。
 */
#ifndef ACCESSTERMINAL_TESTS_FAKE_GPIOD_H
#define ACCESSTERMINAL_TESTS_FAKE_GPIOD_H

#include <QString>
#include <QStringList>

namespace fakegpiod {

/// 清空所有计数器、注入开关与记录（每个用例开头调用）。
void reset();

// ---- 失败注入 ----

/// 让 gpiod_chip_open_by_name 返回 nullptr（打不开 chip）。
void setChipOpenFail(bool fail);

/// 让 gpiod_chip_get_line 返回 nullptr（拿不到线 / 线号越界）。
void setGetLineFail(bool fail);

/// 让 gpiod_line_request_output 返回 -1。
void setRequestOutputFail(bool fail);

/// 让 gpiod_line_request_input 返回 -1。
void setRequestInputFail(bool fail);

/// 让 gpiod_line_set_value 返回 -1（写失败）。
void setSetValueFail(bool fail);

/// value() 的返回值。设为负值即模拟读取失败。
void setReadValue(int v);

// ---- 调用记录 ----

int chipOpenCount();      ///< gpiod_chip_open_by_name 调用次数
int chipCloseCount();     ///< gpiod_chip_close 调用次数（传 nullptr 不计数）
int getLineCount();       ///< gpiod_chip_get_line 调用次数
int requestOutputCount(); ///< gpiod_line_request_output 调用次数
int requestInputCount();  ///< gpiod_line_request_input 调用次数
int releaseCount();       ///< 对**已请求**的线调用 release 的次数
int setValueCount();      ///< gpiod_line_set_value 调用次数
int getValueCount();      ///< gpiod_line_get_value 调用次数

/// 仍被打开、尚未关闭的 chip 数（open 成功 +1，close -1）。
int liveChipCount();

/**
 * @brief **仍持有有效请求**的线数 —— 本夹具最重要的度量。
 *
 * 现场故障度量就是它：一条泄漏的请求会让线一直 busy，下次开锁直接 EBUSY
 * 失败。用例结束时应为 0。
 */
int busyLineCount();

/// 对**未被请求**的线调用 release 的次数。
///
/// 记录但**不作为错误**：生产代码在"请求失败"路径里会 release 一条从未成功
/// 请求的线，而 libgpiod 对这种情况的确切行为我无法在本机验证（没有真实
/// gpiod.h / 库）。因此只观测、不断言，避免把猜测写成契约。
int releaseUnrequestedCount();

/**
 * @brief 按发生顺序记录的调用序列，形如
 *        "open:gpiochip0"、"getline:133"、"reqout:133:my_app:0"、
 *        "release"、"close"、"set:1"。
 *
 * 资源释放顺序是本测试的断言对象之一：成功释放时必须**先 release 线，
 * 再 close chip**（gpio.cpp 注释所声明的契约）。只数次数发现不了顺序颠倒。
 */
QStringList callLog();

/// 最近一次请求收到的 consumer 名（未调用过返回空）。
QString lastConsumer();

/// 最近一次请求输出收到的初始电平。
int lastInitialValue();

/// 最近一次 gpiod_line_set_value 收到的值。
int lastSetValue();

/// 最近一次 gpiod_chip_open_by_name 收到的 chip 名。
QString lastChipName();

/// 最近一次 gpiod_chip_get_line 收到的线号。
unsigned int lastLineOffset();

/// 对指定 (chip 名, 线号) 请求成功的总次数。便于断言"同一线被请求了几次"。
int requestCountFor(const QString& chipName, unsigned int offset);

} // namespace fakegpiod

#endif // ACCESSTERMINAL_TESTS_FAKE_GPIOD_H
