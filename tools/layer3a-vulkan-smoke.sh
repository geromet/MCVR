#!/usr/bin/env bash
# MCVR Layer-3A LavaPipe Vulkan runtime smoke (#501).
#
# Repo-local entrypoint. Establishes a RuntimeEnvironmentBasis (sealed userspace
# component closure + HostExecutionBasis), starts an attempt-owned Xvfb endpoint,
# runs the smoke under a minimal allowlisted environment, and exercises the
# negative controls that must fail closed.
#
# Usage:
#   bash tools/layer3a-vulkan-smoke.sh            # positive proof run
#   bash tools/layer3a-vulkan-smoke.sh --verify   # positive run + negative matrix
#
# Nothing here is root-capable: the script refuses to run as uid 0, because the
# Vulkan loader ignores driver/layer selection variables under elevated
# privilege, which would invalidate the software-ICD proof.
#
# Environment:
#   MCVR_LAYER3A_WORK        build/evidence root (default <repo>/build-layer3a)
#   MCVR_LAYER3A_XVFB_BIN    X server binary override (recorded in the basis)
#   MCVR_LAYER3A_ALLOW_DIRTY set to 1 to run against a dirty worktree; the
#                            resulting basis is marked dirty and is not proof grade
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${MCVR_LAYER3A_WORK:-$ROOT/build-layer3a}"
MODE="positive"
[ "${1:-}" = "--verify" ] && MODE="verify"

WIDTH=640
HEIGHT=480
DEPTH=24
BUILD_JOBS="${BUILD_JOBS:-2}"
SMOKE_TIMEOUT="${SMOKE_TIMEOUT:-90}"

log() { echo "LAYER3A | $*"; }
fail() { echo "LAYER3A_FAIL | $*" >&2; exit 1; }

absolute() { readlink -f "$1"; }

hash_file() { sha256sum "$1" | awk '{print $1}'; }

# ---------------------------------------------------------------------------
# Preconditions: elevation, execution overrides, canonical thread policy
# ---------------------------------------------------------------------------

assert_not_elevated() {
    [ "$(id -u)" -ne 0 ] || fail "running as uid 0: the Vulkan loader ignores driver/layer selection when elevated"
}

assert_execution_overrides_absent() {
    local name
    for name in LP_NATIVE_VECTOR_WIDTH GALLIUM_OVERRIDE_CPU_CAPS LP_DEBUG LP_PERF LP_NO_RAST \
        GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE; do
        if [ -n "${!name:-}" ]; then
            fail "forbidden LLVMpipe/Gallium execution override is set: $name=${!name}"
        fi
    done
}

assert_dynamic_link_clean() {
    local name
    for name in LD_PRELOAD LD_LIBRARY_PATH LD_AUDIT; do
        if [ -n "${!name:-}" ]; then
            fail "dynamic-link override is set: $name=${!name}"
        fi
    done
}

assert_canonical_thread_count() {
    if [ -n "${LP_NUM_THREADS:-}" ] && [ "${LP_NUM_THREADS}" != "1" ]; then
        fail "LP_NUM_THREADS must be canonical 1, observed ${LP_NUM_THREADS}"
    fi
}

assert_not_elevated
assert_dynamic_link_clean
assert_execution_overrides_absent
assert_canonical_thread_count

for tool in cmake sha256sum xauth xdpyinfo timeout python3 ldd; do
    command -v "$tool" >/dev/null 2>&1 || fail "required tool missing: $tool"
done
XVFB_BIN="${MCVR_LAYER3A_XVFB_BIN:-$(command -v Xvfb || true)}"
[ -n "$XVFB_BIN" ] && [ -x "$XVFB_BIN" ] || fail "Xvfb not found (set MCVR_LAYER3A_XVFB_BIN only for local dry runs)"

# ---------------------------------------------------------------------------
# Source state and attempt directory
# ---------------------------------------------------------------------------

cd "$ROOT"
SOURCE_HEAD="$(git rev-parse HEAD)"
SOURCE_DIRTY=false
if [ -n "$(git status --porcelain)" ]; then
    SOURCE_DIRTY=true
    [ "${MCVR_LAYER3A_ALLOW_DIRTY:-0}" = "1" ] ||
        fail "worktree is dirty; proof-grade runs require an exact source head (set MCVR_LAYER3A_ALLOW_DIRTY=1 for a dry run)"
fi

mkdir -p "$WORK"
ATTEMPT="$WORK/attempt-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "$ATTEMPT"/{home,logs,evidence,tmp,xdg/config,xdg/data,xdg/cache,xdg/state}
SMOKE_LOG="$ATTEMPT/logs"
STEPS="$ATTEMPT/evidence/steps.tsv"
: >"$STEPS"

XVFB_PID=""
cleanup() {
    if [ -n "$XVFB_PID" ] && kill -0 "$XVFB_PID" 2>/dev/null; then
        kill "$XVFB_PID" 2>/dev/null || true
        wait "$XVFB_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

log "building Layer-3A smoke targets"
BUILD_DIR="$WORK/build"
cmake -S "$ROOT/tests/layer3a" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >"$SMOKE_LOG/cmake-configure.log" 2>&1 ||
    fail "cmake configure failed (see $SMOKE_LOG/cmake-configure.log)"
cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS" >"$SMOKE_LOG/cmake-build.log" 2>&1 ||
    fail "cmake build failed (see $SMOKE_LOG/cmake-build.log)"
SMOKE="$BUILD_DIR/layer3a_smoke"
PRODUCTION_POLICY="$BUILD_DIR/layer3a_production_policy"
[ -x "$SMOKE" ] && [ -x "$PRODUCTION_POLICY" ] || fail "smoke binaries were not produced"

# ---------------------------------------------------------------------------
# Runtime component closure (userspace side of the basis)
# ---------------------------------------------------------------------------

log "resolving runtime basis"

ICD_MANIFEST=""
for candidate in /usr/share/vulkan/icd.d/lvp_icd.x86_64.json /usr/share/vulkan/icd.d/lvp_icd.json; do
    if [ -f "$candidate" ]; then
        ICD_MANIFEST="$candidate"
        break
    fi
done
[ -n "$ICD_MANIFEST" ] || fail "LavaPipe ICD manifest not found (install mesa-vulkan-drivers)"
ICD_MANIFEST="$(absolute "$ICD_MANIFEST")"
ICD_LIBRARY="$(python3 - "$ICD_MANIFEST" <<'PY'
import json, sys
manifest = json.load(open(sys.argv[1]))
print(manifest["ICD"]["library_path"])
PY
)"
[ -f "$ICD_LIBRARY" ] || fail "LavaPipe ICD library does not exist: $ICD_LIBRARY"
ICD_LIBRARY="$(absolute "$ICD_LIBRARY")"

VALIDATION_MANIFEST=""
for candidate in /usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json \
    /usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json; do
    if [ -f "$candidate" ]; then
        VALIDATION_MANIFEST="$candidate"
        break
    fi
done
[ -n "$VALIDATION_MANIFEST" ] || fail "Khronos validation layer manifest not found (install vulkan-validationlayers)"
VALIDATION_MANIFEST="$(absolute "$VALIDATION_MANIFEST")"
VALIDATION_LIBRARY="$(python3 - "$VALIDATION_MANIFEST" <<'PY'
import json, os, sys
manifest = json.load(open(sys.argv[1]))
path = manifest["layer"]["library_path"]
if not os.path.isabs(path):
    path = os.path.join(os.path.dirname(sys.argv[1]), path)
print(path)
PY
)"
VALIDATION_LIBRARY="$(absolute "$VALIDATION_LIBRARY")"
[ -f "$VALIDATION_LIBRARY" ] || fail "validation layer library does not exist: $VALIDATION_LIBRARY"

VULKAN_LOADER="$(ldd "$SMOKE" | awk '/libvulkan\.so/ {print $3; exit}')"
[ -n "$VULKAN_LOADER" ] || fail "smoke binary is not linked against the Vulkan loader"
VULKAN_LOADER="$(absolute "$VULKAN_LOADER")"
GLFW_LIBRARY="$(ldd "$SMOKE" | awk '/libglfw\.so/ {print $3; exit}')"
[ -n "$GLFW_LIBRARY" ] || fail "smoke binary is not linked against GLFW"
GLFW_LIBRARY="$(absolute "$GLFW_LIBRARY")"
XVFB_REAL="$(absolute "$XVFB_BIN")"

# ---------------------------------------------------------------------------
# HostExecutionBasis: the substrate that executes LLVMpipe
# ---------------------------------------------------------------------------

log "recording host execution basis"
CPU_MODEL="$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)"
CPU_VENDOR="$(awk -F': ' '/^vendor_id/ {print $2; exit}' /proc/cpuinfo)"
CPU_FAMILY="$(awk -F': ' '/^cpu family/ {print $2; exit}' /proc/cpuinfo)"
CPU_STEPPING="$(awk -F': ' '/^stepping/ {print $2; exit}' /proc/cpuinfo)"
CPU_MICROCODE="$(awk -F': ' '/^microcode/ {print $2; exit}' /proc/cpuinfo)"
CPU_FLAGS_NORMALIZED="$(awk -F': ' '/^flags/ {print $2; exit}' /proc/cpuinfo | tr ' ' '\n' | sort -u | tr '\n' ' ')"
CPU_FLAGS_SHA="$(printf '%s' "$CPU_FLAGS_NORMALIZED" | sha256sum | awk '{print $1}')"
NPROC="$(nproc)"
CPU_AFFINITY="$(taskset -pc $$ 2>/dev/null | sed 's/^[^:]*: //' || echo unknown)"
CGROUP_CPUSET="$(cat /sys/fs/cgroup/cpuset.cpus.effective 2>/dev/null || cat /sys/fs/cgroup/cpuset/cpuset.cpus 2>/dev/null || echo unknown)"
KERNEL_RELEASE="$(uname -r)"
KERNEL_BUILD="$(uname -v)"
KERNEL_VERSION_SHA="$(hash_file /proc/version)"
ARCH="$(uname -m)"

# ---------------------------------------------------------------------------
# Vulkan loader control plane normalization
# ---------------------------------------------------------------------------

# Ambient per-user loader settings must not participate. The attempt owns HOME
# and XDG roots, and we verify no loader settings file exists in them.
VK_LOADER_SETTINGS_HITS="$(find "$ATTEMPT/home" "$ATTEMPT/xdg" -name 'vk_loader_settings.json' 2>/dev/null | wc -l)"
[ "$VK_LOADER_SETTINGS_HITS" -eq 0 ] || fail "attempt-owned loader settings directory unexpectedly contains vk_loader_settings.json"

# Attempt-owned X11 endpoint.
DISPLAY_NUM=""
for candidate in $(shuf -i 90-199 -n 20); do
    if [ ! -e "/tmp/.X11-unix/X$candidate" ]; then
        DISPLAY_NUM="$candidate"
        break
    fi
done
[ -n "$DISPLAY_NUM" ] || fail "no free X display number available"
XAUTH_FILE="$ATTEMPT/Xauthority"
COOKIE="$(python3 -c 'import secrets; print(secrets.token_hex(16))')"
xauth -f "$XAUTH_FILE" add ":$DISPLAY_NUM" . "$COOKIE" >/dev/null 2>&1 || fail "xauth cookie creation failed"

log "starting attempt-owned X server on :$DISPLAY_NUM"
"$XVFB_BIN" ":$DISPLAY_NUM" -screen 0 "${WIDTH}x${HEIGHT}x${DEPTH}" -nolisten tcp -auth "$XAUTH_FILE" -noreset \
    >"$SMOKE_LOG/xserver.log" 2>&1 &
XVFB_PID=$!

ready=false
for _ in $(seq 1 100); do
    if [ -e "/tmp/.X11-unix/X$DISPLAY_NUM" ] &&
        env -i PATH="$PATH" DISPLAY=":$DISPLAY_NUM" XAUTHORITY="$XAUTH_FILE" xdpyinfo >/dev/null 2>&1; then
        ready=true
        break
    fi
    if ! kill -0 "$XVFB_PID" 2>/dev/null; then
        cat "$SMOKE_LOG/xserver.log" >&2 || true
        fail "X server exited during startup"
    fi
    sleep 0.1
done
[ "$ready" = true ] || fail "attempt-owned X server did not become ready"

# The endpoint must require the attempt-owned cookie: an unauthenticated
# connection to the same display must fail.
X_EMPTY_AUTH="$ATTEMPT/empty-Xauthority"
: >"$X_EMPTY_AUTH"
if env -i PATH="$PATH" DISPLAY=":$DISPLAY_NUM" XAUTHORITY="$X_EMPTY_AUTH" xdpyinfo >/dev/null 2>&1; then
    fail "attempt-owned X server accepted a connection without the attempt cookie"
fi
X_SOCKET_INODE="$(stat -c '%i' "/tmp/.X11-unix/X$DISPLAY_NUM")"

# ---------------------------------------------------------------------------
# Write the basis (before the first Vulkan/GLFW effect)
# ---------------------------------------------------------------------------

BASIS_JSON="$ATTEMPT/evidence/basis.json"
export ATTEMPT SOURCE_HEAD SOURCE_DIRTY BASIS_JSON
export CPU_MODEL CPU_VENDOR CPU_FAMILY CPU_STEPPING CPU_MICROCODE CPU_FLAGS_SHA NPROC CPU_AFFINITY CGROUP_CPUSET \
    KERNEL_RELEASE KERNEL_BUILD KERNEL_VERSION_SHA ARCH
export ICD_MANIFEST ICD_LIBRARY VALIDATION_MANIFEST VALIDATION_LIBRARY VULKAN_LOADER GLFW_LIBRARY
export XVFB_REAL DISPLAY_NUM X_SOCKET_INODE XAUTH_FILE
export SMOKE PRODUCTION_POLICY ROOT BUILD_DIR

python3 - <<'PY'
import hashlib, json, os, platform, subprocess

def h(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()

def closure(binary):
    out = subprocess.run(["ldd", binary], capture_output=True, text=True).stdout
    libs = sorted({f for f in out.split() if f.startswith("/")})
    return {"libraries": libs, "sha256": hashlib.sha256("".join(f"{h(l)}  {l}\n" for l in libs if os.path.exists(l)).encode()).hexdigest()}

def sh(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout.strip()

env = os.environ
basis = {
    "basis_kind": "sealed-component-closure",
    "source": {
        "repository": "geromet/MCVR",
        "head": env["SOURCE_HEAD"],
        "dirty": env["SOURCE_DIRTY"] == "true",
        "submodules_policy": "not required for the Layer-3A test targets",
    },
    "host_execution_basis": {
        "os": "linux",
        "arch": env["ARCH"],
        "cpu_model": env["CPU_MODEL"],
        "cpu_vendor": env["CPU_VENDOR"],
        "cpu_family": env["CPU_FAMILY"],
        "cpu_stepping": env["CPU_STEPPING"],
        "cpu_microcode": env["CPU_MICROCODE"],
        "cpu_flags_sha256": env["CPU_FLAGS_SHA"],
        "nproc_visible": int(env["NPROC"]),
        "cpu_affinity": env["CPU_AFFINITY"],
        "cgroup_cpuset_effective": env["CGROUP_CPUSET"],
        "kernel_release": env["KERNEL_RELEASE"],
        "kernel_build": env["KERNEL_BUILD"],
        "proc_version_sha256": env["KERNEL_VERSION_SHA"],
        "lp_num_threads": 1,
        "forbidden_overrides": {
            "LP_NATIVE_VECTOR_WIDTH": "absent",
            "GALLIUM_OVERRIDE_CPU_CAPS": "absent",
            "LP_DEBUG": "absent",
            "LP_PERF": "absent",
            "LP_NO_RAST": "absent",
        },
    },
    "native_runtime_closure": {
        "mesa_lavapipe": {"icd_manifest": env["ICD_MANIFEST"], "icd_manifest_sha256": h(env["ICD_MANIFEST"]),
                          "icd_library": env["ICD_LIBRARY"], "icd_library_sha256": h(env["ICD_LIBRARY"])},
        "vulkan_loader": {"path": env["VULKAN_LOADER"], "sha256": h(env["VULKAN_LOADER"])},
        "validation_layer": {"manifest": env["VALIDATION_MANIFEST"], "manifest_sha256": h(env["VALIDATION_MANIFEST"]),
                             "library": env["VALIDATION_LIBRARY"], "library_sha256": h(env["VALIDATION_LIBRARY"])},
        "glfw": {"path": env["GLFW_LIBRARY"], "sha256": h(env["GLFW_LIBRARY"])},
        "x_server": {"path": env["XVFB_REAL"], "sha256": h(env["XVFB_REAL"]), "display": ":" + env["DISPLAY_NUM"],
                     "socket_inode": env["X_SOCKET_INODE"], "auth_required": True, "closure": closure(env["XVFB_REAL"])},
        "smoke_binary": {"path": env["SMOKE"], "sha256": h(env["SMOKE"])},
        "production_policy_binary": {"path": env["PRODUCTION_POLICY"], "sha256": h(env["PRODUCTION_POLICY"])},
        "linked_runtime_closure": closure(env["SMOKE"]),
    },
    "build_toolchain_basis": {
        "cmake": sh("cmake", "--version").splitlines()[0],
        "cxx": (sh(os.environ.get("CXX") or "c++", "--version").splitlines() or ["unknown"])[0],
    },
    "declared_loader_control_plane": {
        "VK_DRIVER_FILES": env["ICD_MANIFEST"],
        "VK_LOADER_LAYERS_DISABLE": "~implicit~",
        "VK_LAYER_SETTINGS_PATH": "absent",
        "VK_LAYER_PATH": "absent",
        "LD_PRELOAD": "absent",
        "LD_LIBRARY_PATH": "absent",
        "LP_NUM_THREADS": "1",
    },
}

canonical = json.dumps(basis, sort_keys=True, separators=(",", ":")).encode()
basis["basis_sha256"] = hashlib.sha256(canonical).hexdigest()
json.dump(basis, open(env["BASIS_JSON"], "w"), indent=2, sort_keys=True)
print(basis["basis_sha256"])
PY
BASIS_SHA="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["basis_sha256"])' "$BASIS_JSON")"
[ -n "$BASIS_SHA" ] || fail "basis digest was not produced"
log "runtime basis sha256: $BASIS_SHA"

# ---------------------------------------------------------------------------
# Child environment: minimal allowlist, attempt-owned HOME/XDG/display
# ---------------------------------------------------------------------------

CHILD_ENV=(
    env -i
    "PATH=$PATH"
    "HOME=$ATTEMPT/home"
    "XDG_CONFIG_HOME=$ATTEMPT/xdg/config"
    "XDG_DATA_HOME=$ATTEMPT/xdg/data"
    "XDG_CACHE_HOME=$ATTEMPT/xdg/cache"
    "XDG_STATE_HOME=$ATTEMPT/xdg/state"
    "TMPDIR=$ATTEMPT/tmp"
    "LC_ALL=C.UTF-8"
    "LANG=C.UTF-8"
    "TZ=UTC"
    "DISPLAY=:$DISPLAY_NUM"
    "XAUTHORITY=$XAUTH_FILE"
    "VK_DRIVER_FILES=$ICD_MANIFEST"
    "VK_LOADER_LAYERS_DISABLE=~implicit~"
    "LP_NUM_THREADS=1"
)

run_child() {
    local logfile="$1"
    shift
    timeout "$SMOKE_TIMEOUT" "${CHILD_ENV[@]}" "$@" >"$logfile" 2>&1
}

record() {
    # name | expected | actual-exit | ok
    printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >>"$STEPS"
}

positive_step() {
    local name="$1" output="$2"
    shift 2
    local rc=0
    run_child "$SMOKE_LOG/$name.log" "$@" || rc=$?
    if [ "$rc" -eq 0 ]; then
        record "$name" "exit 0" "$rc" "ok"
        return 0
    fi
    record "$name" "exit 0" "$rc" "FAILED"
    tail -n 5 "$SMOKE_LOG/$name.log" >&2 || true
    return 1
}

negative_step() {
    local name="$1" purpose="$2"
    shift 2
    local rc=0
    run_child "$SMOKE_LOG/$name.log" "$@" || rc=$?
    if [ "$rc" -ne 0 ]; then
        record "$name" "nonzero ($purpose)" "$rc" "ok"
        return 0
    fi
    record "$name" "nonzero ($purpose)" "$rc" "FAILED: unexpectedly succeeded"
    return 1
}

FAILED=0
positive_step "preflight" "$ATTEMPT/evidence/preflight.json" \
    "$SMOKE" --preflight --require-validation --allow-cpu --expect-device-name llvmpipe \
    --emit "$ATTEMPT/evidence/preflight.json" || FAILED=1
positive_step "smoke" "$ATTEMPT/evidence/smoke.json" \
    "$SMOKE" --smoke --require-validation --allow-cpu --expect-device-name llvmpipe \
    --emit "$ATTEMPT/evidence/smoke.json" || FAILED=1
positive_step "validation-canary" "$ATTEMPT/evidence/canary.json" \
    "$SMOKE" --canary --require-validation --allow-cpu --expect-device-name llvmpipe \
    --emit "$ATTEMPT/evidence/canary.json" || FAILED=1

# Production-policy negative: exit 3 means the CPU-only enumeration was rejected.
PROD_RC=0
run_child "$SMOKE_LOG/production-policy.log" "$PRODUCTION_POLICY" --prod-reject \
    --emit "$ATTEMPT/evidence/production-policy.json" || PROD_RC=$?
if [ "$PROD_RC" -eq 3 ]; then
    record "production-policy-rejects-cpu" "exit 3" "$PROD_RC" "ok"
else
    record "production-policy-rejects-cpu" "exit 3" "$PROD_RC" "FAILED"
    FAILED=1
fi

# Test-only admission is structurally absent from the production target.
negative_step "production-target-refuses-cpu-admission" "CPU admission not compiled in" \
    "$PRODUCTION_POLICY" --smoke --require-validation --allow-cpu \
    --emit "$ATTEMPT/evidence/production-cpu-refusal.json" || FAILED=1

if [ "$MODE" = "verify" ]; then
    cat >"$ATTEMPT/evidence/icd-with-missing-library.json" <<'JSON'
{"file_format_version": "1.0.0", "ICD": {"library_path": "/nonexistent/libvulkan_lvp.so", "api_version": "1.4.0"}}
JSON

    negative_step "wrong-icd-manifest" "missing manifest fails before smoke" \
        env "VK_DRIVER_FILES=$ATTEMPT/does-not-exist.json" \
        "$SMOKE" --preflight --require-validation --allow-cpu --expect-device-name llvmpipe \
        --emit "$ATTEMPT/evidence/neg-wrong-icd.json" || FAILED=1

    negative_step "missing-icd-library" "unusable ICD library fails" \
        env "VK_DRIVER_FILES=$ATTEMPT/evidence/icd-with-missing-library.json" \
        "$SMOKE" --preflight --require-validation --allow-cpu --expect-device-name llvmpipe \
        --emit "$ATTEMPT/evidence/neg-missing-icd-library.json" || FAILED=1

    negative_step "validation-layer-disabled" "ambient layer filtering cannot silently drop validation" \
        env "VK_LOADER_LAYERS_DISABLE=VK_LAYER_KHRONOS_validation" \
        "$SMOKE" --smoke --require-validation --allow-cpu --expect-device-name llvmpipe \
        --emit "$ATTEMPT/evidence/neg-validation-disabled.json" || FAILED=1

    negative_step "device-identity-substitution" "a different device cannot satisfy the proof" \
        "$SMOKE" --preflight --require-validation --allow-cpu --expect-device-name not-llvmpipe \
        --emit "$ATTEMPT/evidence/neg-device-identity.json" || FAILED=1

    negative_step "missing-required-feature" "capability-fixture path fails closed" \
        "$SMOKE" --preflight --require-validation --allow-cpu --expect-device-name llvmpipe \
        --simulate-missing raytracing --emit "$ATTEMPT/evidence/neg-missing-feature.json" || FAILED=1

    negative_step "ambient-display-endpoint" "a foreign/unauthenticated X endpoint cannot satisfy the surface path" \
        env "XAUTHORITY=$X_EMPTY_AUTH" \
        "$SMOKE" --smoke --require-validation --allow-cpu --expect-device-name llvmpipe \
        --emit "$ATTEMPT/evidence/neg-display.json" || FAILED=1

    # Guard-level negatives: each must be rejected before any proof effect.
    if (LP_NUM_THREADS=4; assert_canonical_thread_count) >/dev/null 2>&1; then
        record "guard-noncanonical-thread-count" "nonzero" "0" "FAILED: accepted"
        FAILED=1
    else
        record "guard-noncanonical-thread-count" "nonzero" "1" "ok"
    fi
    if (LP_NO_RAST=1; assert_execution_overrides_absent) >/dev/null 2>&1; then
        record "guard-forbidden-llvmpipe-override" "nonzero" "0" "FAILED: accepted"
        FAILED=1
    else
        record "guard-forbidden-llvmpipe-override" "nonzero" "1" "ok"
    fi
    if (LD_PRELOAD=/nonexistent/libfake.so; assert_dynamic_link_clean) >/dev/null 2>&1; then
        record "guard-dynamic-link-override" "nonzero" "0" "FAILED: accepted"
        FAILED=1
    else
        record "guard-dynamic-link-override" "nonzero" "1" "ok"
    fi
fi

# ---------------------------------------------------------------------------
# Evidence bundle
# ---------------------------------------------------------------------------

export BASIS_SHA MODE
python3 - <<'PY'
import json, os

attempt = os.environ["ATTEMPT"]
rows = []
with open(os.path.join(attempt, "evidence", "steps.tsv")) as handle:
    for line in handle:
        line = line.rstrip("\n")
        if not line:
            continue
        name, expected, actual, ok = line.split("\t")
        rows.append({"step": name, "expected": expected, "observed_exit": actual, "ok": ok == "ok"})

def load(name):
    path = os.path.join(attempt, "evidence", name)
    if not os.path.exists(path):
        return None
    with open(path) as handle:
        try:
            return json.load(handle)
        except json.JSONDecodeError:
            return {"parse_error": True}

basis = load("basis.json") or {}
evidence = {
    "issue": "geromet/Fleet-Control#501",
    "mode": os.environ["MODE"],
    "basis_sha256": os.environ["BASIS_SHA"],
    "source_head": basis.get("source", {}).get("head"),
    "source_dirty": basis.get("source", {}).get("dirty"),
    "steps": rows,
    "all_steps_ok": all(row["ok"] for row in rows),
    "smoke": load("smoke.json"),
    "canary": load("canary.json"),
    "preflight": load("preflight.json"),
    "production_policy": load("production-policy.json"),
    "inputs": {
        "icd_manifest": basis.get("native_runtime_closure", {}).get("mesa_lavapipe", {}).get("icd_manifest"),
        "device": (load("smoke.json") or {}).get("device_name"),
        "x_server": basis.get("native_runtime_closure", {}).get("x_server", {}).get("path"),
        "display": (load("smoke.json") or {}).get("display"),
    },
}
json.dump(evidence, open(os.path.join(attempt, "evidence", "evidence.json"), "w"), indent=2, sort_keys=True)
print(json.dumps({k: evidence[k] for k in ("mode", "basis_sha256", "all_steps_ok", "inputs")}, indent=2))
PY

log "evidence bundle: $ATTEMPT/evidence/evidence.json"
if [ "$FAILED" -ne 0 ]; then
    log "RESULT: FAILED (see steps above)"
    exit 1
fi
log "RESULT: all steps matched expectations"
