#!/usr/bin/env bash
# 一键产出产线代码覆盖率（T113 / 门口机）。
#
# 为什么需要它：第 24 轮之前，本工程从没量化过"测试到底执行了多少产线代码"。
# 补上后才发现"94.4%"只统计被链接进测试的 17 个文件，另有 20 个产线文件
# （约 2093 行）从未被测，真实值约 22%。
#
# 本机只有 gcov（无 lcov/gcovr/genhtml），故直接解析 `gcov -j -t` 的 JSON v2。
#
# 用法：bash tools/run_coverage.sh [输出 json 路径]
# 说明：需要 msys64 的 mingw64 工具链（g++/gcov/cmake/ninja）。
set -u
export PATH=/mingw64/bin:/usr/bin:$PATH

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
REPO_W="$(cd "$REPO" && pwd -W 2>/dev/null || echo "$REPO")"
PY="${PYTHON:-python}"
JSON_OUT="${1:-}"

# 构建目录放在 Windows 可见的位置：MSYS 的 /tmp 与 Windows 侧不是同一处，
# 混用会导致"构建成功但可执行文件找不到"（第 24 轮踩过）。
BUILD_TESTS="$(mktemp -d)"
BUILD_INTEROP="$(mktemp -d)"
echo "构建目录: $BUILD_TESTS"
echo "          $BUILD_INTEROP"

echo "=== 1/4 配置（--coverage -O0 -g）==="
cmake -S "$REPO/tests" -B "$BUILD_TESTS" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="--coverage -O0 -g" \
      -DCMAKE_EXE_LINKER_FLAGS="--coverage" > /dev/null || exit 1

echo "=== 2/4 构建 ==="
cmake --build "$BUILD_TESTS" > /dev/null || exit 1

echo "=== 3/4 运行单元测试 ==="
ctest --test-dir "$BUILD_TESTS" > /dev/null 2>&1

# 互操作测试是**独立 CMake 工程**，链接了 core/net/signaling_client.cpp。
# 只扫 tests/ 的构建目录会把它误判成"从未被测"。
echo "=== 4/4 运行互操作测试（跨仓库真实 TCP）==="
DESKTOP="${DESKTOP_REPO:-$(cd "$REPO/../Accessterminal_desktop" 2>/dev/null && pwd -W || true)}"
if [ -n "${DESKTOP:-}" ] && [ -d "$DESKTOP" ]; then
  cmake -S "$REPO/tests/interop" -B "$BUILD_INTEROP" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DT113_REPO="$REPO_W" -DDESKTOP_REPO="$DESKTOP" \
        -DCMAKE_CXX_FLAGS="--coverage -O0 -g" \
        -DCMAKE_EXE_LINKER_FLAGS="--coverage" > /dev/null 2>&1 \
    && cmake --build "$BUILD_INTEROP" > /dev/null 2>&1 \
    && ctest --test-dir "$BUILD_INTEROP" > /dev/null 2>&1 \
    && echo "  互操作已计入" || echo "  互操作构建/运行失败，已跳过（覆盖率会偏低）"
else
  echo "  未找到桌面仓库，跳过互操作（signaling_client.cpp 会被误判为未测）"
fi

echo
if [ -n "$JSON_OUT" ]; then
  "$PY" "$REPO_W/tools/coverage_report.py" "$REPO_W" \
        --build "$BUILD_TESTS" --build "$BUILD_INTEROP" --json "$JSON_OUT"
else
  "$PY" "$REPO_W/tools/coverage_report.py" "$REPO_W" \
        --build "$BUILD_TESTS" --build "$BUILD_INTEROP"
fi
RC=$?

echo
echo "构建目录保留在: $BUILD_TESTS"
echo "                $BUILD_INTEROP"
echo "（如需清理请手动删除）"
exit $RC
