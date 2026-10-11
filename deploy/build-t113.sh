#!/bin/sh
#
# 构建 Accessterminal_T113（门口机）—— T113 交叉编译，带**预检**与**产物核验**
#
# 用法（与 SDK 自带的 environment.sh 直接配套，推荐）：
#   source "/path/to/交叉编译工具链/environment.sh"
#   ./deploy/build-t113.sh
#
# 也接受显式变量（任一来源都行，CMake 变量优先）：
#   T113_SYSROOT         目标平台 sysroot（必填）
#   T113_CROSS_COMPILE   形如 .../bin/arm-openwrt-linux-（SDK 的 environment.sh 风格）
#   T113_TOOLCHAIN_DIR   或直接给工具链目录（其下含 bin/）
#   OPENCV_ROOT          ARM OpenCV 根目录（含 cmake/opencv4 或 lib/cmake/opencv4）
#   GPIOD_ROOT/GPIO_ROOT libgpiod + ALSA 根目录（含 include/ 与 lib/）
#
# 为什么要有预检（第 40 轮第一次真正交叉编译的教训）
# ------------------------------------------------
# 那次实际卡住的四处**全都不是工程代码**，而是环境与路径约定：
#   1. T113_TOOLCHAIN_DIR 被多拼了一层 /bin（SDK 导出的是 bin 目录本身）；
#   2. T113_SYSROOT 只认 CMake 变量，而 CMake 的编译器 ABI 探测会再包含一次
#      工具链文件，嵌套项目拿不到 -D 缓存变量 → FATAL_ERROR；
#   3. SDK 用环境变量导出 OPENCV_ROOT/GPIOD_ROOT（还差一个字母 D），工具链
#      文件只读 CMake 变量 → OpenCV 找不到；
#   4. 板端 sysroot 的 musl 有 getentropy，而工具链自带的 libc 桩没有，
#      默认搜索顺序命中了后者 → 编译全过、链接报 undefined reference。
# 它们的共同点是：**报错出现在几十行 CMake 输出里，看起来像代码问题**。
# 预检把它们变成编译前的一句人话；产物核验则防止"编译过了但板端跑不起来"。
#
set -e

SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$SRC_DIR/build-t113}"

die() {
    echo "" >&2
    echo "错误: $1" >&2
    shift
    for line in "$@"; do echo "      $line" >&2; done
    exit 1
}

echo "=============================================================="
echo " Accessterminal_T113 交叉编译（T113 / Tina Linux / musl）"
echo "=============================================================="
echo "源码目录: $SRC_DIR"
echo "构建目录: $BUILD_DIR"
echo ""

# ---------------------------------------------------------------------------
# 一、预检
# ---------------------------------------------------------------------------
echo "---- 预检 ----"

# 1) sysroot
[ -n "$T113_SYSROOT" ] || die "必须设置 T113_SYSROOT 指向目标平台 sysroot" \
    "推荐：source <SDK>/交叉编译工具链/environment.sh（它会导出该变量）" \
    "或：  export T113_SYSROOT=/path/to/tina/out/<board>/staging_dir/target"
[ -d "$T113_SYSROOT" ] || die "T113_SYSROOT 不是目录: $T113_SYSROOT"
[ -e "$T113_SYSROOT/lib/libc.so" ] || die "sysroot 里没有 lib/libc.so，路径可能给错了" \
    "当前: $T113_SYSROOT"
echo "  [OK] sysroot: $T113_SYSROOT"

# 2) 交叉编译器（必须真的是 ARM/musl，而不是宿主的 g++）
CXX=""
if [ -n "$T113_CROSS_COMPILE" ]; then
    CXX="${T113_CROSS_COMPILE}g++"
    [ -x "$CXX" ] || CXX="${T113_CROSS_COMPILE}gcc"
elif [ -n "$T113_TOOLCHAIN_DIR" ]; then
    case "$T113_TOOLCHAIN_DIR" in
        */bin) _bin="$T113_TOOLCHAIN_DIR" ;;
        *)     _bin="$T113_TOOLCHAIN_DIR/bin" ;;
    esac
    for _p in arm-openwrt-linux-muslgnueabi arm-openwrt-linux; do
        [ -x "$_bin/$_p-g++" ] && CXX="$_bin/$_p-g++" && break
    done
else
    CXX="$(command -v arm-openwrt-linux-muslgnueabi-g++ 2>/dev/null || true)"
fi
[ -n "$CXX" ] && [ -x "$CXX" ] || die "找不到 ARM 交叉编译器 g++" \
    "请 source SDK 的 environment.sh，或设置 T113_CROSS_COMPILE / T113_TOOLCHAIN_DIR"
_machine="$("$CXX" -dumpmachine 2>/dev/null || echo '?')"
case "$_machine" in
    arm*musl*) : ;;
    arm*)      echo "  [警告] 编译器目标是 $_machine（非 musl 变体），SDK 自带 glibc 版本，通常不应使用" ;;
    *)         die "编译器目标不是 ARM: $_machine" "路径: $CXX" ;;
esac
echo "  [OK] 编译器: $CXX ($_machine)"

# 3) Qt5（Tina SDK 装在 sysroot 内的独立前缀里，不是 sysroot/usr）
_qt_prefix=""
if [ -n "$T113_QMAKE_SPEC" ]; then
    _qt_prefix="$(dirname "$(dirname "$T113_QMAKE_SPEC")")"   # .../arm-qt
fi
[ -n "$_qt_prefix" ] || _qt_prefix="$T113_SYSROOT/usr/lib/arm-qt"
if [ ! -f "$_qt_prefix/lib/cmake/Qt5/Qt5Config.cmake" ]; then
    _found="$(find "$T113_SYSROOT" -maxdepth 6 -name Qt5Config.cmake 2>/dev/null | head -1)"
    [ -n "$_found" ] || die "sysroot 里找不到 Qt5 的 CMake 配置（Qt5Config.cmake）" \
        "已查: $_qt_prefix 以及 $T113_SYSROOT 下 6 层"
    _qt_prefix="$(dirname "$(dirname "$(dirname "$_found")")")"
fi
echo "  [OK] Qt5: $_qt_prefix"

# 4) 三方库（OpenCV / libgpiod + ALSA）
_ocv="${OPENCV_ROOT:-}"
[ -n "$_ocv" ] || die "必须设置 OPENCV_ROOT 指向 ARM OpenCV 根目录" \
    "SDK 的 environment.sh 会导出它（本机示例: <sdk>/opencv）"
if [ ! -f "$_ocv/cmake/opencv4/OpenCVConfig.cmake" ] && \
   [ ! -f "$_ocv/lib/cmake/opencv4/OpenCVConfig.cmake" ]; then
    die "在 $OPENCV_ROOT 下找不到 cmake/opencv4 或 lib/cmake/opencv4" \
        "目标平台需要 OpenCV 4.x（含 face 模块，LBPH 在 opencv2/face.hpp）"
fi
echo "  [OK] OpenCV: $_ocv"

_gpio="${GPIO_ROOT:-${GPIOD_ROOT:-}}"
[ -n "$_gpio" ] || die "必须设置 GPIO_ROOT（或 GPIOD_ROOT）指向 libgpiod/ALSA 根目录"
{ [ -f "$_gpio/lib/libgpiod.a" ] || [ -f "$_gpio/lib/libgpiod.so" ]; } || \
    die "在 $_gpio/lib 下找不到 libgpiod.a / libgpiod.so"
[ -f "$_gpio/include/gpiod.h" ] || die "在 $_gpio/include 下找不到 gpiod.h"
[ -f "$_gpio/include/asoundlib.h" ] || die "在 $_gpio/include 下找不到 asoundlib.h（ALSA 头）"
[ -f "$_gpio/lib/libasound.so" ] || die "在 $_gpio/lib 下找不到 libasound.so"
echo "  [OK] libgpiod + ALSA: $_gpio"

# 5) 非 ASCII 路径提示（Qt 5.12 的 moc/qmake 对非 ASCII 路径有过已知问题）
case "$T113_SYSROOT$_ocv" in
    *[!\ -~]*) echo "  [警告] SDK 路径含非 ASCII 字符；若 moc/uic 报奇怪的路径错误，" \
                      "可把 SDK 软链到纯 ASCII 路径（如 \$HOME/t113-sdk）后重试" ;;
esac
echo "  [OK] 预检全部通过"
echo ""

# ---------------------------------------------------------------------------
# 二、配置与编译
# ---------------------------------------------------------------------------
echo "---- 配置 ----"
# STAGING_DIR：工具链包装脚本会读它（不设只是警告，设上更安静也更明确）
STAGING_DIR="${STAGING_DIR:-$T113_SYSROOT}"
export STAGING_DIR

# 交叉编译默认关闭测试（ARM 二进制无法在 x86 主机上运行）
cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$SRC_DIR/cmake/toolchain-t113.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DACCESSTERMINAL_BUILD_TESTS=OFF \
    -DACCESSTERMINAL_STRIP=ON

echo ""
echo "---- 编译 ----"
JOBS="$(nproc 2>/dev/null || echo 4)"
cmake --build "$BUILD_DIR" -j "$JOBS"

BIN="$BUILD_DIR/bin/smartpro"
[ -f "$BIN" ] || die "构建结束但找不到产物: $BIN"
echo ""

# ---------------------------------------------------------------------------
# 三、产物核验（编译过 ≠ 板端跑得起来）
# ---------------------------------------------------------------------------
echo "---- 产物核验 ----"

if command -v file >/dev/null 2>&1; then
    _what="$(file -b "$BIN")"
    case "$_what" in
        *"ELF 32-bit"*ARM*) echo "  [OK] 架构: $_what" ;;
        *) die "产物不是 32 位 ARM ELF: $_what" ;;
    esac
fi

if command -v readelf >/dev/null 2>&1; then
    _interp="$(readelf -l "$BIN" 2>/dev/null | sed -n 's/.*program interpreter: \(.*\)\]/\1/p' | head -1)"
    echo "  [OK] 解释器: ${_interp:-未知}"
    case "$_interp" in
        *musl*) : ;;
        *) echo "  [警告] 解释器不是 musl（SDK 要求用 musl 变体工具链）" ;;
    esac

    # 依赖闭包：每个 DT_NEEDED 都必须能在 sysroot（从板端导出）里找到
    _missing=0
    _total=0
    for _so in $(readelf -d "$BIN" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'); do
        _total=$((_total + 1))
        _hit="$(find "$T113_SYSROOT" -name "$_so" 2>/dev/null | head -1)"
        if [ -z "$_hit" ]; then
            echo "  [缺失] $_so —— 板端 sysroot 里没有，运行时会起不来"
            _missing=$((_missing + 1))
        fi
    done
    if [ "$_missing" -eq 0 ]; then
        echo "  [OK] 依赖闭包: $_total/$_total 均可在目标 sysroot 中解析"
    else
        die "有 $_missing 个依赖在目标 sysroot 中缺失（见上）"
    fi
fi

echo ""
echo "=== 构建完成 ==="
ls -l "$BIN" 2>/dev/null || true
echo ""
echo "推送与部署（板端已联网时）："
echo "  scp $BIN root@<T113_IP>:/tmp/"
echo "  scp $SRC_DIR/config/appconfig.json root@<T113_IP>:/tmp/"
echo "  scp $SRC_DIR/deploy/install.sh root@<T113_IP>:/tmp/"
echo "  ssh root@<T113_IP> 'cd /tmp && ./install.sh'"
echo ""
echo "注意：仍有一件事本机证明不了 —— 在真实 T113 板子上运行（ALSA/GPIO/V4L2 行为、"
echo "      Qt 渲染后端、串口设备节点权限）。见 docs/交接与继续开发.md 的【P0】。"
