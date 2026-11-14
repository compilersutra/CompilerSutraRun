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

Matmul remains Vulkan/SPIR-V only in-tree (not multi-API yet).

## How regenerated

```bash
export DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
./experiments/scripts/smoke_dual_gpu.sh experiments/raw/amd-20261114
```

## matmul multi-API (`256³`, Mode B) — `matmul_multi.yaml`

| Backend | Device | Validation | Median (ms) |
|---------|--------|------------|-------------|
| OpenCL | gfx1200 | passed | ~0.09 |
| OpenCL | gfx1036 | passed | ~1.86 |
| HIP | RX 9060 XT | passed | ~0.11 |
| HIP | Raphael | passed | ~1.87 |
| OpenGL | dGPU (`DRI_PRIME=1`) | passed | ~0.06 |
| OpenGL | iGPU | passed | ~0.86 |

Vulkan handwritten SPIR-V matmul remains `examples/matmul/matmul.yaml` (operation: spirv).
