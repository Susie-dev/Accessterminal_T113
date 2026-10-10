/*
 * fake_v4l2_io.h — 供单元测试使用的**可编程假 V4L2/POSIX 系统调用层**。
 *
 * 为什么需要它
 * ------------
 * `V4L2Camera` 是门口机整个视频链路（UDP 23456 通话视频、UDP 50000 远程监看、
 * 人脸检测、人脸门禁）的画面来源。它此前从未被链接进任何测试目标。
 * 它的危险分支全在**系统调用失败**上：
 *
 *     open() 失败（设备不存在 / 权限不足）
 *     ioctl(VIDIOC_S_FMT / REQBUFS / QUERYBUF / QBUF / STREAMON) 失败
 *     mmap() 失败
 *     ioctl(VIDIOC_DQBUF) 失败（拔线 / 驱动异常）
 *     JPEG 解不出来（loadFromData 返回 false）
 *
 * 现有 tools/devstubs/stub_impl.c 是**固定**桩（ioctl 恒 -1、mmap 恒
 * MAP_FAILED），所以它只能做链接检查，**走不到任何成功路径**，也无法按需
 * 制造某一环失败。
 *
 * 拦截机制：-Wl,--wrap
 * -------------------
 * `open` / `close` 在 MinGW 的 CRT 里已存在，直接定义同名符号会与 CRT 冲突；
 * `ioctl`/`mmap`/`munmap` 在 MinGW 上根本不存在。因此本夹具用
 * **`-Wl,--wrap=open`** 把 `::open` 重定向到 `__wrap_open`。
 *
 * 两个必须注意的点（探针实测过，第一版就错在这里）：
 *
 *   1. `--wrap` 生成的是 **C 符号**引用（`__wrap_open`）。若把定义写成普通
 *      C++ 函数就会被名字修饰（mangle）掉，链接期报
 *      `undefined reference to '__wrap_open'`。**所有 __wrap_* 必须
 *      extern "C"。**
 *
 *   2. **只劫持目标路径**（`/dev/video*`），其余（Qt 自己打开的字体、图标、
 *      qrc 等）一律委托给 `__real_open`。否则整个进程的文件 I/O 都被接管，
 *      测试会因无关原因失败。
 *
 * 另外两个**不**劫持的决定
 * -----------------------
 *   - `close()` 只在 fd 是本夹具发出去的假 fd 时才拦截，其余委托真实实现。
 *   - `loadFromData` 的解码能力不在本夹具范围：夹具提供的是**真实 JPEG 字节**，
 *     让解码真的成功；要测"解码失败"则提供一段非 JPEG 数据。
 *     这比伪造 QImage 解码更接近真实路径。
 */
#ifndef ACCESSTERMINAL_TESTS_FAKE_V4L2_IO_H
#define ACCESSTERMINAL_TESTS_FAKE_V4L2_IO_H

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace fakev4l2io {

/// 清空所有计数器、注入开关与记录（每个用例开头调用）。
void reset();

// ---- 失败注入（全部默认成功） ----

/// open() 失败（返回 -1，errno 置为 ENOENT）。
void setOpenFail(bool fail);

/**
 * @brief 让指定的 VIDIOC_* 请求码返回 -1。
 *
 * 用 ioctl 请求码本身作 key（如 VIDIOC_S_FMT），与生产代码的写法一一对应。
 */
void setIoctlFail(unsigned long request, bool fail);

/// 对所有 ioctl 一律失败（用于"任何一步都不许成功"的极端场景）。
void setAllIoctlFail(bool fail);

/// mmap() 失败（返回 MAP_FAILED）。
void setMmapFail(bool fail);

/// DQBUF 返回的帧数据（会被放进映射区；bytesused 置为其长度）。
void setFrameData(const QByteArray& jpeg);

/// DQBUF 失败（模拟拔线 / 驱动异常）。
void setDqbufFail(bool fail);

// ---- 调用记录 ----

int openCount();          ///< 被劫持的 open 调用次数
int closeCount();         ///< 对本夹具假 fd 的 close 调用次数
int mmapCount();          ///< mmap 调用次数
int munmapCount();        ///< munmap 调用次数

/// 某个 VIDIOC_* 请求码被调用的次数。
int ioctlCount(unsigned long request);

/// 按发生顺序记录的调用序列，形如
/// "open:/dev/video0"、"ioctl:S_FMT"、"mmap"、"munmap"、"close"。
QStringList callLog();

/// 最近一次 open 收到的设备路径。
QString lastOpenPath();

/// 最近一次被处理过的 ioctl 请求码（0 表示还没有）。
unsigned long lastIoctl();

/**
 * @brief 仍处于"已映射"状态的长度总和 —— 本夹具最重要的度量。
 *
 * 现场的故障度量就是它：**没有 munmap 的映射会一直占着地址空间**，
 * 而 `close()` **不会**自动解除映射。若 open() 在 mmap 成功之后失败
 * （QBUF / STREAMON 失败）却没 munmap，重试 open() 会覆盖
 * `v4l2_buffer` 指针，旧映射就**永久**找不回来了。
 *
 * 注意它必须按"还剩多少字节映射"来算，而不是数 munmap 调用次数：
 * 数次数分不清"映射后被解除"与"从未映射"。
 */
int mappedBytes();

/// 仍处于打开状态的假 fd 数量。
int openFdCount();

/// S_FMT 时生产代码请求的像素格式（V4L2_PIX_FMT_MJPEG 等）。
unsigned long lastPixFormat();

/// S_FMT 时生产代码请求的宽 / 高。
unsigned int lastWidth();
unsigned int lastHeight();

/// 是否观察到 VIDIOC_STREAMON（成功路径的关键一步）。
bool sawStreamOn();

} // namespace fakev4l2io

#endif // ACCESSTERMINAL_TESTS_FAKE_V4L2_IO_H
