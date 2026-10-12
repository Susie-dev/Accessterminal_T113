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

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Test Gui Network SerialPort Widgets)

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
    ${ACCESSTERMINAL_SRC_ROOT}/managers/credential_store.cpp
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
# CredentialStore：口令**不得以明文留存**（第 41 轮，方案 P2）
#
# 重构前 `PasswordManager::checkPassword()` 是 `input == password` ——
# **明文比对**；密码来自 AppConfig，落盘在 appconfig.json 里也是明文
# （"door_password": "5788"）。门口机是实体门禁设备，配置文件可被任何能读它
# 的人拿到（含运维、备份、镜像），而门禁密码就是物理准入凭据。
#
# 本目标把 CredentialStore 单独测：加盐迭代哈希、定长比较、按用户计数、
# 锁定到期、临时码、以及**落盘文件里不含明文**。
#
# 关键写法（都是被本项目既有教训逼出来的）：
#   - 「文件里不含明文」是**否定性**断言，极易恒真。因此每条都配阳性对照：
#     文件存在且是合法 JSON、里面的哈希确实能验证正确口令、确实带 sha256 标记。
#   - 「锁定会到期」用**时钟注入**确定性验证，不用 sleep（测试会慢且不确定）。
#   - 「相同口令得到不同摘要」是**盐起作用唯一的直接证据**，必测。
#
# 纯逻辑类，只用 Qt5::Core + Qt5::Test，不需要硬件 / ALSA / OpenCV。
# ---------------------------------------------------------------------------
add_executable(t113_test_credential_store
    test_credential_store.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/managers/credential_store.cpp
)

target_include_directories(t113_test_credential_store PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
)

target_link_libraries(t113_test_credential_store PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME t113_credential_store COMMAND t113_test_credential_store)

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

# ---------------------------------------------------------------------------
# DoorActuator：与补光分离的**唯一**开锁出口（第 40 轮补）
#
# 第 40 轮之前本项目没有"开锁"这条代码路径：PE5 只有 5 处补光用法，
# 认证成功只播音；而 app_config.h 又写着"开锁继电器与补光灯共用 PE5"。
# 本目标把执行器的三条硬约束钉住：不假装开门、不阻塞、不重复驱动。
#
# 用假 libgpiod + **真实** GpioLine + **真实** DoorActuator。
# 头文件必须显式列进源列表让 AUTOMOC 处理（它带 Q_OBJECT）。
# ---------------------------------------------------------------------------
add_executable(t113_test_door_actuator
    test_door_actuator.cpp
    fake_gpiod.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/hal/gpio.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/managers/door_actuator.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/managers/door_actuator.h
)

target_include_directories(t113_test_door_actuator PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}                     # fake_gpiod.h
    ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs   # <gpiod.h>
)

target_link_libraries(t113_test_door_actuator PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME t113_door_actuator COMMAND t113_test_door_actuator)

# ---------------------------------------------------------------------------
# AccessController：唯一的放行入口（第 40 轮 P1 补）
#
# 把"谁能进门"从 Widget 的四个 lambda 收敛到本类之后，两件事只有这里能验：
# 静默期为 0 时必须**完全不抑制**（默认值 = 保持现状），以及静默期大于 0 时
# 必须真的抑制、不触达执行器、到期自动恢复。
#
# 真 AccessController + 真 DoorActuator + 真 GpioLine + 可编程假 libgpiod。
# ---------------------------------------------------------------------------
add_executable(t113_test_access_controller
    test_access_controller.cpp
    fake_gpiod.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/hal/gpio.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/managers/door_actuator.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/managers/door_actuator.h
    ${ACCESSTERMINAL_SRC_ROOT}/managers/access_controller.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/managers/access_controller.h
)

target_include_directories(t113_test_access_controller PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}                     # fake_gpiod.h
    ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs   # <gpiod.h>
)

target_link_libraries(t113_test_access_controller PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME t113_access_controller COMMAND t113_test_access_controller)

# ---------------------------------------------------------------------------
# FaceManager：人脸门禁的注册 / 训练 / 识别（第 29 轮补）
#
# FaceManager 决定**门开不开**：widget.cpp:889 检出人脸后调用 recognize()，
# 由 recognizeResult 决定是否开门。它此前从未被链接进任何测试目标。
#
# 本机构建带 OpenCV face 模块（libopencv_face.dll.a），因此这里**链接真实的
# face_manager.cpp 并用真实 LBPH 跑** —— 不写替身，避免"替身自我验证"。
#
# 写断言前先做了临时探针，量到三条决定性事实：
#   1. 模型未训练时 predict() 抛 cv::Exception，所以"样本不足"守卫一旦失效
#      会抛出异常、逃出 UI 事件循环即终止进程；
#   2. **单 label 时 `predictedLabel == 1` 恒为真**（训练 labels 全是 1），
#      因此阈值 `confidence < threshold` 是**唯一的门**；
#   3. 我原本怀疑的"越界裁剪 -> 空样本 -> train 抛异常"链**不成立**：
#      QImage::copy() 传越界矩形不返回空图。该猜测已被推翻，未写成断言。
#
# 由此阈值相关断言一律用**被测代码自己吐出的 confidence 当基准**
# （阈值 = conf 必须拒绝；conf + 1 必须接受），与 OpenCV 版本无关。
#
# OpenCV 是**可选依赖**：找不到 face 模块时跳过本目标（其余测试仍可独立构建，
# 保持本文件头部"可脱离主工程独立构建"的承诺）。交叉编译到 Tina SDK 时若
# 目标平台 OpenCV 不含 face 模块，也走同一分支。
# ---------------------------------------------------------------------------
find_package(OpenCV QUIET COMPONENTS core imgproc face)

if(OpenCV_FOUND AND OpenCV_face_FOUND)
    add_executable(t113_test_face_manager
        test_face_manager.cpp
        ${ACCESSTERMINAL_SRC_ROOT}/managers/face_manager.cpp
    )

    target_include_directories(t113_test_face_manager PRIVATE
        ${ACCESSTERMINAL_SRC_ROOT}
        ${OpenCV_INCLUDE_DIRS}
    )

    target_link_libraries(t113_test_face_manager PRIVATE
        Qt5::Core
        Qt5::Test
        Qt5::Gui            # QImage
        ${OpenCV_LIBS}
    )

    add_test(NAME t113_face_manager COMMAND t113_test_face_manager)
else()
    message(STATUS "未找到 OpenCV face 模块，跳过 t113_test_face_manager"
                   "（人脸门禁测试需要真实 LBPH）")
endif()

# ---------------------------------------------------------------------------
# FaceQuality：人脸质量门（第 43 轮，方案 P4）
#
# 背景：本工程已实测确认 LBPH 在单 label 下 `predictedLabel == 1` **恒为真**，
# 距离阈值是**唯一的门**。因此一张模糊/过曝/过暗的图只要凑巧落进阈值，
# **门就开了** —— 这是安全问题，不是体验问题。质量门是识别之前的第一道判断。
#
# 为什么这里**没有** OpenCV 可选分支（与本文件 t113_face_manager 的写法不同）
# ---------------------------------------------------------------------------
# 方案文档原计划"按本项目惯例缺 OpenCV 时跳过"。实现时发现清晰度只需一个
# 四邻域 Laplacian 卷积、十几行纯 C++，于是改成**纯 Qt 实现**：本目标在
# 任何环境都真的跑。理由正是 README §78 —— "会静默跳过/安静地少跑一条"
# 的检查比没有检查更糟：t113_face_manager 在本机跑得起来，但换一台没装
# OpenCV face 模块的机器就会安静消失，而没人会注意到。
#
# 纯逻辑类，只用 Qt5::Core/Test/Gui，不需要硬件 / ALSA / OpenCV。
# ---------------------------------------------------------------------------
add_executable(t113_test_face_quality
    test_face_quality.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/managers/face_quality.cpp
)

target_include_directories(t113_test_face_quality PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
)

target_link_libraries(t113_test_face_quality PRIVATE
    Qt5::Core
    Qt5::Test
    Qt5::Gui            # QImage
)

add_test(NAME t113_face_quality COMMAND t113_test_face_quality)

# ---------------------------------------------------------------------------
# V4L2Camera：门口机的画面来源（第 30 轮补）
#
# V4L2Camera 是整个视频链路的源头：UDP 23456 通话视频、UDP 50000 远程监看、
# 人脸检测、人脸门禁全都取自它的 getFrame()。它此前从未被链接进任何测试目标，
# 而它的危险分支全在**系统调用失败**上（设备不存在/权限不足/S_FMT/REQBUFS/
# QUERYBUF/mmap/QBUF/STREAMON/DQBUF/JPEG 解码失败）。
#
# 现有 tools/devstubs/stub_impl.c 是**固定**桩（ioctl 恒 -1、mmap 恒
# MAP_FAILED），只能做链接检查 —— 走不到任何成功路径，也无法按需让某一环失败。
# 因此本目标链接**可编程假 syscall 层** tests/fake_v4l2_io.cpp，并用
# **-Wl,--wrap** 把它接到 ::open/::close/ioctl/mmap/munmap 上。
#
# 为什么必须用 --wrap 而不是直接定义同名符号：
#   - open/close 在 MinGW 的 CRT 里已存在，直接定义会冲突；
#   - ioctl/mmap/munmap 在 MinGW 上根本不存在。
# 两个坑（探针实测，第一版就踩了）：
#   1. --wrap 生成的是 **C 符号**引用（__wrap_open）。定义必须 extern "C"，
#      否则被名字修饰掉，链接期报 undefined reference to '__wrap_open'。
#   2. 夹具**只劫持 /dev/video\***，其余委托 __real_open —— 否则整个进程的
#      文件 I/O 都被接管，测试会因无关原因失败。
#
# 链接真实的 v4l2_camera.cpp（不用替身），UI 定时器由测试直接调
# captureLoop() 驱动。
# ---------------------------------------------------------------------------
add_executable(t113_test_v4l2_camera
    test_v4l2_camera.cpp
    fake_v4l2_io.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/core/vision/v4l2_camera.cpp
)

target_include_directories(t113_test_v4l2_camera PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}                     # fake_v4l2_io.h
    ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs   # <linux/videodev2.h> 等
)

target_link_libraries(t113_test_v4l2_camera PRIVATE
    Qt5::Core
    Qt5::Test
    Qt5::Gui          # QImage
)

# --wrap 只在链接本目标的这些对象时生效；Qt 自身的 open() 在 DLL 内部已绑定，
# 不受影响。__real_open/__real_close 由链接器自动提供。
target_link_options(t113_test_v4l2_camera PRIVATE
    -Wl,--wrap=open
    -Wl,--wrap=close
    -Wl,--wrap=ioctl
    -Wl,--wrap=mmap
    -Wl,--wrap=munmap
)

add_test(NAME t113_v4l2_camera COMMAND t113_test_v4l2_camera)

# ---------------------------------------------------------------------------
# 密码输入窗体（src/ui/form_passwd.cpp）—— 本工程**第一个 Qt Widgets 测试目标**
#
# 为什么值得测：它是真实门禁的密码录入路径，而且是"密码不得进日志"这条安全
# 性质的唯一现场。此前该性质**只由一条静态文本规则**（check_no_secret_in_logs）
# 守着，而那条规则自己写着"只做词法判定，不做数据流分析"。于是只要换一种写法
# （先赋给临时变量再打印、或打印拼接结果），静态规则看不出来，而没有任何行为
# 测试会拦它。本测试改成：真的把按键点下去，用 qInstallMessageHandler 捕获
# **实际日志输出**，再断言日志里不含刚输入的密码。这比静态规则强，因为它不
# 关心密码"怎么"漏出去。
#
# 这是第一个 Widgets 目标，写之前用探针（%TEMP%\probe35\）量清了三件有风险的
# 事：uic 能处理真实 form_passwd.ui；QT_QPA_PLATFORM=offscreen 下 QApplication
# 能起来且 QTest::mouseClick 真能触发 clicked；按钮文本恰好是数字（否则
# btn->text() 拼出的密码是错的）。
#
# 替身只替换两条**不影响密码路径**的东西：
#   - tests/shadows/ui/widget.h  ── 真 ui/widget.h 会拖进 OpenCV + 整个主程序
#     门面（十几个管理器、ui_widget.h）。
#   - tests/shadows/managers/audio_manager.h ── 真 AudioManager 会
#     QtConcurrent::run 去开 ALSA 放音，慢且依赖声卡。
#
# 两条都做成**影子头**（替换路径，而不是另起假文件名）：这样 form_passwd.cpp
# 里原有的 `#include "ui/widget.h"` / `#include "managers/audio_manager.h"`
# 一个字都不用改就命中替身。AudioManager 必须也走影子头，否则它会同时看到真
# 头与假头，报 "redefinition of 'class AudioManager'"（第 35 轮实测）。
#
# 两个影子头都带 Q_OBJECT，**必须显式列进源列表**让 AUTOMOC 处理，否则链接期
# 报 undefined reference to ...::staticMetaObject（实测：只放进 include 目录
# 不够，AUTOMOC 不看 include 目录）。这也是第 35 轮的实测结论之一。
# ---------------------------------------------------------------------------
qt5_wrap_ui(T113_FORM_PASSWD_UIS
    ${ACCESSTERMINAL_SRC_ROOT}/ui/form_passwd.ui
)

add_executable(t113_test_form_passwd
    test_form_passwd.cpp
    # 影子头必须列进来，AUTOMOC 才会为它们生成元对象
    ${CMAKE_CURRENT_LIST_DIR}/shadows/ui/widget.h
    ${CMAKE_CURRENT_LIST_DIR}/shadows/managers/audio_manager.h
    ${ACCESSTERMINAL_SRC_ROOT}/ui/form_passwd.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/config/app_config.cpp
    ${T113_FORM_PASSWD_UIS}
)

# BEFORE：影子 ui/widget.h 与 managers/audio_manager.h 必须排在真头之前命中。
target_include_directories(t113_test_form_passwd BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/shadows
)

target_include_directories(t113_test_form_passwd PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_BINARY_DIR}                   # ui_form_passwd.h（uic 产物）
)

target_compile_definitions(t113_test_form_passwd PRIVATE
    ACC_FORM_PASSWD_TEST
)

target_link_libraries(t113_test_form_passwd PRIVATE
    Qt5::Core
    Qt5::Test
    Qt5::Gui
    Qt5::Widgets
)

add_test(NAME t113_form_passwd COMMAND t113_test_form_passwd)

# 无显示器环境（CI、交叉编译后的板子、SSH）也要能跑：offscreen 平台插件。
# 测试自身在 initTestCase() 里也会兜底设置，这里设是为了让 ctest 直接跑也稳。
set_tests_properties(t113_form_passwd PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# ---------------------------------------------------------------------------
# 通话窗体（src/ui/form_call.cpp）—— **stopsend 的发源地**
#
# 为什么值得测：form_call.cpp 是门口机唯一发出 stopsend 信令的地方，而
# stopsend 在硬约束清单里（TCP 8887 信令语义必须与 baseline 逐字节兼容）。
#
# 此前两头都有测试、中间没人守：
#   - protocol.h 的 StopSend <-> "stopsend" 映射：test_protocol.cpp 有；
#   - SignalingClient::send(StopSend) 写出 "stopsend\n"：test_signaling_client
#     与 interop 有；
#   - **"点下停止按钮 -> stopsend 真的被发出去"这条接线：没有**。
#     form_call.cpp 一直挂在"从未被任何测试目标链接的产线文件"清单上。
#
# 后果：把 `widget->notifyPeerStopSend();` 那一行删掉，此前全部测试目标都会
# 照常通过 —— 现场表现为"点了挂断，但室内机不知道本端已停发"。
#
# 本目标的做法：起**真实** QTcpServer + **真实** SignalingClient，把真实
# Widget 替身挂成父窗口，真的点下按钮，断言**对端收到的字节恰好是
# "stopsend\n"**。断言落在线上字节上，因此"不发、发错、发两次、漏了换行"
# 都会红。
#
# 复用第 35 轮的两个影子头（widget.h 已扩展出 notifyPeerStopSend() 与
# setSignalingClientForTest()），安全阀宏换成 ACC_FORM_CALL_TEST。
# 注意：SignalingClient 用**真实实现**，不用替身。
# ---------------------------------------------------------------------------
qt5_wrap_ui(T113_FORM_CALL_UIS
    ${ACCESSTERMINAL_SRC_ROOT}/ui/form_call.ui
)

add_executable(t113_test_form_call
    test_form_call.cpp
    # 影子头必须列进来，AUTOMOC 才会为它们生成元对象
    ${CMAKE_CURRENT_LIST_DIR}/shadows/ui/widget.h
    ${CMAKE_CURRENT_LIST_DIR}/shadows/managers/audio_manager.h
    ${ACCESSTERMINAL_SRC_ROOT}/ui/form_call.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/config/app_config.cpp
    # 真实信令客户端：stopsend 必须真的走到 TCP 上
    ${ACCESSTERMINAL_SRC_ROOT}/core/net/signaling_client.cpp
    ${T113_FORM_CALL_UIS}
)

# BEFORE：影子 ui/widget.h 与 managers/audio_manager.h 必须排在真头之前命中。
target_include_directories(t113_test_form_call BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/shadows
)

target_include_directories(t113_test_form_call PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_BINARY_DIR}                   # ui_form_call.h（uic 产物）
)

target_compile_definitions(t113_test_form_call PRIVATE
    ACC_FORM_CALL_TEST
)

target_link_libraries(t113_test_form_call PRIVATE
    Qt5::Core
    Qt5::Test
    Qt5::Gui
    Qt5::Widgets
    Qt5::Network                                  # QTcpServer / QTcpSocket
)

add_test(NAME t113_form_call COMMAND t113_test_form_call)

set_tests_properties(t113_form_call PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# ---------------------------------------------------------------------------
# 指纹注册窗体（src/ui/form_finger.cpp）—— 现场**真正执行**的注册状态机
#
# 为什么值得测：指纹注册在工程里有**两份**实现，
#   A) FingerprintManager::startEnroll(int) —— 有测试、**零生产调用者**；
#   B) Form_finger::startFingerRegisterStep() + doFingerCollect()
#      —— 内联状态机，直接读写串口，**这才是现场执行的那条**。
#
# 第 23 轮的静态规则 check_live_fingerprint_enroll_sequence_is_pinned 已经把 B 的
# **命令字节序列**钉住了（它的注释里就写着"B 无法用单元测试覆盖"）。但静态规则
# 只能看文本，看不到三类真实缺陷：
#
#   1. **串口互斥没有成对恢复**：注册期间必须 setSuspended(true) 挂起常驻识别
#      （两者抢同一个串口，会互为对方之应答）。退出/析构漏了恢复，后果是
#      **门禁指纹识别永久失效**，而命令字节一个都没变 —— 静态规则看不出来。
#   2. **轮询定时器泄漏**：回调里 readPacket() 是阻塞读且跑在 GUI 线程；退出时
#      不停表，窗口隐藏后仍每 500ms 阻塞一次 GUI，把界面拖死。
#   3. **延时后继步骤未被作废**：分步之间 QTimer::singleShot(1500) 进入下一步，
#      用户在这 1.5 秒内退出时，仅停表不够（m_pollGeneration 就是为此存在）。
#
# 做法与第 35/36 轮一致：影子头替掉 Widget 与 AudioManager，串口用项目里**原有
# 的**链接期替身 tests/fake_fingerprint_device.cpp（它的 buildPacket() 是真实
# 算法逐字节拷贝，报文字节唯一的契约不能换）。
#
# **关键取向**：FingerprintManager 用**真实实现**。互斥开关 setSuspended() 的
# 可观测效果（停表、恢复时重启识别）就在真实现里；换成替身，"有没有成对恢复"
# 就退化成"替身自己记了一笔"（第 36 轮教训）。
#
# 注意：widget.h 里的指纹相关成员用 ACC_FORM_FINGER_TEST 圈起来了，因为
# hal/fingerprint_hailin.h 会拉进 <QSerialPort>，只有本目标链接了
# Qt5::SerialPort。不加宏会让 form_passwd / form_call 两个目标编译失败。
# ---------------------------------------------------------------------------
qt5_wrap_ui(T113_FORM_FINGER_UIS
    ${ACCESSTERMINAL_SRC_ROOT}/ui/form_finger.ui
)

add_executable(t113_test_form_finger
    test_form_finger.cpp
    # 项目里原有的链接期串口替身（buildPacket 为真实算法）
    fake_fingerprint_device.cpp
    # 影子头必须列进来，AUTOMOC 才会为它们生成元对象
    ${CMAKE_CURRENT_LIST_DIR}/shadows/ui/widget.h
    ${CMAKE_CURRENT_LIST_DIR}/shadows/managers/audio_manager.h
    ${ACCESSTERMINAL_SRC_ROOT}/ui/form_finger.cpp
    ${ACCESSTERMINAL_SRC_ROOT}/config/app_config.cpp
    # 真实指纹管理器：串口互斥语义在这里，不能用替身
    ${ACCESSTERMINAL_SRC_ROOT}/managers/fingerprint_manager.cpp
    # AUTOMOC：替身顶掉了 fingerprint_hailin.cpp，若不显式列出该头文件，
    # moc 不会生成，链接期报 undefined reference to HailinFingerprint::* 信号
    ${ACCESSTERMINAL_SRC_ROOT}/hal/fingerprint_hailin.h
    ${T113_FORM_FINGER_UIS}
)

# BEFORE：影子 ui/widget.h 与 managers/audio_manager.h 必须排在真头之前命中。
target_include_directories(t113_test_form_finger BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/shadows
)

target_include_directories(t113_test_form_finger PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
    ${CMAKE_CURRENT_LIST_DIR}                     # fake_fingerprint_device.h
    ${CMAKE_CURRENT_LIST_DIR}/../tools/devstubs    # 串口桩
    ${CMAKE_CURRENT_BINARY_DIR}                    # ui_form_finger.h（uic 产物）
)

target_compile_definitions(t113_test_form_finger PRIVATE
    ACC_FORM_FINGER_TEST
)

target_link_libraries(t113_test_form_finger PRIVATE
    Qt5::Core
    Qt5::Test
    Qt5::Gui
    Qt5::Widgets
    Qt5::SerialPort
)

add_test(NAME t113_form_finger COMMAND t113_test_form_finger)

set_tests_properties(t113_form_finger PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)
