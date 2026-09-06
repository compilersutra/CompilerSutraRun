# CompilerSutraRun milestones

**Product goal:** the first-go platform for describing a workload once and running it across **GPU and CPU APIs** with validation, native timing, and comparable JSON — without inventing a new IR, driver, or compiler.

**CLI:** `csrun`  
**Sibling:** `csperf` (CompilerSutraPerfTool) — experiment/diff/report front door; delegates GPU execute here.  
**Source of truth for requirements:** [`prompt/001-universal-gpu-platform-spec.md`](../prompt/001-universal-gpu-platform-spec.md)

---

## North star

```text
csrun workload.yaml --all
```

```text
Backend      Device              Compiler       Median       Validation
-----------------------------------------------------------------------
Vulkan       AMD Radeon …        glslang        1.42 ms      PASS
CUDA         NVIDIA …            NVCC           0.91 ms      PASS
D3D12        …                   DXC (DXIL)     1.18 ms      PASS
Metal        Apple …             metalc         …            PASS
HIP          AMD …               hipcc          …            PASS
OpenCL       …                   clc            …            PASS
WebGPU       …                   naga/tint      …            PASS
CPU          host                clang/gcc      …            PASS
```

Unavailable backends report `UNAVAILABLE` / `SKIPPED` — never fake results.

---

## Milestone status legend

| Status | Meaning |
|--------|---------|
| **done** | Shipped in tree |
| **active** | Current engineering focus |
| **planned** | Sequenced next |
| **future** | After core multi-backend is stable |

---

## M0 — Foundation & Vulkan slice *(done)*

Corresponds to prompt **Phase 0–2**.

| Deliverable | Status |
|-------------|--------|
| Repo assessment + `docs/architecture.md` | done |
| Workload YAML, registry, errors, JSON, CLI | done |
| Vulkan discover → SPIR-V (glslang) → execute | done |
| CPU reference validation + GPU timestamps | done |
| `vector_add` vertical slice on real GPU | done |

**Exit:** `csrun examples/vector_add/vector_add.yaml --backend vulkan` → PASS + JSON.

---

## M1 — Vulkan depth *(active)*

Prompt **Phase 3** + hardening.

| Deliverable | Notes |
|-------------|--------|
| Vulkan `matmul` | M/N/K, throughput (GFLOP/s), validation |
| Vulkan `reduction` | Third workload for runtime maturity |
| Capability filtering | dtype / workgroup / memory gates |
| Compilation cache design | key = workload + device + toolchain hash |
| Integration test gate | skip cleanly without GPU |

**Exit:** matmul + reduction on Vulkan; stable JSON schema v1.

---

## M2 — CUDA path

Prompt **Phase 4–5**.

| Deliverable | Notes |
|-------------|--------|
| CUDA device discovery | Driver/Runtime as appropriate |
| Module/PTX load (no custom compiler) | NVCC/toolchain orchestration |
| `vector_add` then `matmul` | Same workloads as Vulkan |
| CUDA events timing | `timing.source = gpu_timestamp` (CUDA) |
| Optional build | `CSRUN_ENABLE_CUDA` — no fail if missing |

**Exit:** `csrun vector_add.yaml --backend cuda` on NVIDIA hardware.

---

## M3 — Cross-backend runner (`--all`)

Prompt **Phase 6**.

| Deliverable | Notes |
|-------------|--------|
| `--all` table report | Backend / Device / Median / Validation |
| Skip unavailable backends | Not a failure unless forced |
| Shared workload fixtures | Identical YAML across backends |
| Result schema stability | Comparable machine-readable outputs |

**Exit:** one YAML → Vulkan + CUDA rows (others SKIPPED if absent).

---

## M4 — D3D12 + DXIL

Prompt **Phase 7**. Windows-first; Linux stays `UNAVAILABLE`.

| Deliverable | Notes |
|-------------|--------|
| DXGI adapter discovery | Honest UNAVAILABLE on non-Windows |
| DXC: HLSL → **DXIL** | Do not reimplement HLSL compiler |
| D3D12 compute path | Queue, resources, PSO, dispatch, fences |
| Timestamp queries + readback | Native D3D12 timing |
| `vector_add` / `matmul` | Same workload model |

**Exit:** `csrun … --backend d3d12` on Windows GPU.

---

## M5 — Metal

macOS / iOS Apple Silicon path.

| Deliverable | Notes |
|-------------|--------|
| Metal device discovery | UNAVAILABLE off-Apple |
| Metallib / metalc orchestration | No custom Metal shader compiler |
| Compute dispatch + counters | Native timing where available |
| Workload parity | `vector_add`, `matmul` |

**Exit:** `csrun … --backend metal` on Apple GPU.

---

## M6 — HIP / ROCm + OpenCL

Independent backends (AMD ≠ Vulkan-only).

| Deliverable | Notes |
|-------------|--------|
| HIP backend | hipcc + ROCm runtime; rocprof optional later |
| OpenCL backend | Portable fallback where vendors expose CL |
| Capability-based selection | Prefer real device features over vendor guesses |

**Exit:** HIP and/or OpenCL execute + validate for core workloads.

---

## M7 — WebGPU + CPU backend

Broaden “first-go” beyond discrete GPU APIs.

| Deliverable | Notes |
|-------------|--------|
| WebGPU backend | Dawn/wgpu; WGSL toolchain via existing compilers |
| **CPU backend** | Host reference + optional optimized CPU kernels |
| Same YAML | CPU row in `--all` for correctness baseline |
| Sync with `csperf` CPU strengths | Avoid duplicating `perf` counters; focus on portable run |

**Exit:** `--all` can include WebGPU + CPU when available.

---

## M8 — `csperf` integration

Product packaging: one front door for experiments.

| Deliverable | Notes |
|-------------|--------|
| Adapter: `csperf` → `csrun` | Vulkan full-run (replace preview for execute) |
| Map `csrun` JSON → `csperf` result schema | Diff/report/dashboard keep working |
| Keep `csrun` standalone CLI | CI + power users |
| Feature flag | preview vs full-run |

**Exit:** `csperf run --backend vulkan` executes via `csrun` and stores comparable artifacts.

---

## M9 — MLIR / LLVM compiler layer *(future)*

Prompt **Phase 8**. Only after native backends are real.

| Deliverable | Notes |
|-------------|--------|
| Investigate MLIR GPU / SPIR-V / NVVM / ROCDL | No custom IR |
| Optional `CSRUN_ENABLE_MLIR` | Off by default |
| Lowering paths | Workload → MLIR → backend artifacts |

**Exit:** documented design + optional prototype path; not required for `--all` MVP.

---

## M10 — Platform polish *(ongoing / future)*

| Deliverable | Notes |
|-------------|--------|
| `--record` / `--replay` | Reproducibility |
| Benchmark modes | kernel / end-to-end / transfer |
| Mesa as reference only | Study ideas; **no hard Mesa dependency** |
| Security hardening | untrusted YAML/toolchain outputs |
| Docs site / tutorials | “first-go” onboarding |

---

## Backend roadmap (at a glance)

```text
Done     Vulkan (SPIR-V)
Active   Vulkan matmul/reduction
Next     CUDA (PTX) → --all
Then     D3D12 (DXIL) → Metal → HIP → OpenCL → WebGPU → CPU
Later    MLIR/LLVM orchestration
Always   integrate mature toolchains; never fake backends
```

| Backend | Artifact | Toolchain | Milestone |
|---------|----------|-----------|-----------|
| Vulkan | SPIR-V | glslang / Shaderc + SPIR-V Tools | M0–M1 |
| CUDA | PTX / cubin | NVCC / CUDA | M2 |
| D3D12 | **DXIL** | DXC | M4 |
| Metal | metallib | metalc / Metal | M5 |
| HIP | AMDGPU | hipcc / ROCm | M6 |
| OpenCL | SPIR-V / native | vendor CL stack | M6 |
| WebGPU | WGSL→… | Dawn/wgpu + existing compilers | M7 |
| CPU | host binary / ref | clang/gcc | M7 |

---

## How this helps `csperf`

1. **`csrun` owns execute/validate/time** across APIs.  
2. **`csperf` owns experiments, diffs, dashboards, CPU `perf`.**  
3. As Metal / DXIL / CUDA / HIP land in milestones above, `csperf` gets them “for free” via the adapter (M8) instead of reimplementing each native path.

---

## Tracking

- GitHub milestones on [compilersutra/CompilerSutraRun](https://github.com/compilersutra/CompilerSutraRun) mirror **M0–M10**.  
- Issues should reference a milestone id (`M1`, `M2`, …).  
- Do not mark a backend milestone complete without a **real** device path or an explicit `UNAVAILABLE` build skip.
