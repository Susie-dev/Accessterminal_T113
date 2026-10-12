/*
 * fake_v4l2_camera.h — V4L2Camera 的**链接期替身**（test double）。
 *
 * 为什么用替身而不是真实 V4L2Camera：
 *   真实 V4L2Camera 需要 /dev/video0、VIDIOC_* ioctl 与 mmap，开发机上都没有。
 *   但 CameraPushThread（UDP 23456）与 CameraUdpServer（UDP 50000）只是把
 *   `camera->getFrame()` 的返回值编码成 JPEG 发出去——两者之间是**纯数据依赖**。
 *   因此在测试里直接提供 V4L2Camera 的符号定义（不链接 src/core/vision/
 *   v4l2_camera.cpp），就能在真实网络路径上验证线上字节。
 *
 * getFrame() 不是 virtual，无法用继承覆盖，所以只能走链接替换这条路。
 * 这样做的好处是**生产代码一行都不用改**（不加测试钩子）。
 *
 * moc 仍由 AUTOMOC 对真实的 v4l2_camera.h 生成，因此 Q_OBJECT 相关的
 * 元对象/虚函数表与正式构建完全一致，测试替身只补成员函数定义。
 */
#ifndef ACCESSTERMINAL_TESTS_FAKE_V4L2_CAMERA_H
#define ACCESSTERMINAL_TESTS_FAKE_V4L2_CAMERA_H

#include <QImage>
#include <QSize>

namespace fakecam {

/// 清空所有状态：无帧、计数归零、尺寸回到默认。
void reset();

/// 设置此后 getFrame() 返回的帧。传空 QImage 表示"暂时没有帧"。
/// **每调用一次（非空帧）代际号 +1**，与真实实现"存入新帧"的语义一致。
void setFrame(const QImage& frame);

/// 便捷：生成一张确定内容的测试图（便于断言像素->JPEG 的稳定性）。
void setSolidFrame(int w, int h, unsigned int rgb);

/// 当前是否有帧。
bool hasFrame();

/// getFrame() 被调用的次数（用于验证"没有请求就不取帧"）。
/// frameSize()/frameGeneration() **不计入**此数 —— 它们不拷贝像素。
int getFrameCount();

/// 替身侧观察到的代际号（应与 camera->frameGeneration() 一致）。
quint64 generation();

/// open()/close() 的调用计数与返回码控制。
void setOpenResult(bool ok);
int openCount();
int closeCount();

} // namespace fakecam

#endif // ACCESSTERMINAL_TESTS_FAKE_V4L2_CAMERA_H
