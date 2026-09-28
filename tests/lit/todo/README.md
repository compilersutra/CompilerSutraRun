# TODO features — how to grow csrun

Each folder under `todo/` is **one missing capability**. Lit expects it to **fail** (`XFAIL`) until you implement it, then you **promote** it to `execute/corpus/`.

## Recipe (add a feature via a test)

1. **Pick a corpus shader** that needs the feature  
   (`external/shader-corpus/Vulkan/shaders/glsl/<sample>/…`).
2. **Confirm it fails loudly** (not a silent PASS):
   ```bash
   . scripts/env-mesa-main-radv.sh
   build/bin/csrun /tmp/probe.yaml --vendor-counters off --no-benchmark --log-level error
   ```
   If it exits 0 while doing the wrong thing, add an `UNSUPPORTED_OPERATION` reject first.
3. **Add the probe**:
   - `tests/lit/Inputs/todo/<feature>.yaml` — shaders + offscreen execute
   - `tests/lit/todo/<feature>/<feature>.test` — `# XFAIL: *` + `# RUN: %csrun …`
4. **Implement** the feature in `src/backends/vulkan/` (and reflect if needed).
5. **Promote**:
   - move YAML → `Inputs/corpus/<feature>.yaml`
   - move test → `execute/corpus/<feature>/` and **remove** `# XFAIL: *`
6. Gate: `./tools/csrun-lit --filter '<feature>'` must **PASS**.

## Already-supported corpus

Put green samples under `execute/corpus/<name>/` (no XFAIL). That widens the regression gate without claiming new features.


## Where to pull new probes from

| Source | Path | Use for |
|--------|------|---------|
| In-tree benches | `examples/shaders/*_bench.yaml`, `examples/matmul`, `examples/vector_add` | timing / smoke regression |
| Vulkan samples | `external/shader-corpus/Vulkan/shaders/glsl/<sample>/` | feature XFAIL + green corpus |
| glslang Test | `external/shader-corpus/glslang/Test/` | compile-only / negative |
| csperf | `../tools/CompilerSutraPerfTool/examples/` | parity workloads (vector_add) |

Prefer a **Vulkan sample that hard-fails** for `todo/`. Prefer a **bench YAML** for `execute/examples/`.

## Promoted

| Feature | Now at |
|---------|--------|
| `multi_set` | `execute/corpus/multi_set/` |
| `push_constants` | `execute/corpus/push_constants/` |
| `texture_mipmap` | `execute/corpus/texture_mipmap/` |
| `descriptor_indexing` | `execute/corpus/descriptor_indexing/` |
| `compute_ubo` | `execute/corpus/compute_ubo/` |
| `storage_image` | `execute/corpus/storage_image/` |
| `input_attachments` | `execute/corpus/input_attachments/` (plus `subpasses/`, `dynamicrenderinglocalread/`) |
| `ray_tracing_stages` | callable / any-hit / intersection shaders bound from the declared stage list |

## Current XFAIL probes

| Folder | Missing capability |
|--------|--------------------|
| `buffer_device_address` | BDA + address push constants — **implemented** (`306a052`), pending promotion |
| `indirect_draw` | `vkCmdDrawIndirect` |
| `inline_uniform` | inline uniform blocks |
| `ray_query` | ray query / AS |
| `spherical_env` | env-map / UBO fill (black draw) |
| `gears` | real model/camera data (black draw) |

```bash
./tools/csrun-lit --filter 'todo/'          # expected fails
./tools/csrun-lit --filter 'execute/corpus' # must stay green
```

## Silent gaps (PASS today — need a reject before XFAIL)

| Gap | Notes |
|-----|-------|
| `depth_stencil` | toon/outline smoke draws color without a depth attachment |
| multi-pass | still single-subpass; the `subpassInput` path adds a second subpass, and `pbrbasic`/`deferred` still want a scene |
| ~~MRT~~ | **closed** — `SpirvReflect::colorOutputCount` sizes the render pass, blend state, clears, and readback, and every target is frame-checked individually. `deferred_mrt` was a false green: it passed with locations 1 and 2 unbound |
| ~~ray-tracing validation~~ | **closed** — the RT path now runs the same `checkFrame` bar as graphics (coverage, distinct colours, determinism) instead of "any pixel non-black". A single stray texel no longer passes |
