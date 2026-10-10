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

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Test)

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
