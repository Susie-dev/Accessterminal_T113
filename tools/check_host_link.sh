#!/usr/bin/env bash
# ============================================================================
# T113 工程“主机端链接检查”
#
# 在 check_host_compile.sh 的语法检查之上再进一步：用 devstubs/stub_impl.c
# 提供的 ALSA/libgpiod 桩实现，把整个工程**真的链接**成一个可执行文件，
# 以发现静态检查与 -fsyntax-only 都覆盖不到的缺陷：
#   - undefined reference（声明了却没有定义）
#   - duplicate symbol（重复定义）
#   - 错误的库顺序 / 漏链某个 Qt 模块
#
# 产物不可运行（桩实现不做真实 I/O），仅用于验证链接完整性。
# 需要本机有 Qt5 与 OpenCV 的开发包。
# ============================================================================
set -u

if [ -d /mingw64/bin ]; then
    case ":$PATH:" in
        *:/mingw64/bin:*) ;;
        *) PATH="/mingw64/bin:/usr/bin:$PATH" ;;
    esac
    export PATH
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 仓库根：优先取环境变量/参数，否则回退到脚本所在目录的上一级
# （脚本若位于仓库 tools/ 内，这一回退是成立的）。
REPO_ROOT="${REPO_ROOT:-${1:-}}"
if [ -z "$REPO_ROOT" ]; then
    if [ -d "$SCRIPT_DIR/devstubs" ] && [ -d "$SCRIPT_DIR/../src" ]; then
        REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
        STUBS="$SCRIPT_DIR/devstubs"
    else
        echo "错误：请用 REPO_ROOT=<仓库根> 指定 T113 仓库路径。" >&2
        exit 1
    fi
else
    REPO_ROOT="$(cd "$REPO_ROOT" && pwd)"
    STUBS="${STUBS:-$REPO_ROOT/tools/devstubs}"
fi
SRC_ROOT="$REPO_ROOT"
OUT="${TMPDIR:-/tmp}/t113-host-link"
rm -rf "$OUT"; mkdir -p "$OUT"

# Windows 上的 CMake 不理解 MSYS 的 /c/... 形式路径，需转换为原生路径。
# Linux/macOS 上 cygpath 不存在，此时保持原值。
if command -v cygpath >/dev/null 2>&1; then
    SRC_ROOT_WIN="$(cygpath -w "$SRC_ROOT")"
    STUBS_WIN="$(cygpath -w "$STUBS")"
    OUT_WIN="$(cygpath -w "$OUT")"
else
    SRC_ROOT_WIN="$SRC_ROOT"
    STUBS_WIN="$STUBS"
    OUT_WIN="$OUT"
fi

cat > "$OUT/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(t113_host_link_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui Widgets Network SerialPort Concurrent)
find_package(OpenCV REQUIRED)

file(GLOB_RECURSE APP_SOURCES "${T113_SRC}/src/*.cpp")
file(GLOB_RECURSE APP_FORMS   "${T113_SRC}/src/*.ui")
list(APPEND APP_FORMS "${T113_SRC}/src/resources/res.qrc")

add_executable(smartpro_link_check
    ${APP_SOURCES} ${APP_FORMS}
    "${STUB_IMPL}"
)

target_include_directories(smartpro_link_check PRIVATE
    "${T113_SRC}/src"
    "${STUBS}"
)

# host_compat.h 提供 MinGW 缺失的 fcntl/F_GETFL 等；仅在主机链接检查时注入。
target_compile_definitions(smartpro_link_check PRIVATE
    ACCESSTERMINAL_HOST_LINK_CHECK
)

# 说明：CMake 会把 target_compile_options 中重复出现的 -include 去重，
# 因此这里把多个强制包含合并进**同一个** SHELL: 选项串。
#
# 这里只写文件名而不写绝对路径：STUBS 已在 target_include_directories 中，
# 而 Windows 路径中的反斜杠会被 SHELL: 解析吞掉（变成 C:Users...），
# 用名字交给预处理器按 include 路径查找最为稳妥。
set(_host_includes "-include host_compat.h")

# 本机 OpenCV 5.x 的 CascadeClassifier 位于 xobjdetect.hpp（目标为 4.x），
# 仅对检查目标追加强制包含，工程源码不动。
if(EXISTS "${OpenCV_INCLUDE_DIRS}/opencv2/xobjdetect.hpp")
    string(APPEND _host_includes " -include opencv2/xobjdetect.hpp")
endif()

target_compile_options(smartpro_link_check PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:${_host_includes}>"
    "$<$<COMPILE_LANGUAGE:C>:-DACCESSTERMINAL_STUB_IMPL_C>"
)

target_link_libraries(smartpro_link_check PRIVATE
    Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network Qt5::SerialPort Qt5::Concurrent
    ${OpenCV_LIBS}
)
EOF

echo "=== 配置（链接检查目标）==="
cmake -S "$OUT_WIN" -B "$OUT_WIN/build" \
      -DT113_SRC="$SRC_ROOT_WIN" -DSTUBS="$STUBS_WIN" \
      -DSTUB_IMPL="$STUBS_WIN/stub_impl.c" \
      -DCMAKE_BUILD_TYPE=Debug > "$OUT/cmake.log" 2>&1 \
  && echo "  配置 OK" || { echo "  配置失败:"; tail -25 "$OUT/cmake.log"; exit 1; }

echo ""
echo "=== 编译并链接 ==="
cmake --build "$OUT_WIN/build" -j "${JOBS:-4}" > "$OUT/build.log" 2>&1
RC=$?
if [ $RC -ne 0 ]; then
    echo "  链接检查失败："
    echo "  ----------------------------------------"
    grep -E "error:|undefined reference|multiple definition" "$OUT/build.log" | head -30
    echo "  ----------------------------------------"
    tail -20 "$OUT/build.log"
    exit 1
fi

echo "  编译并链接 OK"
echo ""
echo "=== 产物 ==="
find "$OUT/build" -maxdepth 1 -type f -name 'smartpro_link_check*' -exec ls -la {} \; 2>/dev/null || true
echo ""
echo "说明：该可执行文件不可运行（ALSA/libgpiod 为桩实现），"
echo "      仅用于验证“全部符号都有定义”这一链接完整性。"
