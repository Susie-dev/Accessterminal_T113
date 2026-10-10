#!/usr/bin/env bash
# ============================================================================
# 跨工程信令互通测试
#
# 把 Accessterminal_T113（门口机）与 Accessterminal_desktop（室内机）里的
# **真实**信令实现链接到一起，在 127.0.0.1 上做真实 TCP 收发，逐条校验。
#
# 这是对“协议逐字节兼容”这一硬约束最强的验证：
# 一端实际写出的字节，另一端必须能正确解析——端口、信令字符串、'\n' 分包、
# 粘包与半包处理，任何偏差都会在这里失败。
#
# 用法：
#   bash tests/interop/run_interop.sh
#   DESKTOP_REPO=/path/to/Accessterminal_desktop bash tests/interop/run_interop.sh
#
# 退出码：0 = 全部通过，非 0 = 失败。
# ============================================================================
set -u

# ---- 定位脚本自身与仓库根 ----
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
T113_REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ---- 定位同级仓库 Accessterminal_desktop ----
if [ -z "${DESKTOP_REPO:-}" ]; then
    DESKTOP_REPO="$(cd "$T113_REPO/.." && pwd)/Accessterminal_desktop"
fi

if [ ! -f "$DESKTOP_REPO/src/core/net/signaling_server.cpp" ]; then
    echo "错误：找不到室内机仓库（Accessterminal_desktop）。"
    echo "      期望路径: $DESKTOP_REPO"
    echo "      可用 DESKTOP_REPO=/path/to/Accessterminal_desktop 显式指定。"
    exit 2
fi

# ---- 环境准备 ----
export PATH=/mingw64/bin:/usr/bin:$PATH

missing=""
for tool in cmake ninja g++; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
    echo "错误：缺少工具:$missing"
    echo "      需要 MSYS2（mingw64）环境，并安装 cmake / ninja / gcc。"
    exit 2
fi

# ---- 构建目录放在仓库外，避免污染工作区 ----
BUILD_DIR="${INTEROP_BUILD_DIR:-/tmp/accessterminal-interop}"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "=== 配置 ==="
echo "  门口机: $T113_REPO"
echo "  室内机: $DESKTOP_REPO"
echo ""

cmake -S "$(cygpath -w "$SCRIPT_DIR")" -B "$(cygpath -w "$BUILD_DIR")" -G Ninja \
      -DDESKTOP_REPO="$(cygpath -w "$DESKTOP_REPO")" \
      -DT113_REPO="$(cygpath -w "$T113_REPO")" > "$BUILD_DIR/cmake.log" 2>&1
if [ $? -ne 0 ]; then
    echo "  配置失败:"
    tail -25 "$BUILD_DIR/cmake.log"
    exit 1
fi
echo "  配置 OK"

echo ""
echo "=== 编译 ==="
cmake --build "$(cygpath -w "$BUILD_DIR")" > "$BUILD_DIR/build.log" 2>&1
if [ $? -ne 0 ]; then
    echo "  编译失败:"
    grep -E "error:" "$BUILD_DIR/build.log" | head -25
    tail -15 "$BUILD_DIR/build.log"
    exit 1
fi
echo "  编译 OK"

echo ""
echo "=== 运行互通测试 ==="
"$BUILD_DIR/test_interop.exe"
rc=$?
exit $rc
