/*
 * fake_alsa.h — 供单元测试使用的**可编程**假 ALSA 控制接口。
 *
 * 为什么需要假实现：
 * AudioManager 的历史缺陷（XRUN 静默丢数据、多个异步任务漏等待）全都在
 * "设备出错 / 被打断 / 并发" 这些分支上，而真实 ALSA 设备**无法按需制造
 * -EPIPE**、也无法让写入变慢以便观察打断时机。因此这个测试刻意链接一份
 * 假 ALSA，用可控的失败注入把那些从未被执行过的分支真正跑一遍。
 *
 * 它测的是 AudioManager 的控制流（写循环、重试、打断、收尾），
 * 不是 ALSA 本身，也不产出可运行的播放器。
 */
#ifndef ACCESSTERMINAL_TESTS_FAKE_ALSA_H
#define ACCESSTERMINAL_TESTS_FAKE_ALSA_H

#include <cstddef>
#include <vector>

namespace fakealsa {

/// 清空所有计数器与已记录数据（每个用例开头调用）。
void reset();

/// 让后续 snd_pcm_open 返回失败（模拟打不开设备）。
void setOpenFail(bool fail);

/// 让接下来 n 次 snd_pcm_writei 返回 -EPIPE（模拟 XRUN）。
void setEpipeFirst(int n);

/// 每次成功写入前休眠若干毫秒（放慢播放，便于观察打断）。
void setWriteDelayMs(int ms);

/// snd_pcm_writei 返回 -EIO 而不是成功（模拟不可恢复错误）。
void setFatalError(bool fatal);

int openCount();
int closeCount();
int prepareCount();
int drainCount();
int dropCount();
int writeiCount();      ///< writei 被调用次数（含失败）
int framesWritten();    ///< 成功写入的总帧数

/// 最后一次协商到的硬件参数，用于校验音频格式与协议一致。
int lastRate();
int lastChannels();
int lastFormat();

/// 成功写入的数据块，按写入顺序（用于校验"有没有丢数据/重复写"）。
std::vector<std::vector<char> > chunks();

} // namespace fakealsa

#endif // ACCESSTERMINAL_TESTS_FAKE_ALSA_H
