cmake_minimum_required(VERSION 3.16)
project(accessterminal_interop LANGUAGES CXX)

# ============================================================================
# 跨工程信令互通测试
#
# 把两个仓库里的**真实**信令实现链接到一起，在 127.0.0.1 上做真实 TCP 收发：
#   Accessterminal_desktop/src/core/net/signaling_server.cpp  （室内机侧）
#   Accessterminal_T113/src/core/net/signaling_client.cpp      （门口机侧）
#
# 这是对“协议逐字节兼容”这一硬约束最强的验证：
# 前者的 reply()/broadcast() 输出的字节，后者必须能解析；
# 反之亦然。任何端口、信令字符串、分包（'\n'）处理的偏差都会让测试失败。
# ============================================================================
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# 注册 add_test 前必须启用测试，否则该注册是"哑"的：
# 构建成功、CTestTestfile.cmake 不生成，ctest 报 "No tests were found!!!"。
# run_interop.sh 目前直接运行可执行文件，不依赖 ctest，因此这个缺陷此前一直
# 没有暴露；启用后 ctest 也能驱动同一套用例（两种入口等价）。
enable_testing()

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Network Test)

if(NOT DESKTOP_REPO OR NOT T113_REPO)
    message(FATAL_ERROR "必须指定 -DDESKTOP_REPO=<路径> -DT113_REPO=<路径>")
endif()

add_executable(test_interop
    test_interop.cpp
    # 室内机侧真实实现
    "${DESKTOP_REPO}/src/core/net/signaling_server.cpp"
    # 门口机侧真实实现
    "${T113_REPO}/src/core/net/signaling_client.cpp"
)

target_include_directories(test_interop PRIVATE
    # 两个仓库的 src 都要在 include 路径中：各自的 protocol.h 与
    # signaling_*.h 用 "core/net/xxx.h" 形式互相引用。
    # 为避免同名 protocol.h 冲突，两个仓库的 protocol.h 内容逐字节一致，
    # 由 include guard 保证只生效一份。
    "${DESKTOP_REPO}/src"
    "${T113_REPO}/src"
)

target_link_libraries(test_interop PRIVATE
    Qt5::Core Qt5::Network Qt5::Test
)

add_test(NAME interop COMMAND test_interop)
