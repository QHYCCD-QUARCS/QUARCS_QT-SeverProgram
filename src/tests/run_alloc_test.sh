#!/usr/bin/env bash
# 纯设备分配决策的离线单元测试（方案 A：无 gtest / 无 INDI/SDK / 无硬件 / 无 GUI）。
# 用法：bash src/tests/run_alloc_test.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
OUT="$(mktemp -d)/alloc_test"
if pkg-config --exists Qt5Core; then
  CF="$(pkg-config --cflags Qt5Core)"; LB="$(pkg-config --libs Qt5Core)"
elif pkg-config --exists Qt6Core; then
  CF="$(pkg-config --cflags Qt6Core)"; LB="$(pkg-config --libs Qt6Core)"
else
  echo "ERROR: 找不到 Qt5Core/Qt6Core 的 pkg-config"; exit 2
fi
g++ -std=c++17 -fPIC $CF "$SRC/device_allocation.cpp" "$SRC/tests/alloc_test.cpp" $LB -o "$OUT"
"$OUT"
