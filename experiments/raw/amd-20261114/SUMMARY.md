# AMD dual-GPU smoke — 2026-11-14

Host: RX 9060 XT (GFX1200) + Raphael iGPU (GFX1036), Mesa RADV / radeonsi, ROCm OpenCL/HIP.

## vector_add (`N=1048576`, Mode B)

| Label | Validation | Median (ms) | Source | Device |
|-------|------------|-------------|--------|--------|
| vulkan-dgpu | passed | 0.391 | gpu_timestamp | RX 9060 XT |
| vulkan-igpu | passed | 0.385 | gpu_timestamp | Raphael |
| hip-dgpu | passed | 0.048 | gpu_timestamp | RX 9060 XT |
| hip-igpu | passed | 0.394 | gpu_timestamp | Raphael |
| opencl-dgpu | passed | 0.020 | gpu_timestamp | gfx1200 |
| opencl-igpu | passed | 0.390 | gpu_timestamp | gfx1036 |
| opengl-dgpu | passed | 0.019 | gpu_timestamp | RX 9060 XT (`DRI_PRIME=1`) |
| opengl-igpu | passed | 0.382 | gpu_timestamp | Raphael |

**8/8 passed.** Medians are not a cross-API ranking. Stddev in JSON is within-run iteration dispersion.

## matmul Vulkan (`256³`, Mode B)

| Device | Validation | Median (ms) | Source |
|--------|------------|-------------|--------|
| dGPU | passed | 0.201 | gpu_timestamp |
| iGPU | passed | 1.032 | gpu_timestamp |

Vulkan matmul uses handwritten SPIR-V (`examples/matmul/matmul.yaml`).
OpenCL/HIP/OpenGL matmul uses `MatmulScaffold` (`matmul_multi.yaml`); see below.

## How regenerated

```bash
export DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
./experiments/scripts/smoke_dual_gpu.sh experiments/raw/amd-20261114
```

## matmul multi-API (`256³`, Mode B) — `matmul_multi.yaml`

| Backend | Device | Validation | Median (ms) |
|---------|--------|------------|-------------|
| OpenCL | gfx1200 | passed | 0.089 |
| OpenCL | gfx1036 | passed | 1.869 |
| HIP | RX 9060 XT | passed | 0.112 |
| HIP | Raphael | passed | 1.983 |
| OpenGL | dGPU (`DRI_PRIME=1`) | passed | 0.064 |
| OpenGL | iGPU | passed | 0.868 |

Vulkan handwritten SPIR-V matmul remains `examples/matmul/matmul.yaml` (operation: spirv).

## multi-batch (Vulkan dGPU vector_add, 5 process restarts)

Per-process medians (ms): 0.377, 0.404, 0.380, 0.375, 0.376.
Mean of medians 0.382 ms; sample stdev 0.012 ms. Illustrative only—not a ranking.
