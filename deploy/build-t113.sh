#!/bin/sh
#
# 构建 Accessterminal_T113（门口机）—— T113 交叉编译
#
# 用法：
#   export T113_SYSROOT=/path/to/Tina-Linux/out/<board>/staging_dir/target
#   export T113_TOOLCHAIN_DIR=/path/to/toolchain/bin
#   export OPENCV_ROOT=/path/to/opencv_arm_build/_install
#   export GPIO_ROOT=/path/to/libgpiod/_install
#   ./deploy/build-t113.sh
#
# 全部路径均可通过环境变量提供；本脚本不写死任何开发机绝对路径。
#
set -e

SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$SRC_DIR/build-t113}"

# ---- 必需变量检查 ----
if [ -z "$T113_SYSROOT" ]; then
    echo "错误: 必须设置 T113_SYSROOT 指向 Tina SDK 的 staging_dir/target" >&2
    echo "例如: export T113_SYSROOT=/path/to/Tina-Linux/out/t113-bingpi_m2/staging_dir/target" >&2
    exit 1
fi

echo "=== 构建 Accessterminal_T113 (交叉编译) ==="
echo "源码目录:   $SRC_DIR"
echo "构建目录:   $BUILD_DIR"
echo "sysroot:    $T113_SYSROOT"
[ -n "$OPENCV_ROOT" ] && echo "OpenCV:     $OPENCV_ROOT"
[ -n "$GPIO_ROOT" ]   && echo "libgpiod:   $GPIO_ROOT"
echo ""

# ---- 配置 ----
# 注意：交叉编译时默认关闭测试（无法在 x86 主机上运行 ARM 二进制）。
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$SRC_DIR/cmake/toolchain-t113.cmake" \
    -DT113_SYSROOT="$T113_SYSROOT" \
    -DOPENCV_ROOT="$OPENCV_ROOT" \
    -DGPIO_ROOT="$GPIO_ROOT" \
    -DCMAKE_BUILD_TYPE=Release \
    -DACCESSTERMINAL_BUILD_TESTS=OFF \
    -DACCESSTERMINAL_STRIP=ON

# ---- 编译 ----
JOBS="$(nproc 2>/dev/null || echo 4)"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo ""
echo "=== 构建完成 ==="
echo "产物: $BUILD_DIR/bin/smartpro"
if command -v file >/dev/null 2>&1; then
    file "$BUILD_DIR/bin/smartpro"
fi
echo ""
echo "推送与部署："
echo "  scp $BUILD_DIR/bin/smartpro root@<T113_IP>:/tmp/"
echo "  scp -r $SRC_DIR/deploy/vendor/pcm root@<T113_IP>:/tmp/"
echo "  scp $SRC_DIR/config/appconfig.json root@<T113_IP>:/tmp/"
echo "  scp $SRC_DIR/deploy/install.sh root@<T113_IP>:/tmp/"
echo "  ssh root@<T113_IP> 'cd /tmp && ./install.sh'"
