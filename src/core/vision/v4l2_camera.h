// 逐行注释：开始头文件防重复包含保护 V4L2CAMERA_H。
#ifndef V4L2CAMERA_H
// 逐行注释：定义预处理宏或常量 V4L2CAMERA_H。
#define V4L2CAMERA_H

// 逐行注释：引入系统或第三方头文件 <QString>。
#include <QString>
// 逐行注释：引入系统或第三方头文件 <QImage>。
#include <QImage>
// 逐行注释：引入系统或第三方头文件 <QTimer>。
#include <QTimer>
// 逐行注释：引入系统或第三方头文件 <QMutex>。
#include <QMutex>

// 逐行注释：声明类 V4L2Camera。
class V4L2Camera : public QObject {
    // 逐行注释：继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
// 逐行注释：声明类成员的 public 访问权限区域。
public:
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    V4L2Camera(const QString& devPath = "/dev/video0");
    // 逐行注释：声明或定义析构函数 V4L2Camera。
    ~V4L2Camera();

    // 封装 Linux V4L2 采集流程：打开设备、设置 MJPEG 格式、mmap 缓冲区并启动流。
    // 逐行注释：定义或声明函数 open。
    bool open();
    // 逐行注释：定义或声明函数 close。
    void close();
    // 逐行注释：定义或声明函数 isOpen。
    bool isOpen() const;

    // 返回最近一次定时采集到的图像副本，避免调用方直接访问采集线程中的共享帧。
    // 逐行注释：定义或声明函数 getFrame。
    QImage getFrame();
    // 逐行注释：定义或声明函数 width。
    int width() const;
    // 逐行注释：定义或声明函数 height。
    int height() const;

// 逐行注释：声明 Qt 信号槽成员区域。
private slots:
    // 逐行注释：定义或声明函数 captureLoop。
    void captureLoop();

// 逐行注释：声明类成员的 private 访问权限区域。
private:
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    QString devicePath;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    int v4l2_fd = -1;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    void* v4l2_buffer = nullptr;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    size_t v4l2_buffer_length = 0;
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    int v4l2_width = 640, v4l2_height = 480;
    // 逐行注释：创建或处理图像帧数据。
    QImage lastFrame;
    // 逐行注释：创建或使用 Qt 定时器对象。
    QTimer* captureTimer = nullptr;
    // lastFrame 既会被定时采集更新，也会被 UI/UDP 线程读取，需要互斥保护。
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    QMutex frameMutex;
// 逐行注释：结束当前代码块作用域。
};

// 逐行注释：结束当前预处理条件块。
#endif // V4L2CAMERA_H
