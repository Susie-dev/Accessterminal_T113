// 引入项目内部头文件 "face_manager.h"。
#include "managers/face_manager.h"
// 引入系统或第三方头文件 <QDebug>。
#include <QDebug>
// 引入系统或第三方头文件 <QImage>。
#include <QImage>

// 定义类 FaceManager 的成员函数 FaceManager。
FaceManager::FaceManager(QObject* parent)
    // 继续当前代码块的参数、表达式或结构定义。
    : QObject(parent)
// 进入新的代码块作用域。
{
    // 执行本行语句完成对应变量、对象或业务状态处理。
    faceRecognizer = cv::face::LBPHFaceRecognizer::create();
// 结束当前代码块作用域。
}

// 定义类 FaceManager 的成员函数 ~FaceManager。
FaceManager::~FaceManager() {}

// 定义或声明函数 FaceManager::addFaceSample。
void FaceManager::addFaceSample(const QImage& faceImg)
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if (registeredFaces.size() >= collectTarget) return;
    // LBPH 使用灰度纹理直方图特征，注册和识别图像需保持一致的灰度预处理。
    // 创建或处理图像帧数据。
    QImage gray = faceImg.convertToFormat(QImage::Format_Grayscale8);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    // gray 是非 const 的 QImage，bits() 返回 uchar*，可直接传给 cv::Mat 的
    // void* data 形参（隐式转换），无需任何显式转型。
    cv::Mat mat(gray.height(), gray.width(), CV_8UC1,
                gray.bits(), gray.bytesPerLine());
    // 执行本行语句完成对应变量、对象或业务状态处理。
    registeredFaces.append(mat.clone());
    // 发出 Qt 信号 faceSampled 通知其他对象。
    emit faceSampled(registeredFaces.size(), collectTarget);
// 结束当前代码块作用域。
}

// 定义或声明函数 FaceManager::train。
void FaceManager::train()
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if (registeredFaces.size() < collectTarget) {
        // 发出 Qt 信号 trainFinished 通知其他对象。
        emit trainFinished(false);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 这里所有样本都标记为 label=1，表示单用户门禁；多用户时应按用户 ID 分配 label。
    // 执行本行语句完成对应变量、对象或业务状态处理。
    std::vector<int> labels(collectTarget, 1);
    // 用迭代器区间构造 std::vector：
    //   - QVector::toStdVector() 在 Qt 5.15 起被标记弃用，Qt6 中已移除；
    //   - 迭代器区间构造从 Qt4 起就可用，跨版本最稳。
    // 原写法 QVector<cv::Mat>(registeredFaces).toStdVector() 中的 QVector
    // 临时拷贝是多余的（registeredFaces 本身就是 QVector<cv::Mat>）。
    const std::vector<cv::Mat> samples(registeredFaces.begin(), registeredFaces.end());
    faceRecognizer->train(samples, labels);
    // 发出 Qt 信号 trainFinished 通知其他对象。
    emit trainFinished(true);
// 结束当前代码块作用域。
}

// 定义或声明函数 FaceManager::clear。
void FaceManager::clear()
// 进入新的代码块作用域。
{
    // 执行本行语句完成对应变量、对象或业务状态处理。
    registeredFaces.clear();
// 结束当前代码块作用域。
}

// 定义或声明函数 FaceManager::recognize。
void FaceManager::recognize(const QImage& faceImg)
// 进入新的代码块作用域。
{
    // 根据条件判断是否执行该分支。
    if (registeredFaces.size() < collectTarget) {
        // 发出 Qt 信号 recognizeResult 通知其他对象。
        emit recognizeResult(false, 0.0);
        // 返回当前函数的执行结果或结束函数。
        return;
    // 结束当前代码块作用域。
    }
    // 创建或处理图像帧数据。
    QImage gray = faceImg.convertToFormat(QImage::Format_Grayscale8);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    // 同上：gray 非 const，bits() 返回 uchar*，可直接隐式转换为 void*。
    cv::Mat testFace(gray.height(), gray.width(), CV_8UC1,
                     gray.bits(), gray.bytesPerLine());
    // 执行本行语句完成对应变量、对象或业务状态处理。
    int predictedLabel = -1;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    double confidence = 0.0;
    // 执行本行语句完成对应变量、对象或业务状态处理。
    faceRecognizer->predict(testFace, predictedLabel, confidence);
    // OpenCV LBPH 的 confidence 是距离，值越小相似度越高；默认 90 是经验阈值，
    // 需要现场光照下标定，因此通过配置 behavior.face_confidence_threshold 暴露。
    // 重构前这里硬编码 90，导致同名配置项被读取却从不生效。
    bool success = (predictedLabel == 1 && confidence < confidenceThreshold);
    // 发出 Qt 信号 recognizeResult 通知其他对象。
    emit recognizeResult(success, confidence);
// 结束当前代码块作用域。
}

// 定义或声明函数 FaceManager::sampleCount。
int FaceManager::sampleCount() const
// 进入新的代码块作用域。
{
    // 返回当前函数的执行结果或结束函数。
    return registeredFaces.size();
// 结束当前代码块作用域。
}
