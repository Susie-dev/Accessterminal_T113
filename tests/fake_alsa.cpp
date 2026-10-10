/*
 * fake_alsa.cpp — 可编程假 ALSA 的实现。
 *
 * 直接 `#include <alsa/asoundlib.h>`（指向 tools/devstubs 下的严格桩头），
 * 因此本文件里的每个函数签名都由头文件**编译期校验**，与 AudioManager 调用
 * 的真实 ALSA 接口完全一致 —— 不会因为参数类型不一致而在链接期"碰巧能过"。
 *
 * 所有行为都由 fake_alsa.h 里的 setter 控制，以便精确走到出错/打断分支。
 */
#include "fake_alsa.h"

#include <alsa/asoundlib.h>

#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <cstring>

// ---- 不透明句柄的实际定义（头文件中只是前向声明）----
struct _snd_pcm {
    int unused;
};
struct _snd_pcm_hw_params {
    int unused;
};

namespace {
QMutex g_mutex;
bool g_openFail = false;
int g_epipeFirst = 0;
int g_writeDelayMs = 0;
bool g_fatal = false;

// 采集方向（readi）
int g_readEpipeFirst = 0;
int g_readEagainFirst = 0;
int g_readFatalAfter = -1; // -1 = 永不致命
int g_readCount = 0;
int g_readFill = 0;
int g_framesRead = 0;

int g_openCount = 0;
int g_closeCount = 0;
int g_prepareCount = 0;
int g_drainCount = 0;
int g_dropCount = 0;
int g_writeiCount = 0;
int g_framesWritten = 0;

int g_lastRate = 0;
int g_lastChannels = 0;
int g_lastFormat = 0;

std::vector<std::vector<char> > g_chunks;

struct _snd_pcm g_handle;
} // namespace

namespace fakealsa {

void reset()
{
    QMutexLocker locker(&g_mutex);
    g_openFail = false;
    g_epipeFirst = 0;
    g_writeDelayMs = 0;
    g_fatal = false;
    g_readEpipeFirst = 0;
    g_readEagainFirst = 0;
    g_readFatalAfter = -1;
    g_readCount = 0;
    g_readFill = 0;
    g_framesRead = 0;
    g_openCount = g_closeCount = g_prepareCount = 0;
    g_drainCount = g_dropCount = g_writeiCount = g_framesWritten = 0;
    g_lastRate = g_lastChannels = g_lastFormat = 0;
    g_chunks.clear();
}

void setOpenFail(bool fail) { QMutexLocker l(&g_mutex); g_openFail = fail; }
void setEpipeFirst(int n) { QMutexLocker l(&g_mutex); g_epipeFirst = n; }
void setWriteDelayMs(int ms) { QMutexLocker l(&g_mutex); g_writeDelayMs = ms; }
void setFatalError(bool fatal) { QMutexLocker l(&g_mutex); g_fatal = fatal; }
void setReadEpipeFirst(int n) { QMutexLocker l(&g_mutex); g_readEpipeFirst = n; }
void setReadEagainFirst(int n) { QMutexLocker l(&g_mutex); g_readEagainFirst = n; }
void setReadFatalAfter(int n) { QMutexLocker l(&g_mutex); g_readFatalAfter = n; }
void setReadFill(int fill) { QMutexLocker l(&g_mutex); g_readFill = fill; }

int readiCount() { QMutexLocker l(&g_mutex); return g_readCount; }
int framesRead() { QMutexLocker l(&g_mutex); return g_framesRead; }

int openCount() { QMutexLocker l(&g_mutex); return g_openCount; }
int closeCount() { QMutexLocker l(&g_mutex); return g_closeCount; }
int prepareCount() { QMutexLocker l(&g_mutex); return g_prepareCount; }
int drainCount() { QMutexLocker l(&g_mutex); return g_drainCount; }
int dropCount() { QMutexLocker l(&g_mutex); return g_dropCount; }
int writeiCount() { QMutexLocker l(&g_mutex); return g_writeiCount; }
int framesWritten() { QMutexLocker l(&g_mutex); return g_framesWritten; }
int lastRate() { QMutexLocker l(&g_mutex); return g_lastRate; }
int lastChannels() { QMutexLocker l(&g_mutex); return g_lastChannels; }
int lastFormat() { QMutexLocker l(&g_mutex); return g_lastFormat; }

std::vector<std::vector<char> > chunks()
{
    QMutexLocker l(&g_mutex);
    return g_chunks;
}

} // namespace fakealsa

extern "C" {

int snd_pcm_open(snd_pcm_t** pcm, const char* name,
                 snd_pcm_stream_t stream, int mode)
{
    (void)name; (void)stream; (void)mode;
    QMutexLocker l(&g_mutex);
    if (g_openFail) {
        return -2; // -ENOENT
    }
    ++g_openCount;
    *pcm = &g_handle;
    return 0;
}

int snd_pcm_close(snd_pcm_t* pcm)
{
    (void)pcm;
    QMutexLocker l(&g_mutex);
    ++g_closeCount;
    return 0;
}

size_t snd_pcm_hw_params_sizeof(void) { return sizeof(struct _snd_pcm_hw_params); }

int snd_pcm_hw_params_any(snd_pcm_t* pcm, snd_pcm_hw_params_t* params)
{
    (void)pcm; (void)params;
    return 0;
}

int snd_pcm_hw_params_set_access(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                 snd_pcm_access_t access)
{
    (void)pcm; (void)params; (void)access;
    return 0;
}

int snd_pcm_hw_params_set_format(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                 snd_pcm_format_t val)
{
    (void)pcm; (void)params;
    QMutexLocker l(&g_mutex);
    g_lastFormat = static_cast<int>(val);
    return 0;
}

int snd_pcm_hw_params_set_channels(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                   unsigned int val)
{
    (void)pcm; (void)params;
    QMutexLocker l(&g_mutex);
    g_lastChannels = static_cast<int>(val);
    return 0;
}

int snd_pcm_hw_params_set_rate_near(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                                    unsigned int* val, int* dir)
{
    (void)pcm; (void)params; (void)dir;
    QMutexLocker l(&g_mutex);
    g_lastRate = static_cast<int>(*val);
    return 0;
}

int snd_pcm_hw_params_set_rate(snd_pcm_t* pcm, snd_pcm_hw_params_t* params,
                               unsigned int val, int dir)
{
    (void)pcm; (void)params; (void)dir;
    QMutexLocker l(&g_mutex);
    g_lastRate = static_cast<int>(val);
    return 0;
}

int snd_pcm_hw_params(snd_pcm_t* pcm, snd_pcm_hw_params_t* params)
{
    (void)pcm; (void)params;
    return 0;
}

snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t* pcm, const void* buffer,
                                 snd_pcm_uframes_t size)
{
    (void)pcm;
    int delay = 0;
    {
        QMutexLocker l(&g_mutex);
        ++g_writeiCount;
        if (g_epipeFirst > 0) {
            --g_epipeFirst;
            return -32; // -EPIPE
        }
        if (g_fatal) {
            return -5; // -EIO
        }
        delay = g_writeDelayMs;
    }
    if (delay > 0) {
        // 故意在锁外休眠：模拟写阻塞，便于另一线程插入打断。
        QThread::msleep(static_cast<unsigned long>(delay));
    }
    QMutexLocker l(&g_mutex);
    const char* bytes = static_cast<const char*>(buffer);
    g_chunks.push_back(std::vector<char>(bytes, bytes + size * 2));
    g_framesWritten += static_cast<int>(size);
    return static_cast<snd_pcm_sframes_t>(size);
}

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t* pcm, void* buffer,
                                snd_pcm_uframes_t size)
{
    (void)pcm;
    int fill = 0;
    {
        QMutexLocker l(&g_mutex);
        ++g_readCount;
        if (g_readEpipeFirst > 0) {
            --g_readEpipeFirst;
            return -32; // -EPIPE
        }
        if (g_readEagainFirst > 0) {
            --g_readEagainFirst;
            return -11; // -EAGAIN
        }
        if (g_readFatalAfter >= 0 && g_readCount > g_readFatalAfter) {
            return -5; // -EIO
        }
        fill = g_readFill++;
        g_framesRead += static_cast<int>(size);
    }
    if (buffer) {
        // 用 fill 填充，便于校验"每个包被独立发送、内容未错位"。
        unsigned char* p = static_cast<unsigned char*>(buffer);
        for (snd_pcm_uframes_t i = 0; i < size * 2; ++i) {
            p[i] = static_cast<unsigned char>(fill + (i % 7));
        }
    }
    return static_cast<snd_pcm_sframes_t>(size);
}

int snd_pcm_prepare(snd_pcm_t* pcm)
{
    (void)pcm;
    QMutexLocker l(&g_mutex);
    ++g_prepareCount;
    return 0;
}

int snd_pcm_drain(snd_pcm_t* pcm)
{
    (void)pcm;
    QMutexLocker l(&g_mutex);
    ++g_drainCount;
    return 0;
}

int snd_pcm_drop(snd_pcm_t* pcm)
{
    (void)pcm;
    QMutexLocker l(&g_mutex);
    ++g_dropCount;
    return 0;
}

const char* snd_strerror(int errnum)
{
    (void)errnum;
    return "fake-alsa-error";
}

} // extern "C"
