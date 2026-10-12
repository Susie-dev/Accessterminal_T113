// 逐行注释：开始头文件防重复包含保护 V4L2CAMERA_H。
#ifndef V4L2CAMERA_H
// 逐行注释：定义预处理宏或常量 V4L2CAMERA_H。
#define V4L2CAMERA_H

// 逐行注释：引入系统或第三方头文件 <QString>。
#include <QString>
// 逐行注释：引入系统或第三方头文件 <QImage>。
#include <QImage>
// 逐行注释：引入系统或第三方头文件 <QSize>（frameSize() 的返回类型）。
#include <QSize>
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

    // 已缓存那一帧的**尺寸**，没有帧时返回 QSize(0, 0)。**不复制任何像素。**
    //
    // 为什么单独开一个函数：调用方有时只想知道"帧有多大"。典型是
    // Widget::onFaceDetectResult —— 它需要把检测框从小图坐标还原到原图，
    // 而检测是在 QtConcurrent 线程里、对缩放后的小图做的，回收结果时原图
    // 可能已经换了。原先它调用 getFrame() 只为读 width()/height()，代价却是
    // 整幅深拷贝（640x480 RGB32 约 1.2 MB）。
    //
    // 语义与"先 getFrame() 再取 size()"**逐位一致**（同一把锁、同一个
    // lastFrame），只是不做那份没人用的拷贝。
    //
    // 注意别和 width()/height() 混淆：那两个返回的是**配置的采集尺寸**
    // （S_FMT 协商的结果），本函数返回的是**实际缓存帧**的尺寸。两者在正常
    // 情况下相等，但驱动吐出一张尺寸异常的 JPEG 时只有本函数反映真实情况。
    QSize frameSize() const;

    // 已发布帧的**代际号**：每成功存入一帧新图像 +1，初始为 0。
    //
    // 用途是"防旧帧"：跨线程消费实时视频时，消费者记下上次处理过的代际号，
    // 代际号没变就说明**没有新帧**，可以跳过重复的解码/编码/发送。
    // 这是 README 第 77 条"跨线程只传容量 1 的最新值，不排队"的配套机制：
    // 容量 1 解决了"不排队"，代际号解决了"不重复处理同一份数据"。
    //
    // 只有**成功解码并存入**新帧时才自增。解码失败会保留上一帧
    // （见 captureLoop），此时代际号**不变** —— 这正是"旧帧"的判据。
    quint64 frameGeneration() const;

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
    // lastFrame 既会被定时采集更新，也会被 UI/UDP 线程读取，需要互斥保护。
    // 代际号 frameGeneration 与 lastFrame **共用同一把锁**，两者必须一起读写：
    // 否则消费者可能拿到"新代际号 + 旧帧"（把旧帧当成新帧，等于没有防旧帧）。
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    quint64 lastFrameGeneration = 0;
    // 逐行注释：创建或使用 Qt 定时器对象。
    QTimer* captureTimer = nullptr;
    // lastFrame 既会被定时采集更新，也会被 UI/UDP 线程读取，需要互斥保护。
    //
    // 必须是 **mutable**：frameSize()/frameGeneration() 是 const 成员函数
    // （它们不改变对象的**逻辑**状态，只是查询），却都要加锁读 lastFrame。
    // const 成员里对非 mutable 成员取地址会编译失败。
    // 逐行注释：执行本行语句完成对应变量、对象或业务状态处理。
    mutable QMutex frameMutex;
// 逐行注释：结束当前代码块作用域。
};

// 逐行注释：结束当前预处理条件块。
#endif // V4L2CAMERA_H
