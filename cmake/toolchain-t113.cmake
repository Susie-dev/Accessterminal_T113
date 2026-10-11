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
#
# 三种来源，优先级从高到低：
#   1. -DT113_TOOLCHAIN_PREFIX=...
#   2. 环境变量 T113_CROSS_COMPILE（形如 .../bin/arm-openwrt-linux-，
#      SDK 自带的 environment.sh 就是这么导出的）→ 从它推导出前缀
#   3. 默认 arm-openwrt-linux-muslgnueabi
# 第 2 条是第 40 轮第一次真正交叉编译时补的：SDK 的 environment.sh 用的是
# "T113_CROSS_COMPILE + 尾随连字符"这套约定，与本文件的默认前缀不是一个
# 名字，照 SDK 说明 `source environment.sh` 后直接构建会找不到编译器。
set(_t113_prefix_from_env "")
if(DEFINED ENV{T113_CROSS_COMPILE})
    get_filename_component(_t113_cross_prefix "$ENV{T113_CROSS_COMPILE}" NAME)
    string(REGEX REPLACE "-+$" "" _t113_cross_prefix "${_t113_cross_prefix}")
    set(_t113_prefix_from_env "${_t113_cross_prefix}")
endif()
if(NOT _t113_prefix_from_env)
    set(_t113_prefix_from_env "arm-openwrt-linux-muslgnueabi")
endif()
set(T113_TOOLCHAIN_PREFIX "${_t113_prefix_from_env}"
    CACHE STRING "交叉编译工具链前缀")

# ---- 工具链目录 ----
# 允许 T113_TOOLCHAIN_DIR 指向 toolchain 根目录**或**它的 bin 目录：
# 原来的实现无条件拼 "/bin"，而 SDK 的 environment.sh 导出的
# T113_CROSS_COMPILE 的父目录恰好就是 bin 目录，会拼成 .../bin/bin 而失败。
set(_t113_toolchain_hint "")
if(DEFINED ENV{T113_TOOLCHAIN_DIR})
    set(_t113_toolchain_hint "$ENV{T113_TOOLCHAIN_DIR}")
elseif(DEFINED ENV{T113_CROSS_COMPILE})
    get_filename_component(_t113_toolchain_hint "$ENV{T113_CROSS_COMPILE}" DIRECTORY)
endif()
if(_t113_toolchain_hint MATCHES "/bin/?$")
    set(_t113_toolchain_bin "${_t113_toolchain_hint}")
elseif(_t113_toolchain_hint)
    set(_t113_toolchain_bin "${_t113_toolchain_hint}/bin")
else()
    set(_t113_toolchain_bin "")
endif()

find_program(T113_CXX_COMPILER
    NAMES ${T113_TOOLCHAIN_PREFIX}-g++ ${T113_TOOLCHAIN_PREFIX}-gcc
    HINTS ${_t113_toolchain_bin}
    REQUIRED)
find_program(T113_C_COMPILER
    NAMES ${T113_TOOLCHAIN_PREFIX}-gcc
    HINTS ${_t113_toolchain_bin}
    REQUIRED)

set(CMAKE_C_COMPILER   ${T113_C_COMPILER})
set(CMAKE_CXX_COMPILER ${T113_CXX_COMPILER})

# 交叉编译不可在目标机上运行测试程序
set(CMAKE_CROSSCOMPILING_EMULATOR "" CACHE STRING "")

# ---- 目标系统描述 ----
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# ---- sysroot ----
#
# 必须支持环境变量回退：CMake 在做**编译器 ABI 探测**时会再包含一次本文件
# （在 CMakeFiles/CMakeTmp 里新建一个嵌套项目），那个嵌套项目拿不到调用者的
# -D 缓存变量，于是 `if(NOT T113_SYSROOT)` 直接 FATAL_ERROR —— 第 40 轮第一次
# 真正交叉编译时撞到的就是这个（SDK 的 environment.sh 导出了 T113_SYSROOT）。
if(NOT T113_SYSROOT AND DEFINED ENV{T113_SYSROOT})
    set(T113_SYSROOT "$ENV{T113_SYSROOT}" CACHE PATH "目标平台 sysroot")
endif()
if(NOT T113_SYSROOT)
    message(FATAL_ERROR
        "必须通过 -DT113_SYSROOT=<Tina staging_dir/target> 或环境变量 "
        "T113_SYSROOT 指定目标平台 sysroot。")
endif()
set(CMAKE_SYSROOT ${T113_SYSROOT})

# ---- 三方库根目录：CMake 变量优先，其次同名环境变量 ----
#
# SDK 自带的 environment.sh 导出的是**环境变量**（OPENCV_ROOT / GPIOD_ROOT），
# 而本文件此前只读 CMake 变量，于是 `if(OPENCV_ROOT)` 恒假、OpenCV 找不到 ——
# 第 40 轮第一次真正交叉编译时的第三个拦路虎。另外该脚本用的是 GPIOD_ROOT
# （带 D），与这里的 GPIO_ROOT 差一个字母，两个都认。
if(NOT OPENCV_ROOT AND DEFINED ENV{OPENCV_ROOT})
    set(OPENCV_ROOT "$ENV{OPENCV_ROOT}" CACHE PATH "第三方 OpenCV 根目录")
endif()
if(NOT GPIO_ROOT)
    if(DEFINED ENV{GPIO_ROOT})
        set(GPIO_ROOT "$ENV{GPIO_ROOT}" CACHE PATH "第三方 libgpiod/ALSA 根目录")
    elseif(DEFINED ENV{GPIOD_ROOT})
        set(GPIO_ROOT "$ENV{GPIOD_ROOT}" CACHE PATH "第三方 libgpiod/ALSA 根目录")
    endif()
endif()

# 在 sysroot 内查找头文件与库，但不要在其中查找可执行程序。
# OPENCV_ROOT / GPIO_ROOT 这类三方库在 sysroot 之外，必须一并作为搜索根，
# 否则 CMAKE_FIND_ROOT_PATH_MODE_* = ONLY 会把它们排除掉。
set(CMAKE_FIND_ROOT_PATH ${T113_SYSROOT})
if(OPENCV_ROOT)
    list(APPEND CMAKE_FIND_ROOT_PATH "${OPENCV_ROOT}")
endif()
if(GPIO_ROOT)
    list(APPEND CMAKE_FIND_ROOT_PATH "${GPIO_ROOT}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---- 第三方库路径（跨平台构建产物，位于 sysroot 之外）----
if(OPENCV_ROOT)
    list(APPEND CMAKE_PREFIX_PATH "${OPENCV_ROOT}")
    # OpenCV 的 cmake 配置目录随打包方式不同（自建 install 在 lib/cmake/opencv4，
    # 手工导出的 SDK 常在 cmake/opencv4），两种都认。
    foreach(_cv_dir "${OPENCV_ROOT}/lib/cmake/opencv4"
                    "${OPENCV_ROOT}/cmake/opencv4"
                    "${OPENCV_ROOT}")
        if(EXISTS "${_cv_dir}/OpenCVConfig.cmake")
            set(OpenCV_DIR "${_cv_dir}" CACHE PATH "OpenCV CMake 配置目录")
            break()
        endif()
    endforeach()
endif()

if(GPIO_ROOT)
    set(GPIO_INCLUDE_DIR "${GPIO_ROOT}/include" CACHE PATH "libgpiod 头文件目录")
    if(EXISTS "${GPIO_ROOT}/lib/libgpiod.a")
        set(GPIO_LIBRARY "${GPIO_ROOT}/lib/libgpiod.a" CACHE FILEPATH "libgpiod 库文件")
    elseif(EXISTS "${GPIO_ROOT}/lib/libgpiod.so")
        set(GPIO_LIBRARY "${GPIO_ROOT}/lib/libgpiod.so" CACHE FILEPATH "libgpiod 库文件")
    endif()
endif()

# ALSA：/usr/include/alsa 与 asoundlib.h 常分开放（SDK 的 deps 就是如此），
# 头文件搜索根与库都要给全，否则 FindALSA 会找到头却找不到库。
if(GPIO_ROOT)
    if(NOT ALSA_INCLUDE_DIR AND EXISTS "${GPIO_ROOT}/include/asoundlib.h")
        set(ALSA_INCLUDE_DIR "${GPIO_ROOT}/include" CACHE PATH "ALSA 头文件目录")
    endif()
    if(NOT ALSA_LIBRARY AND EXISTS "${GPIO_ROOT}/lib/libasound.so")
        set(ALSA_LIBRARY "${GPIO_ROOT}/lib/libasound.so" CACHE FILEPATH "ALSA 库文件")
    endif()
endif()

# ---- Qt5 ----
#
# Tina SDK 不把 Qt 装在 sysroot 的 /usr 下，而是在 sysroot 内另起一个前缀
# （本 SDK 是 /usr/lib/arm-qt，Qt 5.12.9，CMake 配置在 lib/cmake/Qt5*）。
# 因为 CMAKE_FIND_ROOT_PATH_MODE_PACKAGE = ONLY，这个前缀必须显式给出，
# 否则 find_package(Qt5) 一定失败 —— 第 40 轮第一次交叉编译时的第二个拦路虎。
#
# 前缀来源：① -DQt5_DIR / -DCMAKE_PREFIX_PATH（用户显式指定，优先级最高）
#           ② 环境变量 T113_QMAKE_SPEC（SDK 的 environment.sh 会导出，
#              形如 <sysroot>/usr/lib/arm-qt/mkspecs/linux-t113-musl-g++）
#           ③ 常见默认 /usr/lib/arm-qt
if(NOT Qt5_DIR AND NOT T113_QT_PREFIX)
    set(_t113_qt_candidates "")
    if(DEFINED ENV{T113_QMAKE_SPEC})
        get_filename_component(_p "$ENV{T113_QMAKE_SPEC}" DIRECTORY)  # .../mkspecs
        get_filename_component(_p "${_p}" DIRECTORY)                  # .../arm-qt
        list(APPEND _t113_qt_candidates "${_p}")
    endif()
    list(APPEND _t113_qt_candidates "/usr/lib/arm-qt")

    foreach(_qt IN LISTS _t113_qt_candidates)
        if(_qt MATCHES "^${CMAKE_SYSROOT}")
            set(_qt_abs "${_qt}")
            file(RELATIVE_PATH _qt_rel "${CMAKE_SYSROOT}" "${_qt}")
            set(_qt_prefix "${_qt_rel}")
        else()
            set(_qt_abs "${CMAKE_SYSROOT}${_qt}")
            set(_qt_prefix "${_qt}")
        endif()
        if(EXISTS "${_qt_abs}/lib/cmake/Qt5/Qt5Config.cmake")
            list(APPEND CMAKE_PREFIX_PATH "${_qt_prefix}")
            set(T113_QT_PREFIX "${_qt_prefix}" CACHE STRING
                "sysroot 内的 Qt5 前缀（相对 sysroot）")
            set(Qt5_DIR "${_qt_abs}/lib/cmake/Qt5" CACHE PATH "Qt5 CMake 配置目录")
            message(STATUS "Qt5 前缀: ${_qt_prefix}（位于 sysroot 内）")
            break()
        endif()
    endforeach()
endif()

# ---- 编译/链接选项 ----
# 目标机为 ARMv7-A Cortex-A7，Tina 工具链默认已适配，这里仅补充通用优化。
# 警告标志（-Wall -Wextra -Wno-unused-parameter）统一由顶层 CMakeLists.txt
# 的 ACCESSTERMINAL_WARNINGS 控制，此处不再重复设置，避免两处维护。
#
# 链接搜索顺序必须**显式**把 sysroot 的 lib 目录放在前面，否则：
#   工具链自带一份 libc 桩（.../arm-openwrt-linux-muslgnueabi/lib/libc.so），
#   它**没有** getentropy；而板端 sysroot 的 musl（/lib/libc.so）有。GCC 的
#   默认搜索顺序会先命中工具链那份，于是链接报
#       libQt5Core.so.5.12.9: undefined reference to `getentropy'
#   第 40 轮第一次真正交叉编译就卡在这里（编译全过、只挂链接）。加 -L 让
#   sysroot 的 libc 先被选中，比 --allow-shlib-undefined 更正确：后者只是
#   把未定义符号推迟到运行时，板端 libc 万一也没有就变成启动即崩。
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-Wl,--as-needed -L${T113_SYSROOT}/lib -L${T113_SYSROOT}/usr/lib")

# 嵌入式裁剪：减小二进制体积
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT "-Wl,--gc-sections -s")
