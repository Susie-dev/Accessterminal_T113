// 引入项目内部头文件 "widget.h"。
#include "widget.h"

// 引入系统或第三方头文件 <QApplication>。
#include <QApplication>
// 引入系统或第三方头文件 <QSurfaceFormat>。
#include <QSurfaceFormat>

// 定义或声明函数 main。
int main(int argc, char *argv[])
// 进入新的代码块作用域。
{
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QApplication a(argc, argv);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    Widget w;
    // 配置OpenGL
    // 执行本行语句完成对应变量、对象或业务状态处理。
    QSurfaceFormat format;
    // 继续当前代码块的参数、表达式或结构定义。
    format.setVersion(2, 0);  // OpenGL ES 2.0
    // 执行本行语句完成对应变量、对象或业务状态处理。
    format.setProfile(QSurfaceFormat::CoreProfile);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    // 定义类 QSurfaceFormat 的成员函数 setDefaultFormat。
    QSurfaceFormat::setDefaultFormat(format);
    // 执行本行语句完成对应变量、对象或业务状态处理。
    w.show();
    // 返回当前函数的执行结果或结束函数。
    return a.exec();
// 结束当前代码块作用域。
}
