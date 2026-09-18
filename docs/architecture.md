# CompilerSutraRun Architecture

**Project:** CompilerSutraRun  
**CLI:** `csrun`  
**Location:** `/home/aitr/compilersutra/universalrunner`  
**Sibling (do not merge):** `/home/aitr/compilersutra/tools/CompilerSutraPerfTool` (`csperf`)  
**Do not modify:** `/home/aitr/osc/llvm_project_without_change`

---

## 1. Current repository state

| Item | Status |
|------|--------|
| Language | C++20 (greenfield) |
| Build | CMake + Ninja |
| Code | Foundation + Vulkan/OpenGL/OpenCL/HIP `vector_add`; virtual `execute` |
| GPU backends | See [milestones.md](milestones.md); CUDA/D3D12/Metal stubs |
| Tests | GoogleTest unit tests (CPU-only CI path) |
| Prompt archive | `prompt/` (verbatim user briefs) |
| Roadmap | [docs/milestones.md](milestones.md) — M0–M10 first-go multi-API platform |

Phase 0 assessment: empty directory → scaffolded as CompilerSutraRun. No prior GPU platform code to preserve in this tree.

---

## 2. Existing architecture

None in-repo prior to scaffolding. Related ecosystem knowledge:

- **CompilerSutraPerfTool**: Python experiment/perf CLI; CPU stable; HIP/OpenCL experimental; Vulkan preview only. Different product boundary.
- **LLVM/MLIR tree**: reference for future compiler integration only — not a dependency of this repo and must not be edited for this work.

---

## 3. Proposed architecture

```text
                         csrun
                            |
                            v
                    Workload Definition (YAML)
                            |
                            v
                    Workload Parser
                            |
                            v
                  Common Workload Model
                            |
                            v
                 Capability / Device Model
                            |
                            v
                   Backend Selection
                            |
          +-----------------+------------------+
          |                 |                  |
          v                 v                  v
       Vulkan             CUDA              D3D12
       (SPIR-V)           (PTX)             (DXIL)
                            |
                            v
                     Validation + Benchmark
                            |
                            v
                      Result (terminal / JSON)
```

**Product boundary:** orchestration only. Reuse Vulkan/CUDA/D3D12/DXC/Shaderc/SPIR-V Tools/LLVM/MLIR — do not reimplement them.

---

## 4. Open-source components to reuse

| Component | Role | MVP |
|-----------|------|-----|
| Vulkan-Headers / Loader | Vulkan API | Phase 2 optional |
| SPIR-V Tools | SPIR-V validate/inspect | Phase 2 |
| Shaderc / glslang | GLSL→SPIR-V | Phase 2 |
| DXC | HLSL→DXIL | Phase 7 (Windows) |
| CUDA Toolkit | CUDA backend | Phase 4 |
| yaml-cpp | Workload YAML | Phase 1 |
| nlohmann/json | Result JSON | Phase 1 |
| CLI11 | CLI | Phase 1 |
| GoogleTest | Tests | Phase 1 |
| fmt | Formatting (optional) | if needed |

---

## 5. Dependency graph

```text
csrun (CLI)
  └── csrun_runtime          (PlatformRuntime + BuiltinBackends)
        ├── csrun_backend    (registry, selector, Backend interface)
        ├── csrun_workload   (YAML + VectorAddScaffold)
        ├── csrun_compiler   (SpirvToolchain — not in core)
        ├── csrun_validation
        ├── csrun_benchmark  (Timing + Statistics)
        ├── csrun_result
        ├── csrun_device
        └── csrun_core
              └── optional: csrun_backend_{vulkan,opengl,opencl,hip}
```

Backends do not depend on each other. Core never includes `vulkan.h` / `cuda.h` / `hip_runtime.h` / `CL/cl.h`.

---

## 6. Backend architecture

```text
createDefaultBackendRegistry()
  ├── VulkanBackend::execute   (SPIR-V)
  ├── OpenGLBackend::execute   (GLSL)
  ├── OpenCLBackend::execute   (.cl)
  ├── HipBackend::execute      (hiprtc)
  └── StubBackend…            (UNAVAILABLE)
```

`PlatformRuntime::run` selects a backend then calls **`backend->execute(...)`** — no `dynamic_cast` type switch.

Shared host prep / validation / result fill lives in `VectorAddScaffold`.

---

## 7. Workload model

Backend-neutral YAML describing operation, dimensions, dtypes, execution (warmup/iterations), validation, optional backend constraints.

Must not embed API-specific types (`VkBuffer`, `cudaStream_t`, …).

---

## 8. Compiler integration strategy

| Backend | Toolchain | Artifact |
|---------|-----------|----------|
| Vulkan | Shaderc/glslang + SPIR-V Tools | SPIR-V |
| CUDA | NVCC / CUDA toolchain | PTX / module |
| D3D12 | DXC | DXIL |
| Future | MLIR/LLVM dialects | per-target |

MVP: load/compile existing artifacts via adapters. No custom IR. No Everything→SPIR-V.

---

## 9. Runtime architecture

```text
Workload + ExecuteOptions
        → Backend::execute
        → (API compile / submit / timing)
        → VectorAddScaffold::fillVectorAddResult
        → RunResult
```

`--all` runs every available backend through the same path.

---

## 10. Validation architecture

CPU reference vs GPU readback. Exact integer compare; FP abs/rel tolerances; NaN/Inf policy. Deterministic. Every benchmark result includes validation status.

---

## 11. Benchmark architecture

Separate compile / setup / H2D / kernel / D2H / total. GPU timestamps via native API mechanisms; timing source recorded in JSON. No fake numbers.

---

## 12. Error model

`Expected<T>` / `Error` with codes such as:

`INVALID_WORKLOAD`, `UNSUPPORTED_OPERATION`, `BACKEND_UNAVAILABLE`, `DEVICE_NOT_FOUND`, `COMPILATION_FAILED`, `EXECUTION_FAILED`, `VALIDATION_FAILED`, `TOOLCHAIN_NOT_FOUND`, `UNIMPLEMENTED`, …

Exceptions are not used for expected environmental failures.

---

## 13. Testing strategy

| Tier | Scope | Hardware |
|------|-------|----------|
| Unit | parser, selection, stats, schema, errors | CPU only |
| Integration | Vulkan/CUDA/D3D12 | skip if unavailable |
| Workloads | `vector_add`, later `matmul` | GPU when present |

---

## 14. Security considerations

Treat YAML and compiler outputs as untrusted. Avoid shell injection; bound allocations; no hard-coded tool paths as sole discovery; no arbitrary file execution.

---

## 15. Future MLIR architecture

Post-MVP investigation only:

```text
Workload → MLIR → SPIR-V | NVVM | ROCDL/AMDGPU → Vulkan | CUDA | HIP
```

Do not implement in Phase 1–2.

---

## Phase 1 implementation plan

### Goal

Buildable common foundation + CLI skeleton. No fake GPU execution.

### Exact files

| Action | Path |
|--------|------|
| Add | `CMakeLists.txt`, `CMakePresets.json`, `README.md`, `LICENSE`, `.clang-format`, `.gitignore` |
| Add | `docs/architecture.md` (this file) |
| Add | `docs/workload-format.md`, `docs/backend-design.md` (stubs with real content) |
| Add | `include/csrun/core/{Error,Expected,Log,Types}.h` |
| Add | `include/csrun/workload/{Workload,WorkloadParser}.h` |
| Add | `include/csrun/backend/{BackendType,Backend,BackendRegistry,BackendSelector}.h` |
| Add | `include/csrun/device/DeviceInfo.h` |
| Add | `include/csrun/result/RunResult.h` |
| Add | `include/csrun/validation/ValidationConfig.h` |
| Add | `include/csrun/benchmark/BenchmarkConfig.h` |
| Add | matching `src/**/*.cpp` |
| Add | `tools/csrun/main.cpp` |
| Add | `tests/unit/...`, `tests/workloads/vector_add.yaml` |
| Add | `examples/vector_add/vector_add.yaml` |
| Add | `prompt/` archive (done) |

### CMake options

```text
CSRUN_ENABLE_VULKAN=OFF  (Phase 2)
CSRUN_ENABLE_CUDA=OFF
CSRUN_ENABLE_D3D12=OFF
CSRUN_ENABLE_TESTS=ON
CSRUN_ENABLE_MLIR=OFF
```

### Success criteria

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
./build/tools/csrun/csrun --help
./build/tools/csrun/csrun --list-backends
```

### Next

**Phase 2:** Vulkan `vector_add` real GPU vertical slice.
