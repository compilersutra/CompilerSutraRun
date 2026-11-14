# AMD dual-GPU validation (M13)

Reference host used for paper smoke and M13:

| Role | Device | Notes |
|------|--------|-------|
| Discrete | Radeon RX 9060 XT (GFX1200) | PCI discrete; RADV / radeonsi / ROCm |
| Integrated | Raphael (GFX1036) | RADV perf counters often present |

## Device selection

| Backend | Discrete | Integrated |
|---------|----------|------------|
| Vulkan | `--device-index 0` (check `--list-devices`) | `--device-index 1` |
| OpenCL | `--device-index 0` (`gfx1200`) | `--device-index 1` (`gfx1036`) |
| HIP | `--device-index 0` | `--device-index 1` |
| OpenGL | `DRI_PRIME=1` | default context (omit `DRI_PRIME`) |

OpenGL does **not** honor `--device-index` yet.

Force RADV ICD (avoid llvmpipe-only enumeration):

```bash
export DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
./build/bin/csrun --list-devices
```

## Smoke

```bash
./experiments/scripts/smoke_dual_gpu.sh experiments/raw/amd-$(date +%Y%m%d)
```

Latest archived batch: `experiments/raw/amd-20261114/` (8/8 vector_add Mode B).

## Timing interpretation

- Prefer Mode B (`timing.source = gpu_timestamp`).
- JSON `stddev_ms` is **within-run** over measured iterations.
- For experiment-level uncertainty, use `experiments/scripts/multi_batch_amd.sh` (multi-run medians).
- Do not rank Vulkan vs HIP vs OpenCL from a single smoke table.

## Workloads

| Workload | Vulkan | OpenGL | OpenCL | HIP |
|----------|--------|--------|--------|-----|
| `vector_add` | yes | yes | yes | yes |
| `matmul` (SPIR-V) | yes (both GPUs) | — | — | — |

Multi-API matmul remains a follow-on under M13.
