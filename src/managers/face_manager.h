// 开始头文件防重复包含保护 FACE_MANAGER_H。
#ifndef FACE_MANAGER_H
// 定义预处理宏或常量 FACE_MANAGER_H。
#define FACE_MANAGER_H

// 引入系统或第三方头文件 <QObject>。
#include <QObject>
// 引入系统或第三方头文件 <QImage>：接口以 const QImage& 引用该类型。
#include <QImage>
// 引入系统或第三方头文件 <QVector>。
#include <QVector>
// 引入系统或第三方头文件 <opencv2/opencv.hpp>。
#include <opencv2/opencv.hpp>
// 引入系统或第三方头文件 <opencv2/face.hpp>。
#include <opencv2/face.hpp>

// 声明类 FaceManager。
class FaceManager : public QObject
// 进入新的代码块作用域。
{
    // 继续当前代码块的参数、表达式或结构定义。
    Q_OBJECT
// 声明类成员的 public 访问权限区域。
public:
    // 声明显式构造函数 FaceManager。
    explicit FaceManager(QObject* parent = nullptr);
    // 声明或定义析构函数 FaceManager。
    ~FaceManager();

    // 注册与识别接口
    // 定义或声明函数 addFaceSample。
    void addFaceSample(const QImage& faceImg);
    // 定义或声明函数 train。
    void train();
    // 定义或声明函数 clear。
    void clear();
    // 定义或声明函数 recognize。
    void recognize(const QImage& faceImg);

    // 定义或声明函数 sampleCount。
    int sampleCount() const;

    /**
     * @brief 注册所需的人脸样本数（训练/识别的前置门槛）。
     *
     * 界面在采集够样本后要触发 train()，因此这里必须与 widget.cpp 使用
     * **同一个**数字。重构前 widget.cpp 另有一个 kFaceSampleTarget = 3 的
     * 独立常量，与 collectTarget 各改各的就会静默错位（例如界面永远不触发
     * 训练，或样本已满仍继续采集）。改为对外暴露 collectTarget，使全工程
     * 只有一个事实来源。
     */
    int sampleTarget() const { return collectTarget; }

    /**
     * @brief 设置 LBPH 判定阈值（距离，越小越严格）。
     *
     * 对应配置项 behavior.face_confidence_threshold。重构前这里是硬编码的
     * `confidence < 90`，而配置里却提供了可调的 face_confidence_threshold，
     * 结果是**该配置项一直不生效**（改配置没有任何反应）。
     *
     * 默认值取 90.0，与重构前的硬编码常量一致，因此不配置时行为逐字节不变。
     */
    void setConfidenceThreshold(double threshold) { confidenceThreshold = threshold; }

// 声明 Qt 信号槽成员区域。
signals:
    // 定义或声明函数 faceSampled。
    void faceSampled(int current, int total);
    // 定义或声明函数 trainFinished。
    void trainFinished(bool success);
    // 定义或声明函数 recognizeResult。
    void recognizeResult(bool success, double confidence);

// 声明类成员的 private 访问权限区域。
private:
    // 当前实现只维护一个用户的人脸样本；若扩展多用户，需要把 label 和样本持久化存储。
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QVector<cv::Mat> registeredFaces;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int collectTarget = 3;
    // LBPH 距离阈值。默认 90.0 与重构前的硬编码 `confidence < 90` 完全一致；
    // 由 setConfidenceThreshold() 从配置覆盖。
    double confidenceThreshold = 90.0;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    cv::Ptr<cv::face::LBPHFaceRecognizer> faceRecognizer;
// 结束当前代码块作用域。
};

// 结束当前预处理条件块。
#endif // FACE_MANAGER_H
