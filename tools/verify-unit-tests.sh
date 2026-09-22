#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-tests}"
BUILD_JOBS="${BUILD_JOBS:-2}"
EVIDENCE_DIR="${EVIDENCE_DIR:-$ROOT/build-test-evidence}"
MODE="${1:-all}"

cd "$ROOT"
mkdir -p "$EVIDENCE_DIR"

expected_glm_head() {
  git ls-tree HEAD extern/glm | awk '{print $3}'
}

record_identity() {
  local expected actual
  expected="$(expected_glm_head)"
  actual="$(git -C extern/glm rev-parse HEAD)"
  printf 'mcvr_head=%s\n' "$(git rev-parse HEAD)"
  printf 'glm_gitlink=%s\n' "$expected"
  printf 'glm_materialized=%s\n' "$actual"
  test -n "$expected"
  test "$actual" = "$expected"
  test -f extern/glm/glm/glm.hpp
}

acquire() {
  echo 'phase=ACQUISITION'
  echo "glm_gitlink=$(expected_glm_head)"
  git submodule update --init --depth 1 extern/glm
  record_identity
  echo 'phase_result=ACQUISITION_PASS'
}

verify_local() {
  echo 'phase=OFFLINE_VERIFY'
  record_identity
  rm -rf "$BUILD_DIR"
  echo "build_dir=$BUILD_DIR"
  echo "build_jobs=$BUILD_JOBS"
  cmake --version | head -n 1
  c++ --version | head -n 1
  git submodule status extern/glm
  cmake -S tests -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"
  ctest --test-dir "$BUILD_DIR" --output-on-failure --verbose
  echo 'phase_result=OFFLINE_VERIFY_PASS'
}

verify_offline() {
  echo 'phase=OFFLINE_BOUNDARY'
  record_identity
  if ! command -v unshare >/dev/null 2>&1; then
    echo 'phase_result=OFFLINE_BOUNDARY_UNAVAILABLE' >&2
    return 90
  fi

  # Prefer an unprivileged user+network namespace. GitHub-hosted Ubuntu blocks
  # that path, but grants passwordless sudo; in that case create only the
  # network namespace as root and immediately drop back to the invoking uid/gid
  # before the proof-bearing command. Both paths execute verify-local with no
  # host network interface or route.
  if unshare --user --map-root-user --net true 2>/dev/null; then
    echo 'network_boundary=user+net-namespace'
    exec unshare --user --map-root-user --net bash "$0" verify-local
  fi

  if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
    echo 'network_boundary=sudo-net-namespace+uid-drop'
    exec sudo -n unshare --net -- runuser -u "$(id -un)" -- \
      env BUILD_DIR="$BUILD_DIR" BUILD_JOBS="$BUILD_JOBS" EVIDENCE_DIR="$EVIDENCE_DIR" \
      LC_ALL="${LC_ALL:-C.UTF-8}" LANG="${LANG:-C.UTF-8}" TZ="${TZ:-UTC}" \
      bash "$0" verify-local
  fi

  echo 'phase_result=OFFLINE_BOUNDARY_UNAVAILABLE' >&2
  return 90
}

case "$MODE" in
  acquire)
    acquire 2>&1 | tee "$EVIDENCE_DIR/acquisition.log"
    ;;
  verify-offline)
    verify_offline 2>&1 | tee "$EVIDENCE_DIR/offline-verification.log"
    ;;
  verify-local)
    verify_local
    ;;
  all)
    acquire 2>&1 | tee "$EVIDENCE_DIR/acquisition.log"
    verify_offline 2>&1 | tee "$EVIDENCE_DIR/offline-verification.log"
    ;;
  *)
    echo "usage: $0 [all|acquire|verify-offline]" >&2
    exit 64
    ;;
esac
