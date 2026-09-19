#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-tests}"
BUILD_JOBS="${BUILD_JOBS:-2}"

cd "$ROOT"

# This test layer needs only the pinned glm gitlink, not the renderer's full recursive dependency tree.
git submodule update --init --depth 1 extern/glm

test -f extern/glm/glm/glm.hpp

rm -rf "$BUILD_DIR"

echo "mcvr_head=$(git rev-parse HEAD)"
echo "build_dir=$BUILD_DIR"
echo "build_jobs=$BUILD_JOBS"
cmake --version | head -n 1
c++ --version | head -n 1
git submodule status extern/glm

cmake -S tests -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure --verbose
