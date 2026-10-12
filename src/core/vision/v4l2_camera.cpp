#include "core/vision/v4l2_camera.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <QImage>
#include <QDebug>
#include <cstring>
#include <unistd.h>
#include <QTimer>
#include <QMutexLocker>

// 说明：这里原有一段 extern "C" 声明，引用 g_last_jpeg / g_last_jpeg_size /
// g_jpeg_mutex 三个全局量。它们来自更早的版本——当时这三个变量定义在
// widget.cpp 里，用于把最后一帧 JPEG 跨模块传递。重构后改用
// V4L2Camera::lastFrame + frameMutex 封装（见下方的 getFrame/captureLoop），
// widget.cpp 中的定义已删除，这三个名字在全工程内既无定义也无引用。
//
// 声明未使用的 extern 变量不会产生链接错误（不构成未定义引用），
// 因此编译一直是通过的，但留着会造成两点误导：
//   1. 让人以为存在一个仍需维护的全局帧缓冲；
//   2. pthread_mutex_t 强制引入 <pthread.h> 依赖，而本文件并不需要。
// 故一并移除。

V4L2Camera::V4L2Camera(const QString& devPath)
    : QObject(nullptr), devicePath(devPath)
{
    captureTimer = new QTimer(this);
    connect(captureTimer, &QTimer::timeout, this, &V4L2Camera::captureLoop);
}

V4L2Camera::~V4L2Camera() {
    close();
    if (captureTimer) {
        captureTimer->stop();
        delete captureTimer;
        captureTimer = nullptr;
    }
}

bool V4L2Camera::open() {
    v4l2_fd = ::open(devicePath.toStdString().c_str(), O_RDWR);
    if (v4l2_fd < 0) {
        qDebug() << "[V4L2Camera] 打开摄像头失败";
        return false;
    }
    // 终端摄像头直接输出 MJPEG，CPU 只负责 JPEG 解码，适合 T113 这类算力有限的平台。
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = v4l2_width;
    fmt.fmt.pix.height = v4l2_height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        qDebug() << "[V4L2Camera] 设置格式失败";
        ::close(v4l2_fd);
        v4l2_fd = -1;
        return false;
    }
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
        qDebug() << "[V4L2Camera] 请求缓冲区失败";
        ::close(v4l2_fd);
        v4l2_fd = -1;
        return false;
    }
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
        qDebug() << "[V4L2Camera] 查询缓冲区失败";
        ::close(v4l2_fd);
        v4l2_fd = -1;
        return false;
    }
    v4l2_buffer_length = buf.length;
    v4l2_buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l2_fd, buf.m.offset);
    if (v4l2_buffer == MAP_FAILED) {
        qDebug() << "[V4L2Camera] mmap失败";
        ::close(v4l2_fd);
        v4l2_fd = -1;
        return false;
    }
    if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
        qDebug() << "[V4L2Camera] 入队失败";
        // mmap 已经成功，失败退出前必须先解除映射；close() 不会
        // 自动解除映射，而重试 open() 会用新映射覆盖 v4l2_buffer，旧映射
        // 将永久无法回收。
        munmap(v4l2_buffer, v4l2_buffer_length);
        v4l2_buffer = nullptr;
        v4l2_buffer_length = 0;
        ::close(v4l2_fd);
        v4l2_fd = -1;
        return false;
    }
    int type = buf.type;
    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        qDebug() << "[V4L2Camera] 启动采集失败";
        // mmap 已经成功，失败退出前必须先解除映射；close() 不会
        munmap(v4l2_buffer, v4l2_buffer_length);
        v4l2_buffer = nullptr;
        v4l2_buffer_length = 0;
        ::close(v4l2_fd);
        v4l2_fd = -1;
        return false;
    }
    qDebug() << "[V4L2Camera] 摄像头打开成功";
    if (captureTimer) captureTimer->start(50); // 20fps
    return true;
}

void V4L2Camera::close() {
    if (captureTimer) captureTimer->stop();
    if (v4l2_fd >= 0) {
        ::close(v4l2_fd);
        v4l2_fd = -1;
    }
    if (v4l2_buffer && v4l2_buffer != MAP_FAILED) {
        munmap(v4l2_buffer, v4l2_buffer_length);
        v4l2_buffer = nullptr;
        v4l2_buffer_length = 0;
    }
}

bool V4L2Camera::isOpen() const {
    return v4l2_fd >= 0;
}

void V4L2Camera::captureLoop() {
    if (v4l2_fd < 0) return;
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
        return;
    }
    QImage img;
    // DQBUF 取出一帧后立即解码并缓存，最后 QBUF 归还缓冲区给驱动继续采集。
    bool loaded = img.loadFromData(reinterpret_cast<const uchar*>(v4l2_buffer),
                                   buf.bytesused, "JPEG");
    if (loaded) {
        QMutexLocker locker(&frameMutex);
        lastFrame = img;
        // 代际号与帧**在同一临界区内**自增，消费者才能把"代际号"当作
        // "这一帧"的可靠标识（见头文件里 frameGeneration 的说明）。
        ++lastFrameGeneration;
    }
    ioctl(v4l2_fd, VIDIOC_QBUF, &buf);
}

QImage V4L2Camera::getFrame() {
    QMutexLocker locker(&frameMutex);
    return lastFrame.copy();
}

QSize V4L2Camera::frameSize() const {
    QMutexLocker locker(&frameMutex);
    // QImage::size() 对空图返回 QSize(0,0)，因此没有帧时天然给出 (0,0)，
    // 无需额外分支 —— 与"先 getFrame() 判 isNull() 再取 size()"等价。
    return lastFrame.size();
}

quint64 V4L2Camera::frameGeneration() const {
    QMutexLocker locker(&frameMutex);
    return lastFrameGeneration;
}

int V4L2Camera::width() const { return v4l2_width; }
int V4L2Camera::height() const { return v4l2_height; }
