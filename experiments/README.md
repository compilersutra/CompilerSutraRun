# CompilerSutraRun experiments (paper artifact)

Auditable material for the workshop tool paper (CompilerSutraRun / `csrun`).

## Research question

> Can a single workload contract reduce duplicated GPU test-harness code while
> preserving explicit correctness evidence, backend availability information,
> and the provenance needed to interpret measurements across APIs?

**Not claimed:** that Vulkan / OpenGL / OpenCL / HIP have comparable performance
on one microkernel, or that a shared YAML implies identical GPU source kernels.

## Paper smoke (archived)

| Item | Path |
|------|------|
| Cited batch | `raw/revalidate-20261020/` (8 JSON files, Mode B) |
| Regeneration script | `scripts/smoke_dual_gpu.sh` |
| Timing contract | `../docs/timing-model.md` |
| Validation contract | `../docs/validation.md` |
| Audit notes | `analysis/phase1-audit.md` |

Regenerate on an AMD dual-GPU Linux host (indices may differ—check `--list-devices`):

```bash
cmake --preset default && cmake --build --preset default
export DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
./experiments/scripts/smoke_dual_gpu.sh experiments/raw/smoke-$(date +%Y%m%d)
```

Stddev in JSON is **within-run** iteration dispersion, not multi-run IQR.
Do not rank APIs from a single smoke batch.

## Phase status

| Phase | Status | Notes |
|-------|--------|-------|
| 1 Audit | Done | `analysis/phase1-audit.md` |
| 2 Strengthen | Done for tool paper | OpenCL Mode B; device-index; validation/stub unit tests; `raw/revalidate-20261020/` |
| 3+ | Optional follow-on | Multi-workload, multi-batch timings, harness LOC baseline |
