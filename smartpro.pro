# ============================================================================
# Accessterminal_T113 — 门口机（全志 T113-S3 / Tina Linux）
#
# 说明：本文件为兼容 Qt Creator / qmake 工作流的构建脚本。
#       新增的 CMakeLists.txt 是推荐入口，两者源文件集合保持一致。
#
# 重要：所有第三方库路径均改为“可覆盖变量 + 合理默认”，
#       不再写死开发者本机的 /home/meetyoo/... 绝对路径。
#       交叉编译时可在 qmake 命令行传入，例如：
#         qmake T113_SYSROOT=/path/to/staging_dir/target \
#               OPENCV_ROOT=/path/to/_install \
#               GPIO_ROOT=/path/to/libgpiod/_install
# ============================================================================

QT += core gui network serialport concurrent
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11
CONFIG += warn_on

# 与 CMakeLists.txt 的 ACCESSTERMINAL_WARNINGS 保持一致：
# 开启 -Wall -Wextra，但不启用 -Werror。
QMAKE_CXXFLAGS += -Wall -Wextra -Wno-unused-parameter

TARGET = smartpro
TEMPLATE = app

# 使源码内 #include "core/net/protocol.h" 等以 src/ 为根
INCLUDEPATH += $$PWD/src

# ============================================================================
# 第三方库路径（全部可通过 qmake 变量覆盖）
# ============================================================================

# ---- ALSA ----
isEmpty(T113_SYSROOT) {
    # 本机（桌面）构建：直接使用系统 ALSA
    LIBS += -lasound
} else {
    INCLUDEPATH += $$T113_SYSROOT/usr/include
    LIBS += -L$$T113_SYSROOT/usr/lib -lasound
}

# ---- OpenCV ----
isEmpty(OPENCV_ROOT) {
    # 本机构建：使用系统 OpenCV
    CONFIG += link_pkgconfig
    PKGCONFIG += opencv4
} else {
    INCLUDEPATH += $$OPENCV_ROOT/include/opencv4
    LIBS += -L$$OPENCV_ROOT/lib \
        -lopencv_videoio -lopencv_video -lopencv_core -lopencv_calib3d \
        -lopencv_flann -lopencv_features2d -lopencv_highgui -lopencv_face \
        -lopencv_imgproc -lopencv_dnn -lopencv_imgcodecs -lopencv_objdetect \
        -lopencv_photo
}

# ---- libgpiod ----
isEmpty(GPIO_ROOT) {
    LIBS += -lgpiod
} else {
    INCLUDEPATH += $$GPIO_ROOT/include
    # 目标平台常只提供静态库
    LIBS += $$GPIO_ROOT/lib/libgpiod.a
}

LIBS += -lpthread

# ============================================================================
# 源文件
# ============================================================================

SOURCES += \
    src/main.cpp \
    src/config/app_config.cpp \
    src/config/resource_paths.cpp \
    src/core/net/signaling_client.cpp \
    src/core/media/audio_receiver_thread.cpp \
    src/core/media/audio_sender_thread.cpp \
    src/core/media/camera_preview_server.cpp \
    src/core/media/video_push_thread.cpp \
    src/core/vision/v4l2_camera.cpp \
    src/hal/fingerprint_hailin.cpp \
    src/hal/gpio.cpp \
    src/managers/access_controller.cpp \
    src/managers/audio_manager.cpp \
    src/managers/credential_store.cpp \
    src/managers/door_actuator.cpp \
    src/managers/face_manager.cpp \
    src/managers/face_quality.cpp \
    src/managers/fingerprint_manager.cpp \
    src/managers/password_manager.cpp \
    src/ui/widget.cpp \
    src/ui/form_call.cpp \
    src/ui/form_finger.cpp \
    src/ui/form_passwd.cpp

HEADERS += \
    src/config/app_config.h \
    src/config/resource_paths.h \
    src/core/net/protocol.h \
    src/core/net/signaling_client.h \
    src/core/media/audio_receiver_thread.h \
    src/core/media/audio_sender_thread.h \
    src/core/media/camera_preview_server.h \
    src/core/media/video_push_thread.h \
    src/core/vision/v4l2_camera.h \
    src/hal/fingerprint_frame_parser.h \
    src/hal/fingerprint_hailin.h \
    src/hal/gpio.h \
    src/managers/access_controller.h \
    src/managers/audio_manager.h \
    src/managers/credential_store.h \
    src/managers/door_actuator.h \
    src/managers/face_manager.h \
    src/managers/face_quality.h \
    src/managers/fingerprint_manager.h \
    src/managers/password_manager.h \
    src/ui/widget.h \
    src/ui/form_call.h \
    src/ui/form_finger.h \
    src/ui/form_passwd.h

FORMS += \
    src/ui/widget.ui \
    src/ui/form_call.ui \
    src/ui/form_finger.ui \
    src/ui/form_passwd.ui

RESOURCES += \
    src/resources/res.qrc

# ============================================================================
# 部署规则
# ============================================================================
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
