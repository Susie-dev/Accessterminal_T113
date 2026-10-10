#
# T113-S3 (全志 Tina Linux / OpenWrt) 交叉编译工具链
#
# 说明：Tina SDK 的 staging_dir 中已包含目标平台的 Qt5、ALSA 等库。
# 由于不同开发者 SDK 路径不同，这里全部通过变量注入，不写死绝对路径。
#
# 用法：
#   cmake -S . -B build-t113 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-t113.cmake \
#         -DT113_SYSROOT=/path/to/Tina-Linux/out/t113-bingpi_m2/staging_dir/target \
#         -DOPENCV_ROOT=/path/to/opencv_arm_build/_install \
#         -DGPIO_ROOT=/path/to/libgpiod/_install
#
# 其中 T113_SYSROOT 必填；OPENCV_ROOT / GPIO_ROOT 三方库根目录。
#

# ---- 工具链前缀 ----
# Tina Linux 默认使用 musl；若你的 SDK 为 glibc，请改为 arm-openwrt-linux-gcc。
set(T113_TOOLCHAIN_PREFIX "arm-openwrt-linux-muslgnueabi" CACHE STRING "交叉编译工具链前缀")

# 允许通过环境变量提供工具链目录
if(DEFINED ENV{T113_TOOLCHAIN_DIR})
    set(_t113_toolchain_dir "$ENV{T113_TOOLCHAIN_DIR}")
else()
    set(_t113_toolchain_dir "")
endif()

find_program(T113_CXX_COMPILER
    NAMES ${T113_TOOLCHAIN_PREFIX}-g++ ${T113_TOOLCHAIN_PREFIX}-gcc
    HINTS ${_t113_toolchain_dir}/bin
    REQUIRED)
find_program(T113_C_COMPILER
    NAMES ${T113_TOOLCHAIN_PREFIX}-gcc
    HINTS ${_t113_toolchain_dir}/bin
    REQUIRED)

set(CMAKE_C_COMPILER   ${T113_C_COMPILER})
set(CMAKE_CXX_COMPILER ${T113_CXX_COMPILER})

# 交叉编译不可在目标机上运行测试程序
set(CMAKE_CROSSCOMPILING_EMULATOR "" CACHE STRING "")

# ---- 目标系统描述 ----
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ---- sysroot ----
if(NOT T113_SYSROOT)
    message(FATAL_ERROR
        "必须通过 -DT113_SYSROOT=<Tina staging_dir/target> 指定目标平台 sysroot。")
endif()
set(CMAKE_SYSROOT ${T113_SYSROOT})

# 在 sysroot 内查找头文件与库，但不要在其中查找可执行程序
set(CMAKE_FIND_ROOT_PATH ${T113_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---- 第三方库路径（跨平台构建产物，位于 sysroot 之外）----
if(OPENCV_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${OPENCV_ROOT}")
    set(OpenCV_DIR "${OPENCV_ROOT}/lib/cmake/opencv4" CACHE PATH "OpenCV CMake 配置目录")
endif()

if(GPIO_ROOT)
    set(GPIO_INCLUDE_DIR "${GPIO_ROOT}/include" CACHE PATH "libgpiod 头文件目录")
    set(GPIO_LIBRARY "${GPIO_ROOT}/lib/libgpiod.a" CACHE FILEPATH "libgpiod 库文件")
endif()

# ---- 编译/链接选项 ----
# 目标机为 ARMv7-A Cortex-A7，Tina 工具链默认已适配，这里仅补充通用优化。
set(CMAKE_CXX_FLAGS_INIT "-Wall -Wextra -Wno-unused-parameter")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-Wl,--as-needed")

# 嵌入式裁剪：减小二进制体积
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT "-Wl,--gc-sections -s")
