#
# 单元测试。两种用法：
#
#   A. 独立构建（推荐，本机无 ALSA 时唯一可行）：
#        cmake -S tests -B /tmp/t113-tests -G Ninja
#        cmake --build /tmp/t113-tests
#        ctest --test-dir /tmp/t113-tests --output-on-failure
#
#   B. 作为主工程的子目录（随 -DACCESSTERMINAL_BUILD_TESTS=ON 启用）：
#        cmake -S . -B build -DACCESSTERMINAL_BUILD_TESTS=ON
#
# 为什么必须有 cmake_minimum_required 与 project()：
# 用法 B 时这两行由主 CMakeLists.txt 提供，本文件作为片段（fragment）无所谓；
# 但用法 A 下 CMake 会直接报
#   "No cmake_minimum_required command is present"
# 于是——**恰好在本机（开发机没有 ALSA）唯一可用的用法下**——单元测试完全
# 跑不起来。文件头注释一直写着可以 `cmake -S tests`，却没人验证过。这两行在
# 用法 B 下是幂等的（子目录里重复调用是允许的），因此对两种用法都安全。
#
# 测试是纯逻辑（协议编解码、指纹帧解析），只依赖 Qt5::Core/Test，
# 不依赖 ALSA / libgpiod / OpenCV，故可脱离主工程独立构建。
#
cmake_minimum_required(VERSION 3.16)
project(accessterminal_t113_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 两个测试文件末尾都有 `#include "test_xxx.moc"`（QTest 把用例类声明与实现
# 写在同一个 .cpp 里的常见写法），因此必须开 AUTOMOC，否则报
#   fatal error: test_protocol.moc: No such file or directory
# 主工程早已设置 AUTOMOC，此处补上才能独立构建。
set(CMAKE_AUTOMOC ON)

# 同理，主工程在顶层调用过 enable_testing()；独立构建时必须自己调用，
# 否则 add_test() 不会生成 CTestTestfile.cmake，ctest 会报
#   "No tests were found!!!"（构建成功却一个用例都跑不到）。
enable_testing()

# 作为子目录被主工程引入时 CMAKE_SOURCE_DIR 指向主工程根，src 在其下；
# 独立构建时 CMAKE_SOURCE_DIR 就是 tests/，src 在上一级。两种都要能工作。
if(EXISTS "${CMAKE_SOURCE_DIR}/src/core/net/protocol.h")
    set(ACCESSTERMINAL_SRC_ROOT "${CMAKE_SOURCE_DIR}/src")
else()
    get_filename_component(ACCESSTERMINAL_SRC_ROOT
                           "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)
endif()

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Test Gui Network SerialPort)

# ---------------------------------------------------------------------------
# 协议契约测试
# ---------------------------------------------------------------------------
add_executable(t113_test_protocol test_protocol.cpp)

target_include_directories(t113_test_protocol PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
)

target_link_libraries(t113_test_protocol PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME t113_protocol COMMAND t113_test_protocol)

# ---------------------------------------------------------------------------
# 指纹串口帧解析测试（纯逻辑，无需硬件）
# ---------------------------------------------------------------------------
add_executable(t113_test_fingerprint_parser test_fingerprint_parser.cpp)

target_include_directories(t113_test_fingerprint_parser PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
)

target_link_libraries(t113_test_fingerprint_parser PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME t113_fingerprint_parser COMMAND t113_test_fingerprint_parser)

# ---------------------------------------------------------------------------
# 配置加载与数值兜底（第 8 轮 SIGFPE 与第 9 轮"假开关"的回归测试）
#
# 链接真实的 src/config/app_config.cpp。不需要 ALSA / libgpiod / OpenCV，
# 因此可以脱离主工程独立构建。
# ---------------------------------------------------------------------------
add_executable(t113_test_app_config
    test_app_config.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/config/app_config.cpp
)

target_include_directories(t113_test_app_config PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
)

target_link_libraries(t113_test_app_config PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME t113_app_config COMMAND t113_test_app_config)

# ---------------------------------------------------------------------------
# AudioManager 写循环 / XRUN 恢复 / 打断 / 析构等待
#
# 本工程出过两个真实缺陷的类（第 11 轮 XRUN 静默丢数据、第 12 轮
# playAsync 漏等待导致 use-after-free），此前没有任何可执行测试。
#
# 危险分支全在"设备报错 / 被打断 / 并发"上，真实 ALSA 无法按需制造 -EPIPE、
# 也无法让写入变慢以观察打断时机，因此本目标链接 tests/fake_alsa.cpp ——
# 一份**可编程的假 ALSA**。它直接 include tools/devstubs/alsa/asoundlib.h，
# 于是每个函数签名都由头文件在编译期校验，与真实 ALSA 接口一致。
#
# 注意：只构建本目标就能完整验证 AudioManager 的控制流，不需要真实声卡，
# 也不需要 ALSA 开发库。
# ---------------------------------------------------------------------------
add_executable(t113_test_audio_manager
    test_audio_manager.cpp
    fake_alsa.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/managers/audio_manager.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/config/app_config.cpp   # 取 audioDevice()
)

target_include_directories(t113_test_audio_manager PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}                # fake_alsa.h
    ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs  # <alsa/asoundlib.h>
)

target_link_libraries(t113_test_audio_manager PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME t113_audio_manager COMMAND t113_test_audio_manager)

# ---------------------------------------------------------------------------
# AudioSenderThread：UDP 12345 上行音频
#
# 这条通路的**字节格式是协议硬约束**（裸 PCM、S16_LE/16000/单声道、
# 每包 320 帧 640 字节、无包头），但此前从未被真正验证过：devstubs 的
# sendto() 只"返回 len"、不记录载荷，其 inet_pton() 还永远返回 0，导致
# run() 在建连阶段就 return，**根本走不到发送代码**。
#
# 本目标改用可记录的 fake_net.cpp（记录每次 sendto 的目标与载荷）与
# 可注入采集错误的 fake_alsa.cpp，从而能对**线上字节**做断言。
# 因此它不能链接 devstubs/stub_impl.c（两者都定义 sendto/inet_pton）。
# ---------------------------------------------------------------------------
add_executable(t113_test_audio_sender
    test_audio_sender.cpp
    fake_alsa.cpp
    fake_net.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/core/media/audio_sender_thread.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/config/app_config.cpp
)

target_include_directories(t113_test_audio_sender PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs
)

target_link_libraries(t113_test_audio_sender PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME t113_audio_sender COMMAND t113_test_audio_sender)

# ---------------------------------------------------------------------------
# AudioReceiverThread：UDP 12345 下行音频播放
#
# 同一类问题：devstubs 的 select() 恒返回 0、recvfrom() 恒返回 -1，所以
# run() 里两个循环体**一次都没执行过**。这里用可投递数据报 / 可注入 errno 的
# fake_net，加上带 mixer 与 write 错误注入的 fake_alsa，把播放通路跑起来。
# ---------------------------------------------------------------------------
add_executable(t113_test_audio_receiver
    test_audio_receiver.cpp
    fake_alsa.cpp
    fake_net.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/core/media/audio_receiver_thread.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/config/app_config.cpp
)

target_include_directories(t113_test_audio_receiver PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs
)

# audio_receiver_thread.cpp 用到 fcntl/F_GETFL/F_SETFL/O_NONBLOCK —— 这些由
# tools/devstubs/host_compat.h 在主机端补齐（MinGW 没有 fcntl）。正式的
# check_host_compile.sh / check_host_link.sh 也是用 -include 注入同一文件。
target_compile_options(t113_test_audio_receiver PRIVATE
    -include ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs/host_compat.h
)

target_link_libraries(t113_test_audio_receiver PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME t113_audio_receiver COMMAND t113_test_audio_receiver)

# ---------------------------------------------------------------------------
# CameraPushThread：UDP 23456 视频推流
#
# UDP 23456 是协议硬约束（每包一个完整 JPEG、不重组不重传、端口固定），但此前
# 完全没有测试。与音频线程同类：真实 V4L2Camera 需要 /dev/video0，开发机上
# 不存在。这里用 fake_v4l2_camera.cpp 作为**链接期替身**替换相机（生产代码
# 不改一行），用可记录的 fake_net.cpp 断言线上字节。
#
# 注意：替换相机意味着**不能**同时链接 core/vision/v4l2_camera.cpp，
# 否则 V4L2Camera 的成员会重复定义。
# ---------------------------------------------------------------------------
add_executable(t113_test_video_push
    test_video_push.cpp
    fake_net.cpp
    fake_v4l2_camera.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/core/media/video_push_thread.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/config/app_config.cpp
    # 必须把真实的 v4l2_camera.h 显式列为源文件，AUTOMOC 才会为它生成 moc。
    # AUTOMOC 默认只为"与某个 .cpp 同名"的头文件生成 moc；本目标不含
    # v4l2_camera.cpp（它被 fake_v4l2_camera.cpp 顶替），所以没有任何源文件
    # 与它同名，moc 不会生成，链接期就会报
    #   undefined reference to `vtable for V4L2Camera'
    # （Q_OBJECT 的虚表由 moc 提供）。列进来即可，不需要任何生产代码改动。
    ${ACCESSTERMINAL_SRC_ROOT}/core/vision/v4l2_camera.h
)

target_include_directories(t113_test_video_push PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs
)

target_link_libraries(t113_test_video_push PRIVATE
    Qt5::Core
    Qt5::Test
    Qt5::Gui          # QImage / QBuffer
)

add_test(NAME t113_video_push COMMAND t113_test_video_push)

# ---------------------------------------------------------------------------
# CameraUdpServer：UDP 50000 远程监看
#
# 与 UDP 23456 的差异全是线上语义：只在收到裸 `getframe`（**不带 '\n'**）后
# 才推帧、收到 `stop` 立刻停、50ms/20fps、JPEG 用默认质量（通话视频是 75）。
# 此前没有测试。
#
# 本目标用**真实 QUdpSocket** 走 127.0.0.1:50000，把 bind、事件循环、
# readyRead 派发一起验证，而不是直接调私有槽（那会绕开真正的线上路径）。
# 相机仍用链接期替身，故不链接 v4l2_camera.cpp。
# ---------------------------------------------------------------------------
add_executable(t113_test_camera_preview
    test_camera_preview.cpp
    fake_v4l2_camera.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/core/media/camera_preview_server.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/config/app_config.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/core/vision/v4l2_camera.h  # 见下方说明
)

target_include_directories(t113_test_camera_preview PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs
)

target_link_libraries(t113_test_camera_preview PRIVATE
    Qt5::Core
    Qt5::Test
    Qt5::Gui
    Qt5::Network
)

add_test(NAME t113_camera_preview COMMAND t113_test_camera_preview)

# ---------------------------------------------------------------------------
# FingerprintManager：常驻识别与注册流程的**串口互斥**调度
#
# 门口机上只有一根指纹串口，两个使用者（常驻识别轮询、注册流程）都会
# readPacket() 抢读同一串口的应答，互为对方之应答 —— setSuspended() 就是
# 为此存在的互斥开关，此前**没有任何测试**。
#
# HailinFingerprint 的 buildPacket/sendPacket/readPacket 都不是 virtual，
# 无法继承覆盖，故用**链接期替身** tests/fake_fingerprint_device.cpp，
# 并**不链接** src/hal/fingerprint_hailin.cpp（否则成员重复定义）。
# 替身里 buildPacket() 是真实算法的逐字节拷贝：报文字节唯一的契约不能换。
#
# 注意 AUTOMOC：本目标不含任何与 fingerprint_hailin.h 同名的源文件，若不明
# 确把该头文件列进源文件列表，moc 不会生成，链接期报 undefined reference to
# `HailinFingerprint::identifyResult(...)` 等信号。
# ---------------------------------------------------------------------------
add_executable(t113_test_fingerprint_manager
    test_fingerprint_manager.cpp
    fake_fingerprint_device.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/managers/fingerprint_manager.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/hal/fingerprint_hailin.h   # AUTOMOC，见上
)

target_include_directories(t113_test_fingerprint_manager PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs
)

target_link_libraries(t113_test_fingerprint_manager PRIVATE
    Qt5::Core
    Qt5::Test
    Qt5::SerialPort
)

add_test(NAME t113_fingerprint_manager COMMAND t113_test_fingerprint_manager)

# ---------------------------------------------------------------------------
# SignalingClient：门口机侧 TCP 8887 信令客户端
#
# 第 24 轮首次量化覆盖率后发现 signaling_client.cpp 有 16/61 行从未执行，
# 恰好是**发送失败**与**断线重连**两条路径（47-51、87-92、124-139）。这几行
# 全在 TCP 8887 这条硬约束路径上：互操作测试覆盖了正常收发，但"连不上时会
# 不会退化成静默失败或疯狂重试"没人验证过。
#
# 本目标用真实 QTcpServer/QTcpSocket 走 127.0.0.1：连接被拒、3 秒后真的
# 重试、stop() 后不再重试 —— 全部靠可观测信号计数判定，不看内部标志位。
# ---------------------------------------------------------------------------
add_executable(t113_test_signaling_client
    test_signaling_client.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/core/net/signaling_client.cpp
)

target_include_directories(t113_test_signaling_client PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(t113_test_signaling_client PRIVATE
    Qt5::Core
    Qt5::Test
    Qt5::Network
)

add_test(NAME t113_signaling_client COMMAND t113_test_signaling_client)

# ---------------------------------------------------------------------------
# HailinFingerprint 的**真实**报文构造（fingerprint_hailin.cpp 的纯逻辑部分）
#
# 第 25 轮新增。此前报文字节从未被运行时测试直接验证过：
#   - t113_test_fingerprint_manager 用的是**链接期替身**
#     （tests/fake_fingerprint_device.cpp），其 buildFrame() 是真实算法的
#     逐字节拷贝，但那个"拷贝"**没有任何检查盯着它是否还一致**；
#   - 于是生产的 buildPacket() 覆盖率为 0，且契约存在两份副本。
#     有人改了生产的校验和起始下标或长度字段，替身不跟着变，测试照样全绿，
#     而真机报文已经错了 —— 这是第 23 轮"平行实现"教训的又一种形态。
#
# 本轮查明 buildPacket() 是 **public 且纯函数**（只拼字节，不碰串口），
# 构造函数也只设置串口参数、不 open()。因此可以**链接真实实现**直接测，
# 无需替身、也无需给生产代码加任何测试钩子。
#
# 与 t113_test_fingerprint_manager 相反的链接取向，两者互不干扰：
# 本目标**不含** fake_fingerprint_device.cpp，也不含 fingerprint_manager.cpp。
# ---------------------------------------------------------------------------
add_executable(t113_test_fingerprint_wire
    test_fingerprint_wire.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/hal/fingerprint_hailin.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/hal/fingerprint_frame_parser.h
)

target_include_directories(t113_test_fingerprint_wire PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(t113_test_fingerprint_wire PRIVATE
    Qt5::Core
    Qt5::Test
    Qt5::SerialPort
)

add_test(NAME t113_fingerprint_wire COMMAND t113_test_fingerprint_wire)

# ---------------------------------------------------------------------------
# PasswordManager：门禁密码不得进日志（第 27 轮补）
#
# 原始 baseline 的 checkPassword() 把**用户输入、正确密码、两者 hex** 全打进
# qDebug（已核对 baseline 源码）。门口机是实体门禁设备，密码是物理准入凭据，
# 而 qDebug 在嵌入式 Linux 上落 stderr / journal、跨重启留存 —— 输错一次就把
# 密码交出去。
#
# 重构已修掉，但此前**只有一条静态文本规则**（check_no_secret_in_logs）在守：
# 它扫源码里敏感变量名有没有出现在日志语句里。静态规则可能被绕过（改名、
# 拼串、经辅助函数转一手）。
#
# 本目标从**运行期**守：真的把密码设成哨兵串、真的调校验、真的抓 qDebug
# 输出，断言哨兵串及其 hex 一次都没出现。这也是 password_manager.cpp 第一次
# 被链接进任何测试目标（第 26 轮统计的 18 个"从未链接"文件之一）。
#
# 链接真实的 password_manager.cpp 与 app_config.cpp（密码来源）。不需要
# ALSA / libgpiod / OpenCV，也不需要真实硬件。
# ---------------------------------------------------------------------------
add_executable(t113_test_password_manager
    test_password_manager.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/managers/password_manager.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/config/app_config.cpp
)

target_include_directories(t113_test_password_manager PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
)

target_link_libraries(t113_test_password_manager PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME t113_password_manager COMMAND t113_test_password_manager)

# ---------------------------------------------------------------------------
# GpioLine：物理门锁/补光线的 RAII 与资源生命周期（第 28 轮补）
#
# GpioLine 管的是 T113 的 PE5（线号 133，开锁与补光共用同一条线，见
# widget.cpp:224-226），此前从未被链接进任何测试目标。危险分支全在硬件出错上：
# 打不开 chip / 拿不到线 / 请求失败 —— 处理错的后果是"该开门时不开"，
# 或**句柄泄漏把线占死**（下一次开锁直接失败）。
#
# 因此链接**可编程假 libgpiod** tests/fake_gpiod.cpp：它能注入这几类失败，
# 并用真实分配返回句柄、真的 delete，于是 liveLineCount()/liveChipCount()
# 能直接暴露泄漏与重复释放。callLog() 记录调用**顺序**，用于断言
# gpio.cpp 注释所声明的契约："先释放线，再关闭 chip"。
#
# 注意：**不能**链接 tools/devstubs/stub_impl.c —— 那是固定桩（返回值恒定、
# 不可编程），只能做链接检查，无法注入失败；两者对本目标二选一。
# ---------------------------------------------------------------------------
add_executable(t113_test_gpio
    test_gpio.cpp
    fake_gpiod.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/hal/gpio.cpp
)

target_include_directories(t113_test_gpio PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}                     # fake_gpiod.h
    ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs   # <gpiod.h>
)

target_link_libraries(t113_test_gpio PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME t113_gpio COMMAND t113_test_gpio)
