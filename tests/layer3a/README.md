# Layer-3A LavaPipe Vulkan runtime smoke (`geromet/Fleet-Control#501`)

Test-only, repo-local verification lane for MCVR. It proves the MCVR Vulkan
instance/surface/device/queue/swapchain lifecycle on Mesa **LavaPipe** under a
recorded runtime basis and an attempt-owned X11 endpoint, and it proves that the
shipping discrete-GPU-only policy still rejects software devices.

This is **Layer 3A only**. It says nothing about RTX/DLSS behavior, rendering
fidelity, performance, or physical-GPU/HIL evidence (`#496` remains the owner of
that tier).

## Entrypoint

```sh
bash tools/layer3a-vulkan-smoke.sh            # positive proof run
bash tools/layer3a-vulkan-smoke.sh --verify   # positive run + negative matrix
```

The GitHub workflow `.github/workflows/layer3a-vulkan-smoke.yml` only installs
the native stack and invokes that script; it contains no verification logic of
its own.

## What a run does

1. Refuses to run as uid 0 (the Vulkan loader ignores `VK_DRIVER_FILES` and layer
   path variables when elevated), with `LD_PRELOAD`/`LD_LIBRARY_PATH`/`LD_AUDIT`
   set, with `LP_NUM_THREADS` other than the canonical `1`, or with an unadmitted
   LLVMpipe/Gallium override (`LP_NATIVE_VECTOR_WIDTH`, `GALLIUM_OVERRIDE_CPU_CAPS`,
   `LP_DEBUG`, `LP_PERF`, `LP_NO_RAST`, `GALLIUM_DRIVER`,
   `MESA_LOADER_DRIVER_OVERRIDE`).
2. Builds the two test targets from `tests/layer3a` (standalone CMake project;
   only C++23, Vulkan headers/loader and GLFW).
3. Records `basis.json` **before** any Vulkan/GLFW effect:
   - source head and dirty flag;
   - `HostExecutionBasis`: Linux arch, CPU vendor/family/model/stepping/microcode,
     normalized CPU-flag digest, visible CPU count, affinity, cgroup cpuset,
     kernel release/build and `/proc/version` digest, `LP_NUM_THREADS=1`;
   - Mesa/LavaPipe ICD manifest and driver library, Vulkan loader, Khronos
     validation manifest and library, GLFW, X server binary, smoke and
     production-policy binaries, and the smoke binary's shared-library closure,
     each by SHA-256;
   - the declared loader control plane (`VK_DRIVER_FILES`, implicit-layer
     filtering, absent loader settings/`VK_LAYER_PATH`/dynamic-link overrides).
4. Starts an attempt-owned X server on a private display with an attempt-owned
   `XAUTHORITY`, and proves that the same display rejects a connection without
   that cookie.
5. Runs `layer3a_smoke` in an allowlisted `env -i` environment with attempt-owned
   `HOME`/`XDG_*`/`TMPDIR`:
   - `--preflight`: instance, device enumeration, extension/feature/queue
     capability contract;
   - `--smoke`: GLFW X11 window → Vulkan surface → device → graphics/present
     queue → swapchain → bounded destroy;
   - `--canary`: a deliberately invalid `vkCreateBuffer` must produce a
     validation **error**; a run that does not see it fails.
6. Applies the production policy with `layer3a_production_policy --prod-reject`
   (exit 3 = CPU device rejected as required) and proves the production target
   refuses `--allow-cpu` (CPU admission is compiled in only for the test target).
7. In `--verify` mode, runs the negative matrix:

   | Negative | Must fail because |
   |---|---|
   | wrong ICD manifest path | no device is visible |
   | ICD manifest with missing driver library | the software driver cannot load |
   | validation layer filtered out by the loader | required layer cannot be enabled |
   | expected device name mismatch | a different device cannot satisfy the proof |
   | required ray-tracing capability fixture | capability preflight fails closed |
   | connection without the attempt X cookie | the X endpoint is not the admitted one |
   | non-canonical `LP_NUM_THREADS` | host thread-count policy is bound to 1 |
   | forbidden LavaPipe/Gallium override | unadmitted execution controls are rejected |
   | `LD_PRELOAD` set | unbound dynamic-link injection is rejected |

8. Writes `evidence.json` summarizing every step, the basis digest, the selected
   device, display and source head.

## Device policy

`src/core/vulkan/device_policy.hpp` is now the single implementation of MCVR's
device contract. `src/core/vulkan/physical_device.cpp` uses
`Admission::ProductionDiscreteOnly`; the test target uses
`Admission::TestAllowCpuDevice`, which admits **only** `VK_PHYSICAL_DEVICE_TYPE_CPU`
and never an integrated/discrete GPU. The test target is compiled with
`MCVR_LAYER3A_TEST_CPU_ADMISSION`; the production-policy target is compiled
without it and refuses `--allow-cpu`.

## Evidence boundary

A green run is evidence for: the exercised software-Vulkan path on the recorded
basis, with the recorded validation layer and the recorded X endpoint.

It is **not** evidence for: immutable hosted-runner identity, network/dependency
closure of package installation, elevated-execution behavior, full validation
settings-basis equivalence (`VK_LAYER_SETTINGS_PATH` / per-setting `VK_LAYER_*`
semantics), sealed container/rootfs packaging, or `#496` HIL claims.

The hosted CI job installs Ubuntu packages by name, so the CI **basis digest is
recorded per run** rather than being a pre-declared immutable image. A PASS from
this lane must not be described as a reproducible-baseline PASS until the basis is
sealed separately.
