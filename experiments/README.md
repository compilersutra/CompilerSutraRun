# CompilerSutraRun experiments (paper + M13 artifact)

Auditable material for the workshop tool paper and **M13 AMD dual-GPU validation**.

## Research question

> Can a single workload contract reduce duplicated GPU test-harness code while
> preserving explicit correctness evidence, backend availability information,
> and the provenance needed to interpret measurements across APIs?

**Not claimed:** that Vulkan / OpenGL / OpenCL / HIP have comparable performance
on one microkernel, or that a shared YAML implies identical GPU source kernels.

## AMD dual-GPU (M13) — current

| Item | Path |
|------|------|
| Latest 8-cell smoke | `raw/amd-20261114/` (vector_add Mode B, 8/8 passed) |
| Summary | `raw/amd-20261114/SUMMARY.md` |
| Matmul (Vulkan both GPUs) | `raw/amd-20261114/matmul-vulkan/` |
| Matmul multi-API | `raw/amd-20261114/matmul-multi/` |
| Multi-batch example | `raw/amd-20261114/multi-batch-vulkan-dgpu/` |
| Dual-GPU docs | `../docs/amd-dual-gpu.md` |
| Smoke script | `scripts/smoke_dual_gpu.sh` |
| Multi-run script | `scripts/multi_batch_amd.sh` |

```bash
cmake --preset default && cmake --build --preset default
export DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
./experiments/scripts/smoke_dual_gpu.sh experiments/raw/amd-$(date +%Y%m%d)
./experiments/scripts/multi_batch_amd.sh 5 vulkan 0
```

JSON `stddev_ms` = **within-run**. Multi-batch script reports **stdev-of-medians** (experiment-level).

## Paper smoke

| Item | Path |
|------|------|
| Cited batch (current) | `raw/amd-20261114/` |
| Prior archived batch | `raw/revalidate-20261020/` |
| Timing contract | `../docs/timing-model.md` |
| Validation contract | `../docs/validation.md` |
| Audit notes | `analysis/phase1-audit.md` |

## Phase status

| Phase | Status | Notes |
|-------|--------|-------|
| 1 Audit | Done | `analysis/phase1-audit.md` |
| 2 Strengthen | Done for tool paper | OpenCL Mode B; device-index |
| M13 AMD dual-GPU | **Done (smoke)** | `raw/amd-20261114/`; vector_add + matmul multi-API |
| CUDA (M2) | Deferred | No local NVIDIA; cloud GPU quota pending |
