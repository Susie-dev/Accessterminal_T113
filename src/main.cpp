/**
 * @file main.cpp
 * @brief 门口机程序入口。
 */
#include "config/app_config.h"
#include "core/net/protocol.h"
#include "ui/widget.h"

#include <QApplication>
#include <QDebug>
#include <QSurfaceFormat>

int main(int argc, char* argv[])
{
    // T113 使用 EGLFS/linuxfb 直接渲染，配置 OpenGL ES 2.0 减少
    // 不必要的特性协商（保留重构前的设置）。
    QSurfaceFormat format;
    format.setVersion(2, 0);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);

    // 配置必须在任何模块构造之前加载完毕。
    // 支持命令行覆盖配置文件路径：./smartpro /path/to/appconfig.json
    const QString configPath = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    accessterminal::AppConfig::instance().load(configPath);
    qInfo() << "[启动] 协议版本" << accessterminal::protocol::kProtocolVersion;

    Widget widget;
    widget.show();
    return app.exec();
}
