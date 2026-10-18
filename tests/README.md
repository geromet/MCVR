# Deterministic singleton diagnostics (partial #500)

This standalone C++23/CMake suite ports and corrects the prior
`tests/deterministic-suite` singleton cases onto the current MCVR source.
It does not require JNI, Vulkan, GLM or a GPU.

Run locally from the repository root:

```sh
cmake -S tests -B build/singleton-diagnostics
cmake --build build/singleton-diagnostics --parallel
ctest --test-dir build/singleton-diagnostics --output-on-failure
```

The five distinct `Singleton<Derived>` types make the cases independent of
registration/execution order. Each `isolate_*` CTest entry executes in its
own fresh process, while `singleton_full` runs all cases. `--list` emits
the fixed registry, and `--run-exact NAME` selects only one exact case;
unknown names, unexpected arguments, and duplicate/empty identities fail.
There is deliberately no substring matching.

**Proof boundary:** these are useful compile/runtime diagnostics, **not** an
accepted Layer-2 deterministic proof. This subset does **not** implement
#500 spec11 `EffectInputClosure` across path and non-path inputs, descendants,
inherited handles or mappings; it does not bind immutable execution/source
basis, demonstrate equivalence between full/isolated invocations, or prove
emitted-SPIR-V ABI. A local/hosted green CTest result MUST NOT be interpreted
as satisfying #500 or its #499/#487/#538 prerequisites. Other prior test
families must be selectively ported and verified separately.

Source ancestry: `tests/deterministic-suite/tests/{main.cpp,harness.hpp,
test_singleton.cpp}` (read as evidence only); new code intentionally does
not preserve their order-dependent cross-case singleton assumptions.
