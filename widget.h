/*
 * @Author: '嵌入式王子' 'donminedm@163.com'
 * @Date: 2026-04-14 17:38:44
 * @LastEditors: '嵌入式王子' 'donminedm@163.com'
 * @LastEditTime: 2026-04-14 18:08:38
 * @FilePath: \smartpronew项目最终源码\widget.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
//开始头文件防重复包含保护 WIDGET_H。
#ifndef WIDGET_H
//定义预处理宏或常量 WIDGET_H。
#define WIDGET_H

//引入系统或第三方头文件 <QWidget>。
#include <QWidget>
//引入系统或第三方头文件 <QThread>。
#include <QThread>
//引入系统或第三方头文件 <QLabel>。
#include <QLabel>
//引入系统或第三方头文件 <QTimer>。
#include <QTimer>
//引入系统或第三方头文件 <QImage>。
#include <QImage>
//引入系统或第三方头文件 <QPixmap>。
#include <QPixmap>
//引入系统或第三方头文件 <QUdpSocket>。
#include <QUdpSocket>
//引入系统或第三方头文件 <QTcpSocket>。
#include <QTcpSocket>
//引入系统或第三方头文件 <QFuture>。
#include <QFuture>
//引入系统或第三方头文件 <atomic>。
#include <atomic>
//引入系统或第三方头文件 <fcntl.h>。
#include <fcntl.h>
//引入系统或第三方头文件 <sys/ioctl.h>。
#include <sys/ioctl.h>
//引入系统或第三方头文件 <linux/videodev2.h>。
#include <linux/videodev2.h>
//引入系统或第三方头文件 <sys/mman.h>。
#include <sys/mman.h>
//引入系统或第三方头文件 <opencv2/opencv.hpp>。
#include <opencv2/opencv.hpp>
//引入系统或第三方头文件 <opencv2/objdetect.hpp>。
#include <opencv2/objdetect.hpp>
//引入系统或第三方头文件 <opencv2/face.hpp>。
#include <opencv2/face.hpp>
//引入系统或第三方头文件 <QVector>。
#include <QVector>
//引入系统或第三方头文件 <QtConcurrent/QtConcurrent>。
#include <QtConcurrent/QtConcurrent>
//引入系统或第三方头文件 <memory>。
#include <memory>
//引入系统或第三方头文件 <gpiod.h>。
#include <gpiod.h>
//引入系统或第三方头文件 <stdio.h>。
#include <stdio.h>
//引入系统或第三方头文件 <unistd.h>。
#include <unistd.h>
//引入系统或第三方头文件 <QElapsedTimer>。
#include <QElapsedTimer>

//引入项目内部头文件 "audiosenderthread.h"。
#include "audiosenderthread.h"
//引入项目内部头文件 "audioreceiverthread.h"。
#include "audioreceiverthread.h"
//引入项目内部头文件 "camera_push_thread.h"。
#include "camera_push_thread.h"
//引入项目内部头文件 "form_call.h"。
#include "form_call.h"
//引入项目内部头文件 "HailinFingerprint.h"。
#include "HailinFingerprint.h"
//引入项目内部头文件 "form_finger.h"。
#include "form_finger.h"
//引入项目内部头文件 "form_passwd.h"。
#include "form_passwd.h"
//引入项目内部头文件 "managers/fingerprint_manager.h"。
#include "managers/fingerprint_manager.h"
//引入项目内部头文件 "managers/audio_manager.h"。
#include "managers/audio_manager.h"
//引入项目内部头文件 "managers/face_manager.h"。
#include "managers/face_manager.h"
//引入项目内部头文件 "managers/password_manager.h"。
#include "managers/password_manager.h"
//引入项目内部头文件 "camera/v4l2camera.h"。
#include "camera/v4l2camera.h"
//引入项目内部头文件 "camera/camera_udp_server.h"。
#include "camera/camera_udp_server.h"

//继续当前代码块的参数、表达式或结构定义。
extern "C" {
//引入系统或第三方头文件 <alsa/asoundlib.h>。
#include <alsa/asoundlib.h>
//结束当前代码块作用域。
}

//进入 Qt 命名空间声明区域。
QT_BEGIN_NAMESPACE
//声明或进入命名空间 Ui。
namespace Ui { class Widget; }
//结束 Qt 命名空间声明区域。
QT_END_NAMESPACE

//引入系统或第三方头文件 <QThread>。
#include <QThread>
//引入系统或第三方头文件 <atomic>。
#include <atomic>

//引入系统或第三方头文件 <gpiod.h>。
#include <gpiod.h>
//引入系统或第三方头文件 <stdio.h>。
#include <stdio.h>
//引入系统或第三方头文件 <unistd.h>。
#include <unistd.h>
//引入项目内部头文件 "managers/audio_manager.h"。
#include "managers/audio_manager.h"
//引入项目内部头文件 "managers/face_manager.h"。
#include "managers/face_manager.h"

//声明类 Widget。
class Widget;

// PE4 接人体/红外检测信号，独立线程轮询后通过信号通知 UI 线程是否有人靠近。
//声明类 GpioPe4MonitorThread。
class GpioPe4MonitorThread : public QThread {
    //继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
//声明类成员的 public 访问权限区域。
public:
    //继续当前代码块的参数、表达式或结构定义。
    GpioPe4MonitorThread(QObject* parent = nullptr)
        //继续当前代码块的参数、表达式或结构定义。
        : QThread(parent), running(true), paused(false) {}
    //定义或声明函数 stop。
    void stop() { running = false; }
    //定义或声明函数 pause。
    void pause() { paused = true; }
    //定义或声明函数 resume。
    void resume() { paused = false; }
//声明 Qt 信号槽成员区域。
signals:
    //定义或声明函数 pe4High。
    void pe4High();
    //定义或声明函数 pe4Low。
    void pe4Low();
//声明类成员的 protected 访问权限区域。
protected:
    //定义或声明函数 run。
    void run() override {
        //声明或创建 C 结构体变量。
        struct gpiod_chip *chip = gpiod_chip_open_by_name("gpiochip0");
        //根据条件判断是否执行该分支。
        if (!chip) return;
        //声明或创建 C 结构体变量。
        struct gpiod_line *line = gpiod_chip_get_line(chip, 132); // PE4
        //根据条件判断是否执行该分支。
        if (!line) { gpiod_chip_close(chip); return; }
        //调用 libgpiod 接口访问 GPIO。
        gpiod_line_request_input(line, "pe4_monitor");
        //执行本行语句完成对应变量、对象或业务状态处理。
        int highCount = 0;
        //开始循环处理满足条件的数据或状态。
        while (running) {
            //根据条件判断是否执行该分支。
            if (paused) {
                //继续当前代码块的参数、表达式或结构定义。
                msleep(100); // 暂停时短暂休眠，避免CPU占用
                //跳过本轮循环剩余逻辑并进入下一轮。
                continue;
            //结束当前代码块作用域。
            }
            //执行本行语句完成对应变量、对象或业务状态处理。
            int value = gpiod_line_get_value(line);
            //根据条件判断是否执行该分支。
            if (value == 1) {
                //执行本行语句完成对应变量、对象或业务状态处理。
                highCount++;
                //根据条件判断是否执行该分支。
                if (highCount >= 1) {
                    //发出 Qt 信号 pe4High 通知其他对象。
                    emit pe4High();
                    //执行本行语句完成对应变量、对象或业务状态处理。
                    highCount = 0;
                //结束当前代码块作用域。
                }
            //结束当前代码块作用域。
            } else {
                //执行本行语句完成对应变量、对象或业务状态处理。
                highCount = 0;
                //发出 Qt 信号 pe4Low 通知其他对象。
                emit pe4Low(); // 检测到低电平时emit pe4Low()
            //结束当前代码块作用域。
            }
            //继续当前代码块的参数、表达式或结构定义。
            msleep(400); // 约400ms轮询一次，降低抖动和CPU占用
        //结束当前代码块作用域。
        }
        //调用 libgpiod 接口访问 GPIO。
        gpiod_line_release(line);
        //调用 libgpiod 接口访问 GPIO。
        gpiod_chip_close(chip);
    //结束当前代码块作用域。
    }
//声明类成员的 private 访问权限区域。
private:
    //执行本行语句完成对应变量、对象或业务状态处理。
    std::atomic<bool> running;
    //执行本行语句完成对应变量、对象或业务状态处理。
    std::atomic<bool> paused;
//结束当前代码块作用域。
};


//声明类 FingerDetectThread。
class FingerDetectThread : public QThread {
    //继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
//声明类成员的 public 访问权限区域。
public:
    //声明显式构造函数 FingerDetectThread。
    explicit FingerDetectThread(QObject* parent = nullptr)
        //继续当前代码块的参数、表达式或结构定义。
        : QThread(parent), running(true) {}
    //定义或声明函数 stop。
    void stop() { running = false; }
//声明 Qt 信号槽成员区域。
signals:
    //定义或声明函数 detectRequest。
    void detectRequest();
//声明类成员的 protected 访问权限区域。
protected:
    //定义或声明函数 run。
    void run() override {
        //开始循环处理满足条件的数据或状态。
        while (running) {
            //发出 Qt 信号 detectRequest 通知其他对象。
            emit detectRequest();
            //执行本行语句完成对应变量、对象或业务状态处理。
            msleep(1000);
        //结束当前代码块作用域。
        }
    //结束当前代码块作用域。
    }
//声明类成员的 private 访问权限区域。
private:
    //执行本行语句完成对应变量、对象或业务状态处理。
    std::atomic<bool> running;
//结束当前代码块作用域。
};



//声明类 Widget。
class Widget : public QWidget
//进入新的代码块作用域。
{
    //继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT

//声明类成员的 public 访问权限区域。
public:
    //执行本行语句完成对应变量、对象或业务状态处理。
    Widget(QWidget *parent = nullptr);
    //声明或定义析构函数 Widget。
    ~Widget();

    // ================== 业务管理器 ==================
    //继续当前代码块的参数、表达式或结构定义。
    FingerprintManager* fingerprintManager = nullptr; // 指纹注册/识别
    //继续当前代码块的参数、表达式或结构定义。
    AudioManager* audioManager = nullptr;             // 音频播放
    //继续当前代码块的参数、表达式或结构定义。
    FaceManager* faceManager = nullptr;               // 人脸采集/识别
    //继续当前代码块的参数、表达式或结构定义。
    PasswordManager* passwordManager = nullptr;       // 密码校验/管理
    //继续当前代码块的参数、表达式或结构定义。
    AudioManager* connAudioManager = nullptr; // 专用于 conn.pcm 播放

    //继续当前代码块的参数、表达式或结构定义。
    HailinFingerprint* getFingerprint() const { return fingerprint; }
    //定义或声明函数 getNextFingerId。
    int getNextFingerId() const { return nextFingerId; }
    //定义或声明函数 incrementNextFingerId。
    void incrementNextFingerId() { ++nextFingerId; }
    //执行本行语句完成对应变量、对象或业务状态处理。
    QTcpSocket* tcpSignalSocket = nullptr;
//声明 Qt 信号槽成员区域。
private slots:
    // ================== UI按键相关槽 ==================
    //定义或声明函数 on_btncall_clicked。
    void on_btncall_clicked();
    //定义或声明函数 on_btnadd_clicked。
    void on_btnadd_clicked();
    //定义或声明函数 on_btnpsswd_clicked。
    void on_btnpsswd_clicked();


    // ================== 指纹相关槽 ==================
    //定义或声明函数 onFingerprintIdentify。
    void onFingerprintIdentify(bool success, int id, int score, const QString& msg);
    //定义或声明函数 onFingerprintEnroll。
    void onFingerprintEnroll(bool success, int id, const QString& msg);
    //定义或声明函数 onFingerprintDebug。
    void onFingerprintDebug(const QString& info);
    //定义或声明函数 onStepRegisterStatus。
    void onStepRegisterStatus(const QString& msg, int step, bool ok);
    //定义或声明函数 onStepIdentifyStatus。
    void onStepIdentifyStatus(const QString& msg, bool ok, int id, int score);
    //定义或声明函数 onAddFingerFromFingerForm。
    void onAddFingerFromFingerForm();

    // ================== 人脸相关槽 ==================
    //定义或声明函数 onAddFaceFromFingerForm。
    void onAddFaceFromFingerForm();
    //定义或声明函数 onFaceDetectResult。
    void onFaceDetectResult(const QVector<cv::Rect>& faces, int imgWidth, int imgHeight);

    //定义或声明函数 on_btnpower_clicked。
    void on_btnpower_clicked();

//声明类成员的 private 访问权限区域。
private:
    // ================== UI相关成员 ==================
    //执行本行语句完成对应变量、对象或业务状态处理。
    Ui::Widget *ui;
    //创建或使用 Qt 定时器对象。
    QTimer* timer = nullptr;
    //执行本行语句完成对应变量、对象或业务状态处理。
    Form_finger* fingerForm = nullptr;
    //执行本行语句完成对应变量、对象或业务状态处理。
    Form_passwd* passwdForm = nullptr;
    //执行本行语句完成对应变量、对象或业务状态处理。
    Form_CALL* callForm = nullptr;

    // ================== 指纹相关成员 ==================
    //执行本行语句完成对应变量、对象或业务状态处理。
    HailinFingerprint* fingerprint = nullptr;
    //执行本行语句完成对应变量、对象或业务状态处理。
    int nextFingerId = 1;

    // ================== 摄像头相关成员 ==================
    //执行本行语句完成对应变量、对象或业务状态处理。
    V4L2Camera* camera = nullptr;
    //执行本行语句完成对应变量、对象或业务状态处理。
    cv::CascadeClassifier face_cascade;
    //执行本行语句完成对应变量、对象或业务状态处理。
    QVector<cv::Rect> faces;
    //执行本行语句完成对应变量、对象或业务状态处理。
    int collectCount = 0;
    //执行本行语句完成对应变量、对象或业务状态处理。
    cv::Ptr<cv::face::LBPHFaceRecognizer> faceRecognizer;
    //定义或声明函数 loadFaceCascade。
    bool loadFaceCascade(const QString &path);
    //定义或声明函数 updateFrame。
    void updateFrame();
    //定义或声明函数 resizeEvent。
    void resizeEvent(QResizeEvent* event) override;

    // ================== 音频/网络/通话相关成员 ==================
    //执行本行语句完成对应变量、对象或业务状态处理。
    AudioSenderThread *audioThread = nullptr;
    //执行本行语句完成对应变量、对象或业务状态处理。
    AudioReceiverThread *audioRecvThread = nullptr;
    //继续当前代码块的参数、表达式或结构定义。
    int audioSocketFd = -1; // 新增：音频UDP socket
    //执行本行语句完成对应变量、对象或业务状态处理。
    CameraPushThread* cameraPushThread = nullptr;

    //定义或声明函数 onServerAccept。
    void onServerAccept();
    //定义或声明函数 onTcpReadyRead。
    void onTcpReadyRead();
    //定义或声明函数 hangupCall。
    void hangupCall();

    // ================== GPIO/硬件相关成员 ==================
    //执行本行语句完成对应变量、对象或业务状态处理。
    GpioPe4MonitorThread* pe4Thread = nullptr;
    //执行本行语句完成对应变量、对象或业务状态处理。
    FingerDetectThread* fingerDetectThread = nullptr;
    //声明或创建 C 结构体变量。
    struct gpiod_line *line5 = nullptr;

    // ================== 其它辅助成员 ==================
    //执行本行语句完成对应变量、对象或业务状态处理。
    QElapsedTimer faceRecognizeCooldown;
    //执行本行语句完成对应变量、对象或业务状态处理。
    int faceRecognizeIntervalMs = 2000;
    //执行本行语句完成对应变量、对象或业务状态处理。
    bool hasPerson = false;
    //执行本行语句完成对应变量、对象或业务状态处理。
    CameraUdpServer* cameraUdpServer = nullptr;
//结束当前代码块作用域。
};
//结束当前预处理条件块。
#endif // WIDGET_H
