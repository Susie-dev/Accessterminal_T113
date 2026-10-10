// 逐行注释：引入项目内部头文件 "v4l2camera.h"。
#include "core/vision/v4l2_camera.h"
// 逐行注释：引入系统或第三方头文件 <fcntl.h>。
#include <fcntl.h>
// 逐行注释：引入系统或第三方头文件 <sys/ioctl.h>。
#include <sys/ioctl.h>
// 逐行注释：引入系统或第三方头文件 <linux/videodev2.h>。
#include <linux/videodev2.h>
// 逐行注释：引入系统或第三方头文件 <sys/mman.h>。
#include <sys/mman.h>
// 逐行注释：引入系统或第三方头文件 <QImage>。
#include <QImage>
// 逐行注释：引入系统或第三方头文件 <QDebug>。
#include <QDebug>
// 逐行注释：引入系统或第三方头文件 <cstring>。
#include <cstring>
// 逐行注释：引入系统或第三方头文件 <unistd.h>。
#include <unistd.h>
// 逐行注释：引入系统或第三方头文件 <QTimer>。
#include <QTimer>
// 逐行注释：引入系统或第三方头文件 <QMutexLocker>。
#include <QMutexLocker>

// 逐行注释：继续当前代码块的参数、表达式或结构定义。
extern "C" {
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    extern unsigned char* g_last_jpeg;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    extern size_t g_last_jpeg_size;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    extern pthread_mutex_t g_jpeg_mutex;
// 逐行注释：结束当前代码块作用域。
}

// 逐行注释：定义类 V4L2Camera 的成员函数 V4L2Camera。
V4L2Camera::V4L2Camera(const QString& devPath)
    // 逐行注释：继续当前代码块的参数、表达式或结构定义。
    : QObject(nullptr), devicePath(devPath)
// 逐行注释：进入新的代码块作用域。
{
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    captureTimer = new QTimer(this);
    // 逐行注释：建立 Qt 信号与槽或 lambda 回调连接。
    connect(captureTimer, &QTimer::timeout, this, &V4L2Camera::captureLoop);
// 逐行注释：结束当前代码块作用域。
}

// 逐行注释：定义类 V4L2Camera 的成员函数 ~V4L2Camera。
V4L2Camera::~V4L2Camera() {
    // 逐行注释：关闭文件描述符或 socket。
    close();
    // 逐行注释：根据条件判断是否执行该分支。
    if (captureTimer) {
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        captureTimer->stop();
        // 逐行注释：释放动态创建的对象资源。
        delete captureTimer;
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        captureTimer = nullptr;
    // 逐行注释：结束当前代码块作用域。
    }
// 逐行注释：结束当前代码块作用域。
}

// 逐行注释：定义或声明函数 V4L2Camera::open。
bool V4L2Camera::open() {
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    v4l2_fd = ::open(devicePath.toStdString().c_str(), O_RDWR);
    // 逐行注释：根据条件判断是否执行该分支。
    if (v4l2_fd < 0) {
        // 逐行注释：输出调试日志。
        qDebug() << "[V4L2Camera] 打开摄像头失败";
        // 逐行注释：返回当前函数的执行结果或结束函数。
        return false;
    // 逐行注释：结束当前代码块作用域。
    }
    // 终端摄像头直接输出 MJPEG，CPU 只负责 JPEG 解码，适合 T113 这类算力有限的平台。
    // 逐行注释：声明或创建 C 结构体变量。
    struct v4l2_format fmt;
    // 逐行注释：清空结构体或缓冲区内容。
    memset(&fmt, 0, sizeof(fmt));
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    fmt.fmt.pix.width = v4l2_width;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    fmt.fmt.pix.height = v4l2_height;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    // 逐行注释：根据条件判断是否执行该分支。
    if (ioctl(v4l2_fd, VIDIOC_S_FMT, &fmt) < 0) {
        // 逐行注释：输出调试日志。
        qDebug() << "[V4L2Camera] 设置格式失败";
        // 逐行注释：关闭文件描述符或 socket。
        ::close(v4l2_fd);
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        v4l2_fd = -1;
        // 逐行注释：返回当前函数的执行结果或结束函数。
        return false;
    // 逐行注释：结束当前代码块作用域。
    }
    // 逐行注释：声明或创建 C 结构体变量。
    struct v4l2_requestbuffers req;
    // 逐行注释：清空结构体或缓冲区内容。
    memset(&req, 0, sizeof(req));
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    req.count = 1;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    req.memory = V4L2_MEMORY_MMAP;
    // 逐行注释：根据条件判断是否执行该分支。
    if (ioctl(v4l2_fd, VIDIOC_REQBUFS, &req) < 0) {
        // 逐行注释：输出调试日志。
        qDebug() << "[V4L2Camera] 请求缓冲区失败";
        // 逐行注释：关闭文件描述符或 socket。
        ::close(v4l2_fd);
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        v4l2_fd = -1;
        // 逐行注释：返回当前函数的执行结果或结束函数。
        return false;
    // 逐行注释：结束当前代码块作用域。
    }
    // 逐行注释：声明或创建 C 结构体变量。
    struct v4l2_buffer buf;
    // 逐行注释：清空结构体或缓冲区内容。
    memset(&buf, 0, sizeof(buf));
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    buf.memory = V4L2_MEMORY_MMAP;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    buf.index = 0;
    // 逐行注释：根据条件判断是否执行该分支。
    if (ioctl(v4l2_fd, VIDIOC_QUERYBUF, &buf) < 0) {
        // 逐行注释：输出调试日志。
        qDebug() << "[V4L2Camera] 查询缓冲区失败";
        // 逐行注释：关闭文件描述符或 socket。
        ::close(v4l2_fd);
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        v4l2_fd = -1;
        // 逐行注释：返回当前函数的执行结果或结束函数。
        return false;
    // 逐行注释：结束当前代码块作用域。
    }
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    v4l2_buffer_length = buf.length;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    v4l2_buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, v4l2_fd, buf.m.offset);
    // 逐行注释：根据条件判断是否执行该分支。
    if (v4l2_buffer == MAP_FAILED) {
        // 逐行注释：输出调试日志。
        qDebug() << "[V4L2Camera] mmap失败";
        // 逐行注释：关闭文件描述符或 socket。
        ::close(v4l2_fd);
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        v4l2_fd = -1;
        // 逐行注释：返回当前函数的执行结果或结束函数。
        return false;
    // 逐行注释：结束当前代码块作用域。
    }
    // 逐行注释：根据条件判断是否执行该分支。
    if (ioctl(v4l2_fd, VIDIOC_QBUF, &buf) < 0) {
        // 逐行注释：输出调试日志。
        qDebug() << "[V4L2Camera] 入队失败";
        // 逐行注释：关闭文件描述符或 socket。
        ::close(v4l2_fd);
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        v4l2_fd = -1;
        // 逐行注释：返回当前函数的执行结果或结束函数。
        return false;
    // 逐行注释：结束当前代码块作用域。
    }
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    int type = buf.type;
    // 逐行注释：根据条件判断是否执行该分支。
    if (ioctl(v4l2_fd, VIDIOC_STREAMON, &type) < 0) {
        // 逐行注释：输出调试日志。
        qDebug() << "[V4L2Camera] 启动采集失败";
        // 逐行注释：关闭文件描述符或 socket。
        ::close(v4l2_fd);
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        v4l2_fd = -1;
        // 逐行注释：返回当前函数的执行结果或结束函数。
        return false;
    // 逐行注释：结束当前代码块作用域。
    }
    // 逐行注释：输出调试日志。
    qDebug() << "[V4L2Camera] 摄像头打开成功";
    // 逐行注释：根据条件判断是否执行该分支。
    if (captureTimer) captureTimer->start(50); // 20fps
    // 逐行注释：返回当前函数的执行结果或结束函数。
    return true;
// 逐行注释：结束当前代码块作用域。
}

// 逐行注释：定义或声明函数 V4L2Camera::close。
void V4L2Camera::close() {
    // 逐行注释：根据条件判断是否执行该分支。
    if (captureTimer) captureTimer->stop();
    // 逐行注释：根据条件判断是否执行该分支。
    if (v4l2_fd >= 0) {
        // 逐行注释：关闭文件描述符或 socket。
        ::close(v4l2_fd);
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        v4l2_fd = -1;
    // 逐行注释：结束当前代码块作用域。
    }
    // 逐行注释：根据条件判断是否执行该分支。
    if (v4l2_buffer && v4l2_buffer != MAP_FAILED) {
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        munmap(v4l2_buffer, v4l2_buffer_length);
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        v4l2_buffer = nullptr;
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        v4l2_buffer_length = 0;
    // 逐行注释：结束当前代码块作用域。
    }
// 逐行注释：结束当前代码块作用域。
}

// 逐行注释：定义或声明函数 V4L2Camera::isOpen。
bool V4L2Camera::isOpen() const {
    // 逐行注释：返回当前函数的执行结果或结束函数。
    return v4l2_fd >= 0;
// 逐行注释：结束当前代码块作用域。
}

// 逐行注释：定义或声明函数 V4L2Camera::captureLoop。
void V4L2Camera::captureLoop() {
    // 逐行注释：根据条件判断是否执行该分支。
    if (v4l2_fd < 0) return;
    // 逐行注释：声明或创建 C 结构体变量。
    struct v4l2_buffer buf;
    // 逐行注释：清空结构体或缓冲区内容。
    memset(&buf, 0, sizeof(buf));
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    buf.memory = V4L2_MEMORY_MMAP;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    buf.index = 0;
    // 逐行注释：根据条件判断是否执行该分支。
    if (ioctl(v4l2_fd, VIDIOC_DQBUF, &buf) < 0) {
        // 逐行注释：返回当前函数的执行结果或结束函数。
        return;
    // 逐行注释：结束当前代码块作用域。
    }
    // 逐行注释：创建或处理图像帧数据。
    QImage img;
    // DQBUF 取出一帧后立即解码并缓存，最后 QBUF 归还缓冲区给驱动继续采集。
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    bool loaded = img.loadFromData((const uchar*)v4l2_buffer, buf.bytesused, "JPEG");
    // 逐行注释：根据条件判断是否执行该分支。
    if (loaded) {
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        QMutexLocker locker(&frameMutex);
        // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
        lastFrame = img;
    // 逐行注释：结束当前代码块作用域。
    }
    // 逐行注释：通过 ioctl 向 Linux 设备驱动发送控制命令。
    ioctl(v4l2_fd, VIDIOC_QBUF, &buf);
// 逐行注释：结束当前代码块作用域。
}

// 逐行注释：定义或声明函数 V4L2Camera::getFrame。
QImage V4L2Camera::getFrame() {
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    QMutexLocker locker(&frameMutex);
    // 逐行注释：返回当前函数的执行结果或结束函数。
    return lastFrame.copy();
// 逐行注释：结束当前代码块作用域。
}

// 逐行注释：定义或声明函数 V4L2Camera::width。
int V4L2Camera::width() const { return v4l2_width; }
// 逐行注释：定义或声明函数 V4L2Camera::height。
int V4L2Camera::height() const { return v4l2_height; }
