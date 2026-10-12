#
# Accessterminal_T113 — 门口机（全志 T113-S3 / Tina Linux）
#
# 本工程可通过两套构建系统构建：
#   - CMake（推荐，新增）：见本文件
#   - qmake（保留兼容）：src/resources/../smartpro.pro 见仓库根的 smartpro.pro
#
# 桌面本机快速编译（需本机 Qt5 + OpenCV）：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build build -j
#
# T113 交叉编译：
#   cmake -S . -B build-t113 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-t113.cmake \
#         -DT113_SYSROOT=/path/to/staging_dir/target
#   cmake --build build-t113 -j
#
cmake_minimum_required(VERSION 3.16)

project(Accessterminal_T113
    VERSION 1.0.0
    DESCRIPTION "别墅门禁可视化对讲系统 — T113 门口机"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局编译设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 使源文件内的 #include "core/net/protocol.h" 等以 src/ 为根，
# 不再依赖 qmake 的隐式当前目录。
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# ---------------------------------------------------------------------------
# 警告设置
#
# 默认开启 -Wall -Wextra，让“定义了却没用到的函数”“有符号/无符号比较”
# 这类问题在编译期就暴露。交叉编译同样受益（T113 工具链为 GCC）。
# 不启用 -Werror：不同 GCC/Qt 版本的告警集合会变，卡死构建反而无益。
# ---------------------------------------------------------------------------
option(ACCESSTERMINAL_WARNINGS "开启编译警告 (-Wall -Wextra)" ON)

if(ACCESSTERMINAL_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
    # 未使用的参数在 Qt 的虚函数重写与信号槽里很常见，噪声大于价值。
    add_compile_options(-Wno-unused-parameter)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "构建类型" FORCE)
endif()

# 嵌入式目标内存有限，默认不生成调试符号以外的东西，并支持 strip。
option(ACCESSTERMINAL_STRIP "Release 构建后 strip 可执行文件" OFF)

# ---------------------------------------------------------------------------
# 第三方依赖查找
#
# 所有路径均通过变量注入，避免把开发者本机绝对路径写死在工程里。
# 交叉编译时由 toolchain 文件设置；本机构建时用系统安装路径。
# ---------------------------------------------------------------------------
find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui Widgets Network SerialPort Concurrent)

# OpenCV：优先使用 find_package（交叉编译时由 OpenCV_DIR 指向目标平台构建产物）。
find_package(OpenCV REQUIRED COMPONENTS core imgproc objdetect videoio imgcodecs face)
message(STATUS "OpenCV 版本: ${OpenCV_VERSION}")
message(STATUS "OpenCV 头文件: ${OpenCV_INCLUDE_DIRS}")

# ALSA
find_package(ALSA REQUIRED)

# libgpiod：交叉编译环境通常只提供静态库，故允许显式指定路径。
# 通过 -DGPIO_INCLUDE_DIR=... -DGPIO_LIBRARY=... 覆盖。
set(GPIO_INCLUDE_DIR "" CACHE PATH "libgpiod 头文件目录（留空则自动查找）")
set(GPIO_LIBRARY "" CACHE FILEPATH "libgpiod 库文件（留空则自动查找）")

if(NOT GPIO_LIBRARY)
    find_library(GPIO_LIBRARY NAMES gpiod libgpiod.a)
endif()
if(NOT GPIO_INCLUDE_DIR)
    find_path(GPIO_INCLUDE_DIR NAMES gpiod.h)
endif()
if(NOT GPIO_LIBRARY OR NOT GPIO_INCLUDE_DIR)
    message(FATAL_ERROR
        "未找到 libgpiod。请通过 -DGPIO_INCLUDE_DIR=<dir> -DGPIO_LIBRARY=<path> 指定，"
        "或先为目标平台交叉编译安装 libgpiod。")
endif()
message(STATUS "libgpiod 库: ${GPIO_LIBRARY}")

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------
set(APP_SOURCES
    src/main.cpp

    src/config/app_config.cpp

    src/core/net/signaling_client.cpp
    src/core/media/audio_receiver_thread.cpp
    src/core/media/audio_sender_thread.cpp
    src/core/media/camera_preview_server.cpp
    src/core/media/video_push_thread.cpp
    src/core/vision/v4l2_camera.cpp

    src/hal/fingerprint_hailin.cpp
    src/hal/gpio.cpp

    src/managers/access_controller.cpp
    src/managers/audio_manager.cpp
    src/managers/credential_store.cpp
    src/managers/door_actuator.cpp
    src/managers/face_manager.cpp
    src/managers/face_quality.cpp
    src/managers/fingerprint_manager.cpp
    src/managers/password_manager.cpp

    src/ui/widget.cpp
    src/ui/form_call.cpp
    src/ui/form_finger.cpp
    src/ui/form_passwd.cpp
)

set(APP_HEADERS
    src/config/app_config.h

    src/core/net/protocol.h
    src/core/net/signaling_client.h
    src/core/media/audio_receiver_thread.h
    src/core/media/audio_sender_thread.h
    src/core/media/camera_preview_server.h
    src/core/media/video_push_thread.h
    src/core/vision/v4l2_camera.h

    src/hal/fingerprint_frame_parser.h
    src/hal/fingerprint_hailin.h
    src/hal/gpio.h

    src/managers/access_controller.h
    src/managers/audio_manager.h
    src/managers/credential_store.h
    src/managers/door_actuator.h
    src/managers/face_manager.h
    src/managers/fingerprint_manager.h
    src/managers/password_manager.h

    src/ui/widget.h
    src/ui/form_call.h
    src/ui/form_finger.h
    src/ui/form_passwd.h
)

set(APP_FORMS
    src/ui/widget.ui
    src/ui/form_call.ui
    src/ui/form_finger.ui
    src/ui/form_passwd.ui
)

set(APP_RESOURCES
    src/resources/res.qrc
)

source_group("Sources"  FILES ${APP_SOURCES})
source_group("Headers"  FILES ${APP_HEADERS})
source_group("Forms"    FILES ${APP_FORMS})
source_group("Resources" FILES ${APP_RESOURCES})

# ---------------------------------------------------------------------------
# 目标
# ---------------------------------------------------------------------------
add_executable(smartpro
    ${APP_SOURCES}
    ${APP_HEADERS}
    ${APP_FORMS}
    ${APP_RESOURCES}
)

target_include_directories(smartpro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src          # 使 "core/net/protocol.h" 可用
    ${OpenCV_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
    ${GPIO_INCLUDE_DIR}
)

target_link_libraries(smartpro PRIVATE
    Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network Qt5::SerialPort Qt5::Concurrent
    ${OpenCV_LIBS}
    ALSA::ALSA
    ${GPIO_LIBRARY}
    pthread
)

# 嵌入式场景下 OpenCV 的 face 模块位于 opencv_face 中，若 find_package 未列出则补上。
if(NOT "${OpenCV_LIBS}" MATCHES "opencv_face")
    find_library(OPENCV_FACE_LIB NAMES opencv_face)
    if(OPENCV_FACE_LIB)
        target_link_libraries(smartpro PRIVATE ${OPENCV_FACE_LIB})
    endif()
endif()

# 默认输出名与部署脚本约定一致
set_target_properties(smartpro PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ---------------------------------------------------------------------------
# 安装规则（供 deploy/install.sh 调用 cmake --install）
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS smartpro RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES config/appconfig.json
        DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/accessterminal)
install(FILES deploy/vendor/haarcascade_frontalface_default.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/accessterminal
        OPTIONAL)
install(DIRECTORY src/resources/pcm/
        DESTINATION ${CMAKE_INSTALL_DATADIR}/accessterminal/pcm
        FILES_MATCHING PATTERN "*.pcm")

if(ACCESSTERMINAL_STRIP)
    install(CODE "execute_process(COMMAND \${CMAKE_STRIP} \$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/smartpro)")
endif()

# ---------------------------------------------------------------------------
# 单元测试（仅本机构建；交叉编译时默认关闭）
# ---------------------------------------------------------------------------
option(ACCESSTERMINAL_BUILD_TESTS "构建单元测试" OFF)
if(ACCESSTERMINAL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
