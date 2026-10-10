#!/usr/bin/env bash
# ============================================================================
# T113 工程“主机端编译验证”脚本
#
# 为什么需要它
# ------------
# T113 门口机运行在 Linux（全志 T113-S3 / Tina Linux）上，依赖三类开发机
# 上没有的东西：
#   - ALSA（Linux 专有音频接口）
#   - libgpiod（Linux 专有 GPIO 接口）
#   - 目标平台交叉编译工具链与 Qt5/OpenCV 的 ARM 版本
#
# 因此在本机做**完整**构建是不可能的。但工程 90% 的代码是平台无关的，
# 完全可以先做真实的编译（语法 + 语义 + 类型检查），把绝大多数低级错误
# ——类型不匹配、参数个数错误、拼写错误、缺失头文件、C++ 标准越界——
# 在交叉编译之前就拦下来。
#
# 做法
# ----
#   1. tools/devstubs/ 下提供按官方签名复刻的 ALSA / libgpiod / POSIX / V4L2
#      桩头文件，仅供类型检查，**不是实现**；
#   2. 用 Qt 的 uic 预生成 ui_*.h（AUTOUIC 只在 CMake 构建时才生成）；
#   3. 对 src/ 下全部 .cpp 逐个执行 g++ -fsyntax-only。
#
# 能发现什么 / 不能发现什么
# ------------------------
#   能：类型与签名错误、缺失声明/头文件、C++ 标准越界、拼写错误。
#   不能：链接错误、目标平台 ABI/宏差异、运行时行为。
# 所以本脚本**不能替代** Tina SDK 上的真实交叉编译，详见 README。
#
# 用法
# ----
#   bash tools/check_host_compile.sh              # 用默认 Qt 路径
#   QT_INC=/path/to/qt/include bash tools/check_host_compile.sh
#
# 依赖：一个带 Qt5 的 g++ 环境（MSYS2 + mingw-w64-x86_64-qt5 即可）。
# ============================================================================
set -u

# ---- 工具链可见性 ----
# Windows 上 MSYS2 的 mingw64 工具链不一定在默认 PATH 中；Linux 上通常已在。
# 不设置 PATH 会导致 g++ 找不到，进而每个文件都“失败”，且报错信息里没有
# error: 行，容易被误判为代码问题。
if [ -d /mingw64/bin ]; then
    case ":$PATH:" in
        *:/mingw64/bin:*) ;;
        *) PATH="/mingw64/bin:/usr/bin:$PATH" ;;
    esac
    export PATH
fi

for _tool in g++ uic; do
    if ! command -v "$_tool" >/dev/null 2>&1; then
        echo "错误：找不到 $_tool。请安装带 Qt5 的 g++ 工具链。" >&2
        echo "      Windows/MSYS2 可执行：" >&2
        echo "        pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-qt5-base \\" >&2
        echo "                  mingw-w64-x86_64-qt5-tools" >&2
        exit 1
    fi
done
unset _tool

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STUBS="$SCRIPT_DIR/devstubs"
OUT="${TMPDIR:-/tmp}/t113-host-check"
rm -rf "$OUT"; mkdir -p "$OUT/ui"

# ---- 定位 Qt 与 OpenCV ----
QT_INC="${QT_INC:-/mingw64/include}"
CV_INC="${CV_INC:-$QT_INC/opencv5}"
MOCS="${MOCS:-/mingw64/share/qt5/mkspecs/win32-g++}"

# ---- 定位 uic ----
UIC=""
for c in "${UIC:-}" /mingw64/bin/uic.exe /mingw64/bin/uic \
         /usr/lib/qt5/bin/uic /usr/bin/uic; do
    [ -n "$c" ] && [ -x "$c" ] && UIC="$c" && break
done
if [ -z "$UIC" ]; then
    echo "错误：未找到 uic。请安装 Qt5 工具（如 mingw-w64-x86_64-qt5-tools）。" >&2
    exit 1
fi

echo "=== 生成 ui_*.h ==="
for ui in "$SRC_ROOT"/src/ui/*.ui; do
    [ -e "$ui" ] || continue
    base="$(basename "$ui" .ui)"
    if "$UIC" "$ui" -o "$OUT/ui/ui_$base.h" 2>"$OUT/uic-$base.log"; then
        echo "  ui_$base.h"
    else
        echo "  ui_$base.h 失败:"; cat "$OUT/uic-$base.log"
        exit 1
    fi
done

# ---- OpenCV 主版本适配（只影响本机检查，不改工程源码）----
# 工程按目标平台包含 <opencv2/objdetect.hpp>：OpenCV 4.x 在此声明
# cv::CascadeClassifier。若本机是 5.x，该类被移到 xobjdetect.hpp，
# 故仅对检查命令追加强制包含。
CV_COMPAT=""
if [ -f "$CV_INC/opencv2/objdetect.hpp" ] && \
   ! grep -qE "class .*CascadeClassifier" "$CV_INC/opencv2/objdetect.hpp" 2>/dev/null; then
    if [ -f "$CV_INC/opencv2/xobjdetect.hpp" ]; then
        CV_COMPAT="-include opencv2/xobjdetect.hpp"
        echo ""
        echo "注：本机 OpenCV 为 5.x，已追加 -include opencv2/xobjdetect.hpp"
        echo "    仅用于检查；目标平台 4.x 由 objdetect.hpp 直接提供。"
    fi
fi

CXXFLAGS="-std=c++11 -fsyntax-only -Wall -Wextra -Wno-unused-parameter \
 -I$SRC_ROOT/src -I$OUT/ui -I$STUBS \
 -include host_compat.h \
 -isystem $QT_INC/QtCore -isystem $QT_INC/QtGui -isystem $QT_INC/QtWidgets \
 -isystem $QT_INC/QtNetwork -isystem $QT_INC/QtSerialPort -isystem $QT_INC/QtConcurrent \
 -isystem $CV_INC -isystem $MOCS \
 -DQT_CORE_LIB -DQT_GUI_LIB -DQT_WIDGETS_LIB -DQT_NETWORK_LIB \
 -DQT_SERIALPORT_LIB -DQT_CONCURRENT_LIB $CV_COMPAT"

ALL_CPP="$(cd "$SRC_ROOT" && find src -name '*.cpp' | sort)"

echo ""
echo "================ T113 主机端编译验证 ================"
echo "源文件数: $(echo "$ALL_CPP" | wc -l)"
echo ""

PASS=0; FAIL=0
for f in $ALL_CPP; do
    err="$(cd "$SRC_ROOT" && g++ $CXXFLAGS "$f" 2>&1)"
    rc=$?
    if [ $rc -eq 0 ]; then
        PASS=$((PASS+1)); printf "  [OK]   %s\n" "$f"
    else
        FAIL=$((FAIL+1)); printf "  [FAIL] %s\n" "$f"
        # 优先只显示 error: 行；若一行都没有（例如工具链缺失、桩头文件
        # 语法错误），则回退显示原始输出，避免“静默失败”难以定位。
        shown="$(echo "$err" | grep -E "error:" | head -12)"
        if [ -n "$shown" ]; then
            echo "$shown" | sed 's/^/         /'
        else
            echo "$err" | head -12 | sed 's/^/         /'
        fi
    fi
done

echo ""
echo "======================================================"
echo "通过: $PASS   失败: $FAIL"
if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "提示：若报错来自桩头文件与真实头文件的差异（而非工程代码），"
    echo "      请同步修正 tools/devstubs/ 下对应桩的签名。"
    exit 1
fi
echo "全部通过（含 Qt5、OpenCV、ALSA、libgpiod 的接口使用）"
echo ""
echo "注意：这是主机端类型检查，未做链接，也不能替代 Tina SDK 交叉编译。"
