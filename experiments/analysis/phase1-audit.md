# Phase 1 audit — CompilerSutraRun (2026-10-17)

Verified against tree `/home/aitr/compilersutra/universalrunner` and
raw JSON under `/tmp/csrun-validate-amd/` (also to be archived under
`experiments/raw/` once the paper freeze lands).

## 1. Workload size

| Item | Value |
|------|-------|
| YAML | `examples/vector_add/vector_add.yaml` |
| `dimensions.count` | **1048576** |
| Equivalent | \(N = 2^{20} = 1{,}048{,}576\) fp32 elements |
| Warmup / iterations | 10 / 100 |
| Declared YAML `tolerance` | `1.0e-5` (see validation predicate below) |

**Paper rule:** always write \(N=2^{20}=1{,}048{,}576\), never `N=220`.

## 2. Timing-table audit vs raw JSON

Saved smoke JSON medians (one batch; **not** multi-batch paper data):

| File | Backend | Device | Validation | median_ms | source |
|------|---------|--------|------------|-----------|--------|
| vulkan-discrete.json | vulkan | RX 9060 XT | passed | 0.431 | gpu_timestamp |
| vulkan-integrated.json | vulkan | Raphael iGPU | passed | 0.450 | gpu_timestamp |
| hip-discrete.json | hip | RX 9060 XT | passed | 0.052 | gpu_timestamp |
| hip-integrated.json | hip | Raphael | passed | 0.471 | gpu_timestamp |
| opencl-discrete.json | opencl | gfx1200 | passed | 0.051 | **cpu_wall** |
| opencl-integrated.json | opencl | gfx1036 | passed | 0.455 | **cpu_wall** |
| opengl-default.json | opengl | Raphael (radeonsi) | passed | 0.406 | gpu_timestamp |

A later interactive refresh produced slightly different Vulkan medians (~0.39 ms).
**Do not mix batches in one table.** Paper tables must cite a commit hash + raw
directory. Prior draft numbers (0.391 / 0.018 / …) are **invalid for submission**
until regenerated from a frozen artifact.

OpenGL discrete (`DRI_PRIME=1`) was measured in a separate session (~0.018 ms)
and was **not** in the `/tmp/csrun-validate-amd` archive above.

## 3. What each timer actually measures (vector_add)

Verified from source:

| Backend | Label | Timed interval | Includes sync? | Includes H2D/D2H in timed sample? |
|---------|-------|----------------|----------------|-----------------------------------|
| **Vulkan** | `gpu_timestamp` | `vkCmdWriteTimestamp` span around compute dispatch (stage markers / top→bottom) | Queue wait outside the GPU stamp path; host submit/wait recorded separately in counters | **No** — H2D before warmup; D2H after timed loop (`hostH2dMs` / `hostD2hMs`) |
| **OpenGL** | `gpu_timestamp` | `GL_TIME_ELAPSED` around `DispatchCompute` + memory barrier + `Finish` | Yes (`Finish` before ending query path) | **No** — maps before/after loop |
| **HIP** | `gpu_timestamp` | `hipEventRecord` start→stop around `hipModuleLaunchKernel` + `hipDeviceSynchronize` | Yes | **No** — memcpy H2D before; D2H after |
| **OpenCL** | `gpu_timestamp` (preferred) / `cpu_wall` fallback | Profiling queue + `CL_PROFILING_COMMAND_START/END` on NDRange event; else host wall + `clFinish` | Yes (wait/finish) | **No** — `COPY_HOST_PTR` create; readback after loop |

**Implication (updated):** OpenCL now prefers Mode B when the ICD allows a
profiling queue. Still do not rank APIs from a single microkernel; Mode B
boundaries still differ slightly across Vulkan/GL/HIP/OpenCL. Phase 3 must
report `timing.source` per cell and keep Mode A/B in separate panels.

## 4. Validation predicate (actual code)

`Comparator::compareFloat32` (`src/validation/Comparator.cpp`):

A float element mismatches iff **both**:

\[
|a-b| > \epsilon_{\mathrm{abs}}
\quad\text{and}\quad
\frac{|a-b|}{\max(|b|,10^{-12})} > \epsilon_{\mathrm{rel}}
\]

YAML `tolerance: 1.0e-5` sets **both** `absoluteTolerance` and
`relativeTolerance` to the same value (`WorkloadParser.cpp`).

Statuses: `passed` | `failed` | `skipped` | `not_run`
(`RunResult.cpp`). `--no-validate` → `skipped`, not `passed`
(`VectorAddScaffold.cpp`).

**Gap:** no published negative-test suite asserting that a known-bad kernel
fails validation (Phase 2).

## 5. Backend feature matrix (this host / this build)

| Backend | Built | Runtime on AMD dual-GPU | vector_add | Notes |
|---------|-------|-------------------------|------------|-------|
| Vulkan | yes | yes (RADV, 2 devices) | yes | Graphics/SPIR-V corpus also present; large LOC |
| OpenGL | yes | yes (1 GL context; PRIME selects GPU) | yes | No multi-index; use `DRI_PRIME` |
| OpenCL | yes | yes (gfx1200 + gfx1036) | yes | Timing is cpu_wall only today |
| HIP | yes | yes (2 devices) | yes | hiprtc |
| OpenGLES | stub | — | — | registered UNAVAILABLE |
| CUDA | stub | — | — | registered UNAVAILABLE |
| D3D12 | stub | — | — | registered UNAVAILABLE |
| Metal | stub | — | — | registered UNAVAILABLE |
| WebGPU | stub | — | — | registered UNAVAILABLE |
| CPU backend | stub | — | — | host reference is scaffold, not a backend |

**Paper rule:** stubs = registered but unimplemented, never “supported.”

## 6. Workload coverage (honest)

| Workload | Status for evaluation |
|----------|----------------------|
| `vector_add` (CL / HIP / GLSL-comp / OpenGL) | **Runnable** end-to-end on 4 APIs |
| `matmul` example | Vulkan SPIR-V path / scaffolding — **not** a completed multi-API evaluation |
| Shader corpus (triangle, plasma, …) | Vulkan-oriented; not cross-API compute contract |

Shared YAML ≠ shared GPU source. Kernels are **separately authored** per API
(`.comp` / `.cl` / `.hip` / GLSL compute).

## 7. Rough LOC (indicative only — not RQ1 result)

| Slice | Approx. LOC (`.cpp` under `src/`) |
|-------|-----------------------------------|
| All `src/` | ~11.2k |
| Shared (non-`backends/`) | ~3.3k |
| `backends/vulkan/` | ~6.9k (includes graphics/RT smoke) |
| `backends/opengl/` | ~0.4k |
| `backends/opencl/` | ~0.3k |
| `backends/hip/` | ~0.3k |

RQ1 needs a **scoped** baseline (minimal per-API harness for vector_add only)
vs csrun shared + backend-specific vector_add paths — Phase 3/4. Do not cite
these totals as reuse ratio.

## 8. Availability / fail-closed (code vs tested)

| Behavior | In code? | Automated test? |
|----------|----------|-----------------|
| Stub → UNAVAILABLE note | yes (`StubBackend`) | partial (unit/list) |
| Missing device → UNAVAILABLE | yes | needs injection matrix |
| `--device-index` honored | yes (fixed in `PlatformRuntime::run`) | needs dual-GPU CI or scripted assert |
| Validation disabled → skipped | yes | needs assert |
| Known-bad output → failed | comparator can fail | **missing deliberate bad-kernel test** |
| OpenCL device events timing | no | — |

## 9. Paper claims allowed after Phase 1

**Allowed:** describe architecture; report smoke that dual-GPU AMD host can
execute vector_add with CPU-ref validation and sourced timings; document
contracts above.

**Not allowed yet:** portable performance ranking; general correctness across
ops/types; quantified harness-reuse benefit; “all APIs comparable.”

## 10. Next actions (priority order)

1. Archive smoke JSON under `experiments/raw/phase1-smoke/` with env manifest.
2. Phase 2: correctness + availability injection tests.
3. Phase 3: Mode A vs Mode B timing; ≥3 sizes; ≥3 independent batches; raw samples.
4. RQ1 independent-harness baseline (scoped LOC).
5. Rewrite evaluation section **only** from scripts + raw/.
