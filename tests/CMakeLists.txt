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
add_executable(test_protocol test_protocol.cpp)

target_include_directories(test_protocol PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
)

target_link_libraries(test_protocol PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME protocol COMMAND test_protocol)

# ---------------------------------------------------------------------------
# 指纹串口帧解析测试（纯逻辑，无需硬件）
# ---------------------------------------------------------------------------
add_executable(test_fingerprint_parser test_fingerprint_parser.cpp)

target_include_directories(test_fingerprint_parser PRIVATE
    ${ACCESSTERMINAL_SRC_ROOT}
)

target_link_libraries(test_fingerprint_parser PRIVATE
    Qt5::Core
    Qt5::Test
)

add_test(NAME fingerprint_parser COMMAND test_fingerprint_parser)
