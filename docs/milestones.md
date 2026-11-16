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
Vulkan       AMD Radeon …        glslang/SPIR-V 1.42 ms      PASS
OpenGL       …                   GLSL/Mesa      …            PASS
OpenGL ES    phone/embed         GLSL ES        …            PASS
OpenCL       …                   clc            …            PASS
CUDA         NVIDIA …            NVCC/PTX       0.91 ms      PASS
HIP          AMD …               hipcc          …            PASS
D3D12        Windows …           DXC/DXIL       1.18 ms      PASS
Metal        Apple …             metalc         …            PASS
WebGPU       …                   naga/tint      …            PASS
CPU          host                clang/gcc      …            PASS
```

**Linux history covered:** OpenGL / OpenGL ES / OpenCL (pre-Vulkan era) **and** Vulkan (SPIR-V).  
**Windows:** D3D12 + DXIL. **Apple:** Metal. Never fake a backend.

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

## M1 — Vulkan SPIR-V pipeline *(active)*

Not “one workload type at a time”. General path:

```text
.comp / .vert / .frag  →  glslang  →  .spv  →  spirv-val  →  Vulkan API
```

| Deliverable | Notes |
|-------------|--------|
| `SpirvToolchain` | Stage detect from extension; compile + validate |
| `csrun --compile-spv` | Write real `.spv` files |
| `csrun --vulkan-smoke` | Load SPV → `vkCreateShaderModule` → compute/graphics pipeline |
| Workload execute | Still uses same toolchain (e.g. `vector_add.comp`) |
| GPU timestamps | `VK_QUERY_TYPE_TIMESTAMP` on draw/dispatch (offscreen, WSI, compute) |
| **Vulkan counters** | Portable: pipeline stats + occlusion + timestamps + host record/submit/queue-wait + draws/dispatches. Vendor AMD HW via `VK_KHR_performance_query` (RADV) when present. Full checklist in text + nested JSON (`schema_version` 2). |
| Later on this milestone | matmul/reduction as **more shaders**, not a separate product |

**Exit:** `.comp`/`.vert`/`.frag` → `.spv` → Vulkan execute PASS on real GPU, with **timestamps + pipeline-stat counters** in `--format json`.

---

## M2 — CUDA path *(deferred)*

Prompt **Phase 4–5**. **Deferred** until an NVIDIA GPU is available (local or cloud quota).
M13 AMD dual-GPU smoke is archived under `experiments/raw/amd-20261114/`.

| Deliverable | Notes |
|-------------|--------|
| CUDA device discovery | Driver/Runtime as appropriate |
| Module/PTX load (no custom compiler) | NVCC/toolchain orchestration |
| `vector_add` then `matmul` | Same workloads as Vulkan |
| CUDA events timing | `timing.source = gpu_timestamp` (CUDA) |
| Optional build | `CSRUN_ENABLE_CUDA` — no fail if missing |

**Exit:** `csrun vector_add.yaml --backend cuda` on NVIDIA hardware.

---

## M13 — AMD dual-GPU validation *(done — smoke)*

Validate csrun on the dual-AMD Linux host (discrete + integrated) across the
backends that already work here. No NVIDIA required.

| Deliverable | Notes |
|-------------|--------|
| 8-cell dual-GPU smoke | Done — `experiments/raw/amd-20261114/` (8/8 Mode B) |
| Device-selection contracts | Done — `docs/amd-dual-gpu.md` |
| Second multi-API workload | Done — matmul Vulkan + OpenCL/HIP/OpenGL scaffold |
| Multi-batch timing artifact | Done — `multi-batch-vulkan-dgpu/` (illustrative n=5) |
| Archived JSON refresh | Done — dated `amd-20261114` |

**Exit met:** 8/8 smoke green on RX 9060 XT + Raphael; docs + raw JSON + paper tables updated.

**GitHub:** [milestone M13](https://github.com/compilersutra/CompilerSutraRun/milestone/14)

---

## M3 — Cross-backend runner (`--all`) *(done)*

| Deliverable | Notes |
|-------------|--------|
| `--all` table/rows | Runs every **available** backend via virtual `execute` |
| Skip unavailable | Honest skip / stub errors — not a hard failure |
| Shared fixtures | Same YAML across backends |
| Result schema | Stable JSON per backend |

**Exit:** `csrun examples/vector_add/vector_add.yaml --all` → vulkan/opengl/opencl/hip PASS on this host.

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

## M6 — HIP / ROCm + OpenCL *(active — vector_add done)*

Independent backends (AMD ≠ Vulkan-only). OpenCL is the classic portable **pre-/side-Vulkan** compute API on Linux.

| Deliverable | Notes |
|-------------|--------|
| OpenCL backend | **Done for `vector_add`**: ICD + `.cl` online compile + NDRange |
| HIP backend | **Done for `vector_add`**: hiprtc + `libamdhip64` (no hipcc host build) |
| Capability-based selection | Prefer real device features over vendor guesses |
| CUDA | Separate (M2); **this host has no NVIDIA** — test on CUDA machine / cloud |

**Exit (partial):** `csrun … --backend opencl` and `--backend hip` PASS on AMD.

---

## M11 — OpenGL + OpenGL ES *(classic Linux / mobile GL)*

What Linux used **before Vulkan** for graphics — still required for “first-go everywhere.”

| Deliverable | Notes |
|-------------|--------|
| OpenGL backend | **Done for `vector_add`**: EGL surfaceless + GL 4.3 compute + SSBO + timer queries |
| OpenGL ES backend | ES 3.x path for Android/embedded — next |
| Timing | `GL_TIME_ELAPSED` when available |
| Mesa | Used as system GL driver — **not vendored** |

```text
GLSL (.glsl compute)
        |
        v
   OpenGL 4.3+ (EGL headless / Mesa)
        |
        v
      GPU
```

**Exit (partial):** `csrun … --backend opengl` PASS on Linux. GLES still pending.

---

## M12 — Vulkan workload coverage *(active)*

Grow how much real-world Vulkan/GLSL work `csrun` can compile and execute beyond the in-tree examples.

| Deliverable | Notes |
|-------------|--------|
| Broader shader / workload intake | Public and in-house GLSL → SPIR-V paths |
| Compile vs execute coverage | Clear report of what builds vs what runs on GPU |
| Layout / feature growth | Multi-set descriptors done; more patterns via `tests/lit/todo/` |
| Regression gate | In-tree Vulkan examples keep PASS |
| Evidence-based render check | `checkFrame` proves visible output, not just "did not crash" |

**Exit:** documented coverage goals + measurable improvement in shaders/`csrun` can run end-to-end.

Current breadth baseline (`tools/csrun-breadth`, RADV/GFX1200): **84 PASS / 11 FAIL / 0 SKIP**
of 95 external corpus shaders. Remaining gaps, in the order they block coverage:

| Gap | Samples | Root cause |
|-----|---------|-----------|
| `FAIL_BLACK` | 6 | Scene data, not missing capability. Each is one output location with 0–3 sampled images; the smoke pass binds a shared white texture and primes UBOs from the reflected layout, which is not enough to light a scene. `deferred` is the clearest case: `ambient` is `0.0` in the shader and the light UBO is zeroed, so `atten = radius/(dist²+1) = 0` and the frame is `(0,0,0,255)` **by arithmetic** — the run log shows `OCCLUSION=4096` and `FS/PS=4096`, so the draw ran and the shader wrote. `pbrbasic`, `offscreen`, `displacement`, `ssao`, `terraintessellation` are the same shape |
| `FAIL_OTHER` | 4 | `raytracinggltf` needs real glTF geometry and a runtime texture array (its any-hit shader indexes a node buffer by `gl_GeometryIndexEXT`); the other three are ray-tracing samples that need scene data |
| Descriptor kinds | 1 | acceleration-structure / ray-query bindings |
| `FAIL_COMPUTE_RES` | 1 | compute resource binding gap |
| glslang | 1 | `descriptorheapuntyped` needs a newer toolchain |

`FAIL_STAGE` is closed. The ray-tracing smoke path builds its pipeline from the stages a
workload declares instead of a fixed raygen + miss + closest-hit triple, so callable,
any-hit and intersection shaders are bound and run rather than rejected up front. A
declared-but-unbound stage is a real shader that never runs, which is not the same as a
workload that omits it.

A blank frame is only a coverage bug when the workload could have produced colour. These
six are not evidence of a runner defect, and inventing light positions to turn them green
would be fabricating the result, so they stay FAIL with the data requirement stated.

`buffer_device_address` is now supported: `SpirvReflect` detects `PhysicalStorageBuffer`
pointers in push constants (`GL_EXT_buffer_reference`), the graphics device enables
`bufferDeviceAddress`, and the smoke pass creates a device buffer filled with an identity
matrix and pushes its real device address into the push-constant block. The `cube` sample
passes with `OCCLUSION=1352` and `frame check: coverage 33.008%, 252 distinct colours, stable`.

`indirect_draw` is now supported: the smoke path always uses `vkCmdDrawIndirect` with a
pre-filled `VkDrawIndirectCommand` buffer, so the indirect draw path is exercised for every
graphics workload. The `indirectdraw` sample passes with `OCCLUSION=340` and
`frame check: coverage 8.301%, 1 distinct colours, stable`.

`inline_uniform` is now supported: `SpirvReflect` classifies small uniform blocks (≤256 bytes)
as `InlineUniformBlock`, and the smoke path binds them with
`VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK` via `VkWriteDescriptorSetInlineUniformBlock`.
The `pbr` sample passes with `OCCLUSION=1352` and
`frame check: coverage 22.437%, 11 distinct colours, stable`.

`subpassInput` is reflected as its own descriptor kind (carrying `InputAttachmentIndex`)
and bound as `VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT`, so `inputattachments`, `subpasses`,
and `dynamicrenderinglocalread` all execute. An attachment cannot be written and read as
an input in one subpass, so the smoke render pass primes one attachment per declared input
index in subpass 0 and renders the real output to an extra attachment in subpass 1.

The primed values are a documented mid-grey ramp (`0.25 + 0.5·(i+1)/n`), printed in the run
steps — placeholder data, not a G-buffer. What the tests prove is that the subpass load
happened and the shader composed it: `subpasses` composites input 2 with a 0.05 ambient
factor and lands on 10/255, `dynamicrenderinglocalread` uses 0.15 and lands on 29/255, both
matching `0.75 × factor` for attachment 2's prime value. A zero or garbage read would not
produce those numbers.

Smoke defaults fill uniform blocks from the *reflected* matrix layout, not from a
64-byte-granularity guess. `SpirvReflect` flattens every matrix in a block to an absolute
offset, walking nested structs and expanding array elements. This matters: `gears` puts
`model[3]` after a `lightpos` vec4 (so `model[0]` starts at byte 144) and `texturearray`
puts `Instance{ mat4 model; … } instance[8]` after two mat4s — the old heuristic left
those model matrices zero, the geometry collapsed, and both drew nothing.

Determinism is judged on RGB only. A shader that assigns just `.rgb` leaves the alpha
output component undefined per SPIR-V, and RADV fills it with register garbage — two
identical draws differed on 14–18% of texels in alpha alone, which says nothing about the
draw. See `FrameCheckConfig::compareMask`.

Multiple render targets are sized from reflection rather than assumed. `SpirvReflect`
reports `colorOutputCount` — the highest `Location` on a fragment output, plus one — and
the smoke pass builds that many colour attachments in the render pass, the framebuffer, the
blend state, the clear values, and the readback. Every target is read back and frame-checked
individually, and `ok` is the conjunction.

This was a false green, not a missing feature. `deferred_mrt` passed while running
`mrt.frag` against a single-attachment pipeline: locations 1 and 2 were never bound, and a
fragment write to an unbound colour location is undefined rather than harmlessly dropped.
Reverting just the blend-state count reproduces it — targets 1 and 2 come back as pure clear
black and the run now fails with `colour target 1 of 3 failed evidence checks`. The test now
reports `PASS (vkCmdDraw + pixel readback x3)` with per-target coverage, so all three
location writes are covered by evidence instead of one of them.

Tracking: [milestone/13](https://github.com/compilersutra/CompilerSutraRun/milestone/13) · [issue #2](https://github.com/compilersutra/CompilerSutraRun/issues/2)

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
Done     Vulkan (SPIR-V) + .comp/.vert/.frag → .spv smoke
Done     OpenCL (ICD) + HIP (hiprtc) — vector_add
Done     OpenGL (EGL headless, GL 4.3 compute) — vector_add
Active   Vulkan SPIR-V depth (counters / timing)
Active   M12 Vulkan workload coverage
Next     CUDA (PTX) → --all
Then     D3D12 (DXIL) → Metal
Then     OpenGL ES (pre-Vulkan Linux / mobile)
Then     WebGPU → CPU
Later    MLIR/LLVM orchestration
Always   integrate mature toolchains; never fake backends
```

| Backend | Artifact / path | Toolchain | Milestone |
|---------|-----------------|-----------|-----------|
| Vulkan | **SPIR-V** | glslang / Shaderc + SPIR-V Tools | M0–M1 |
| OpenGL | GLSL → driver | Mesa / vendor GL | **M11** |
| OpenGL ES | GLSL ES → driver | GLES / EGL | **M11** |
| OpenCL | SPIR-V / native | vendor CL | M6 |
| CUDA | **PTX** / cubin | NVCC / CUDA | M2 |
| HIP | AMDGPU | hipcc / ROCm | M6 |
| D3D12 | **DXIL** | DXC | M4 |
| Metal | metallib | metalc | M5 |
| WebGPU | WGSL→… | Dawn/wgpu | M7 |
| CPU | host binary / ref | clang/gcc | M7 |

---

## How this helps `csperf`

1. **`csrun` owns execute/validate/time** across APIs.  
2. **`csperf` owns experiments, diffs, dashboards, CPU `perf`.**  
3. As OpenGL / OpenCL / Metal / DXIL / CUDA / HIP land in milestones above, `csperf` gets them via the adapter (M8) instead of reimplementing each native path.

---

## Tracking

- GitHub milestones on [compilersutra/CompilerSutraRun](https://github.com/compilersutra/CompilerSutraRun) mirror **M0–M11**.  
- Issues should reference a milestone id (`M1`, `M11`, …).  
- Do not mark a backend milestone complete without a **real** device path or an explicit `UNAVAILABLE` build skip.
