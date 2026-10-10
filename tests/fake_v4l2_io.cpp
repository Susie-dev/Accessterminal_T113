/*
 * fake_v4l2_io.cpp — 可编程假 V4L2/POSIX 系统调用层的实现。
 *
 * 机制与坑见 fake_v4l2_io.h 的说明。要点复述：
 *   - 所有 __wrap_* 必须 extern "C"（--wrap 生成的是 C 符号引用）；
 *   - 只劫持 /dev/video*，其余委托 __real_open；
 *   - close() 只拦截本夹具发出的假 fd，其余委托 __real_close。
 */
#include "fake_v4l2_io.h"

#include <QMutex>
#include <QMutexLocker>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include <cerrno>
#include <cstring>

extern "C" {
int __real_open(const char* path, int flags, ...);
int __real_close(int fd);
}

namespace {

QMutex g_mutex;

bool g_openFail = false;
bool g_allIoctlFail = false;
bool g_mmapFail = false;
bool g_dqbufFail = false;
QByteArray g_frameData;

/// 被注入失败的请求码集合。用 QSet 需要 QtCore，这里直接用简单数组。
unsigned long g_failRequests[32];
int g_failRequestCount = 0;

int g_openCount = 0;
int g_closeCount = 0;
int g_mmapCount = 0;
int g_munmapCount = 0;
QStringList g_log;
QString g_lastOpenPath;
unsigned long g_lastIoctl = 0;

unsigned long g_lastPixFormat = 0;
unsigned int g_lastWidth = 0;
unsigned int g_lastHeight = 0;
bool g_sawStreamOn = false;

/// 请求码 -> 次数。数组足够大（工程只用 7 个请求码）。
struct IoctlCount { unsigned long req; int n; };
IoctlCount g_ioctlCounts[32];
int g_ioctlCountKinds = 0;

/// 假 fd 从 900 起，避开真实的 0/1/2 与普通文件描述符。
const int kFakeFdBase = 900;
int g_nextFakeFd = kFakeFdBase;
/// 当前已发出的假 fd（简化：单设备场景只需记一个集合）。
int g_fakeFds[16];
int g_fakeFdCount = 0;

/// 已映射区间：起址 -> 长度。
struct Mapping { void* addr; size_t len; };
Mapping g_mappings[16];
int g_mappingCount = 0;

bool isFakeFd(int fd)
{
    for (int i = 0; i < g_fakeFdCount; ++i) {
        if (g_fakeFds[i] == fd) {
            return true;
        }
    }
    return false;
}

int mappedBytesLocked()
{
    int total = 0;
    for (int i = 0; i < g_mappingCount; ++i) {
        total += static_cast<int>(g_mappings[i].len);
    }
    return total;
}

void bumpIoctlCount(unsigned long req)
{
    for (int i = 0; i < g_ioctlCountKinds; ++i) {
        if (g_ioctlCounts[i].req == req) {
            g_ioctlCounts[i].n++;
            return;
        }
    }
    if (g_ioctlCountKinds < 32) {
        g_ioctlCounts[g_ioctlCountKinds].req = req;
        g_ioctlCounts[g_ioctlCountKinds].n = 1;
        ++g_ioctlCountKinds;
    }
}

bool shouldFailIoctl(unsigned long req)
{
    if (g_allIoctlFail) {
        return true;
    }
    for (int i = 0; i < g_failRequestCount; ++i) {
        if (g_failRequests[i] == req) {
            return true;
        }
    }
    return false;
}

/// 请求码 -> 可读名字（日志用）。
QString ioctlName(unsigned long req)
{
    switch (req) {
    case VIDIOC_QUERYCAP:  return QStringLiteral("QUERYCAP");
    case VIDIOC_S_FMT:     return QStringLiteral("S_FMT");
    case VIDIOC_REQBUFS:   return QStringLiteral("REQBUFS");
    case VIDIOC_QUERYBUF:  return QStringLiteral("QUERYBUF");
    case VIDIOC_QBUF:      return QStringLiteral("QBUF");
    case VIDIOC_DQBUF:     return QStringLiteral("DQBUF");
    case VIDIOC_STREAMON:  return QStringLiteral("STREAMON");
    case VIDIOC_STREAMOFF: return QStringLiteral("STREAMOFF");
    default: return QStringLiteral("0x%1").arg(req, 0, 16);
    }
}

} // namespace

namespace fakev4l2io {

void reset()
{
    QMutexLocker l(&g_mutex);
    g_openFail = false;
    g_allIoctlFail = false;
    g_mmapFail = false;
    g_dqbufFail = false;
    g_frameData.clear();

    g_failRequestCount = 0;
    g_openCount = 0;
    g_closeCount = 0;
    g_mmapCount = 0;
    g_munmapCount = 0;
    g_log.clear();
    g_lastOpenPath.clear();
    g_lastIoctl = 0;
    g_lastPixFormat = 0;
    g_lastWidth = 0;
    g_lastHeight = 0;
    g_sawStreamOn = false;
    g_ioctlCountKinds = 0;
    g_fakeFdCount = 0;
    g_nextFakeFd = kFakeFdBase;
    g_mappingCount = 0;
}

void setOpenFail(bool fail) { QMutexLocker l(&g_mutex); g_openFail = fail; }
void setAllIoctlFail(bool fail) { QMutexLocker l(&g_mutex); g_allIoctlFail = fail; }
void setMmapFail(bool fail) { QMutexLocker l(&g_mutex); g_mmapFail = fail; }
void setFrameData(const QByteArray& jpeg) { QMutexLocker l(&g_mutex); g_frameData = jpeg; }
void setDqbufFail(bool fail) { QMutexLocker l(&g_mutex); g_dqbufFail = fail; }

void setIoctlFail(unsigned long request, bool fail)
{
    QMutexLocker l(&g_mutex);
    if (fail) {
        for (int i = 0; i < g_failRequestCount; ++i) {
            if (g_failRequests[i] == request) {
                return;     // 已在集合里
            }
        }
        if (g_failRequestCount < 32) {
            g_failRequests[g_failRequestCount++] = request;
        }
    } else {
        for (int i = 0; i < g_failRequestCount; ++i) {
            if (g_failRequests[i] == request) {
                g_failRequests[i] = g_failRequests[--g_failRequestCount];
                return;
            }
        }
    }
}

int openCount() { QMutexLocker l(&g_mutex); return g_openCount; }
int closeCount() { QMutexLocker l(&g_mutex); return g_closeCount; }
int mmapCount() { QMutexLocker l(&g_mutex); return g_mmapCount; }
int munmapCount() { QMutexLocker l(&g_mutex); return g_munmapCount; }

int ioctlCount(unsigned long request)
{
    QMutexLocker l(&g_mutex);
    for (int i = 0; i < g_ioctlCountKinds; ++i) {
        if (g_ioctlCounts[i].req == request) {
            return g_ioctlCounts[i].n;
        }
    }
    return 0;
}

QStringList callLog() { QMutexLocker l(&g_mutex); return g_log; }
QString lastOpenPath() { QMutexLocker l(&g_mutex); return g_lastOpenPath; }
unsigned long lastIoctl() { QMutexLocker l(&g_mutex); return g_lastIoctl; }
int mappedBytes() { QMutexLocker l(&g_mutex); return mappedBytesLocked(); }
int openFdCount() { QMutexLocker l(&g_mutex); return g_fakeFdCount; }
unsigned long lastPixFormat() { QMutexLocker l(&g_mutex); return g_lastPixFormat; }
unsigned int lastWidth() { QMutexLocker l(&g_mutex); return g_lastWidth; }
unsigned int lastHeight() { QMutexLocker l(&g_mutex); return g_lastHeight; }
bool sawStreamOn() { QMutexLocker l(&g_mutex); return g_sawStreamOn; }

} // namespace fakev4l2io

using namespace fakev4l2io;

/* ========================================================================
 * 被 --wrap 重定向进来的实现。全部必须 extern "C"。
 * ===================================================================== */
extern "C" {

int __wrap_open(const char* path, int flags, ...)
{
    const QString p = QString::fromLocal8Bit(path ? path : "");

    // 只劫持摄像头设备；其余（Qt 打开字体/图标/qrc 等）委托真实实现，
    // 否则整个进程的文件 I/O 都被接管，测试会因无关原因失败。
    if (!p.startsWith(QStringLiteral("/dev/video"))) {
        return __real_open(path, flags);
    }

    QMutexLocker l(&g_mutex);
    ++g_openCount;
    g_lastOpenPath = p;
    g_log << (QStringLiteral("open:") + p);

    if (g_openFail) {
        g_log << QStringLiteral("open:FAIL");
        errno = ENOENT;
        return -1;
    }
    const int fd = g_nextFakeFd++;
    if (g_fakeFdCount < 16) {
        g_fakeFds[g_fakeFdCount++] = fd;
    }
    return fd;
}

int __wrap_close(int fd)
{
    {
        QMutexLocker l(&g_mutex);
        if (!isFakeFd(fd)) {
            // 不是本夹具发的 fd -> 走真实实现。
            return __real_close(fd);
        }
        ++g_closeCount;
        g_log << QStringLiteral("close");
        for (int i = 0; i < g_fakeFdCount; ++i) {
            if (g_fakeFds[i] == fd) {
                g_fakeFds[i] = g_fakeFds[--g_fakeFdCount];
                break;
            }
        }
    }
    // 注意：**不**在这里解除映射 —— 真实的 close() 也不会自动 munmap。
    // 这正是被测代码必须自己 munmap 的原因。
    return 0;
}

void* __wrap_mmap(void* addr, size_t length, int prot, int flags, int fd,
                  long offset)
{
    QMutexLocker l(&g_mutex);
    ++g_mmapCount;
    g_log << QStringLiteral("mmap:%1").arg(static_cast<qulonglong>(length));

    if (g_mmapFail) {
        g_log << QStringLiteral("mmap:FAIL");
        errno = ENOMEM;
        return MAP_FAILED;
    }

    // 用真实分配返回一块可写内存，并记住它 —— 这样 mappedBytes() 与
    // munmap 都能真的被验证，而不是"看起来调用了"。
    void* mem = std::malloc(length > 0 ? length : 1);
    if (!mem) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    std::memset(mem, 0, length);
    if (g_mappingCount < 16) {
        g_mappings[g_mappingCount].addr = mem;
        g_mappings[g_mappingCount].len = length;
        ++g_mappingCount;
    }
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    return mem;
}

int __wrap_munmap(void* addr, size_t length)
{
    QMutexLocker l(&g_mutex);
    ++g_munmapCount;
    g_log << QStringLiteral("munmap:%1").arg(static_cast<qulonglong>(length));

    for (int i = 0; i < g_mappingCount; ++i) {
        if (g_mappings[i].addr == addr) {
            std::free(g_mappings[i].addr);
            g_mappings[i] = g_mappings[--g_mappingCount];
            return 0;
        }
    }
    // 解除一个本夹具不知道的映射 -> 记录但返回 0（真实 munmap 对无效区间
    // 返回 -1；这里宽松处理，因为"解除不存在的映射"不是本测试的考点）。
    g_log << QStringLiteral("munmap:unknown");
    (void)length;
    return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);

    QMutexLocker l(&g_mutex);
    g_lastIoctl = request;
    bumpIoctlCount(request);
    g_log << (QStringLiteral("ioctl:") + ioctlName(request));

    if (shouldFailIoctl(request)) {
        g_log << (QStringLiteral("ioctl:") + ioctlName(request) + QStringLiteral(":FAIL"));
        errno = EINVAL;
        return -1;
    }

    // 按请求码做最小但真实的"驱动行为"：把结构体填上合理的值，
    // 使被测代码能走到下一步，而不是靠运气。
    switch (request) {
    case VIDIOC_DQBUF:
        if (g_dqbufFail) {
            g_log << QStringLiteral("ioctl:DQBUF:FAIL");
            errno = EIO;
            return -1;
        }
        if (arg) {
            struct v4l2_buffer* buf = static_cast<struct v4l2_buffer*>(arg);
            buf->bytesused = static_cast<uint32_t>(g_frameData.size());
            // 帧数据由 mmap 区承载；真正的写入在测试侧通过 setFrameData +
            // 下面的 memcpy 完成（见 __wrap_ioctl 的 mmap 区首地址记录）。
            if (g_mappingCount > 0 && !g_frameData.isEmpty()) {
                const int n = qMin(static_cast<int>(g_mappingCount > 0
                                        ? g_mappings[0].len : 0),
                                   g_frameData.size());
                std::memcpy(g_mappings[0].addr, g_frameData.constData(), n);
            }
        }
        return 0;

    case VIDIOC_QUERYBUF:
        if (arg) {
            struct v4l2_buffer* buf = static_cast<struct v4l2_buffer*>(arg);
            // 生产代码随后用 buf.length 调 mmap()，所以这里必须给一个
            // 非零长度，否则 mmap(0) 会掩盖真实行为。
            buf->length = 1024 * 1024;
            buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf->memory = V4L2_MEMORY_MMAP;
        }
        return 0;

    case VIDIOC_S_FMT:
        if (arg) {
            struct v4l2_format* fmt = static_cast<struct v4l2_format*>(arg);
            g_lastPixFormat = fmt->fmt.pix.pixelformat;
            g_lastWidth = fmt->fmt.pix.width;
            g_lastHeight = fmt->fmt.pix.height;
        }
        return 0;

    case VIDIOC_REQBUFS:
    case VIDIOC_QBUF:
        return 0;

    case VIDIOC_STREAMON:
        g_sawStreamOn = true;
        return 0;

    default:
        return 0;
    }
}

} // extern "C"
