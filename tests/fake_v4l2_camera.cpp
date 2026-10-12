/*
 * fake_v4l2_camera.cpp — V4L2Camera 的链接期替身实现。
 *
 * 只定义测试要用到的成员函数。签名的唯一权威是 src/core/vision/v4l2_camera.h，
 * 本文件直接 include 它，因此任何签名改动都会在编译期报错，不会静默漂移。
 */
#include "fake_v4l2_camera.h"
#include "core/vision/v4l2_camera.h"

#include <QMutex>
#include <QMutexLocker>

namespace {
QMutex g_mutex;
QImage g_frame;
int g_getFrameCount = 0;
quint64 g_generation = 0;
bool g_openResult = true;
int g_openCount = 0;
int g_closeCount = 0;
} // namespace

namespace fakecam {

void reset()
{
    QMutexLocker l(&g_mutex);
    g_frame = QImage();
    g_getFrameCount = 0;
    g_generation = 0;
    g_openResult = true;
    g_openCount = 0;
    g_closeCount = 0;
}

void setFrame(const QImage& frame)
{
    QMutexLocker l(&g_mutex);
    g_frame = frame;
    // 替身也要维护代际号：setFrame() 的语义就是"来了一帧新画面"。
    ++g_generation;
}

void setSolidFrame(int w, int h, unsigned int rgb)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(QColor(static_cast<int>((rgb >> 16) & 0xFF),
                    static_cast<int>((rgb >> 8) & 0xFF),
                    static_cast<int>(rgb & 0xFF)));
    QMutexLocker l(&g_mutex);
    g_frame = img;
    ++g_generation;
}

quint64 generation()
{
    QMutexLocker l(&g_mutex);
    return g_generation;
}

bool hasFrame()
{
    QMutexLocker l(&g_mutex);
    return !g_frame.isNull();
}

int getFrameCount()
{
    QMutexLocker l(&g_mutex);
    return g_getFrameCount;
}

void setOpenResult(bool ok) { QMutexLocker l(&g_mutex); g_openResult = ok; }
int openCount() { QMutexLocker l(&g_mutex); return g_openCount; }
int closeCount() { QMutexLocker l(&g_mutex); return g_closeCount; }

} // namespace fakecam

// ---------------------------------------------------------------------------
// V4L2Camera 的成员定义（替换 src/core/vision/v4l2_camera.cpp）
// ---------------------------------------------------------------------------

V4L2Camera::V4L2Camera(const QString& devPath)
    : QObject(nullptr), devicePath(devPath)
{
}

V4L2Camera::~V4L2Camera()
{
    // 不调用 close()：计数由测试显式驱动，避免析构顺序影响断言。
}

bool V4L2Camera::open()
{
    QMutexLocker l(&g_mutex);
    ++g_openCount;
    if (g_openResult) {
        v4l2_fd = 0; // 仅表示"已打开"，不涉及真实设备
    }
    return g_openResult;
}

void V4L2Camera::close()
{
    QMutexLocker l(&g_mutex);
    ++g_closeCount;
    v4l2_fd = -1;
}

bool V4L2Camera::isOpen() const { return v4l2_fd >= 0; }

QImage V4L2Camera::getFrame()
{
    QMutexLocker l(&g_mutex);
    ++g_getFrameCount;
    return g_frame.copy();
}

QSize V4L2Camera::frameSize() const
{
    QMutexLocker l(&g_mutex);
    // 与真实实现同语义：只读尺寸，**不**计入 g_getFrameCount。
    // 这是刻意的：调用方改用 frameSize() 之后，getFrameCount() 应当下降，
    // 测试正是用这个计数证明"不再为读尺寸而深拷贝整帧"。
    return g_frame.size();
}

quint64 V4L2Camera::frameGeneration() const
{
    QMutexLocker l(&g_mutex);
    return g_generation;
}

int V4L2Camera::width() const { return v4l2_width; }
int V4L2Camera::height() const { return v4l2_height; }

void V4L2Camera::captureLoop() {}
