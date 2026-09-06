# CompilerSutraRun

**CLI:** `csrun`  
**Brand:** [CompilerSutra](https://compilersutra.com)

Universal GPU workload portability, execution, validation, and benchmarking orchestration layer.

Describe a workload once (YAML). Discover devices/APIs. Select a backend. Orchestrate existing toolchains (Shaderc, DXC, CUDA, …). Execute. Validate against a CPU reference. Benchmark with native GPU timing. Emit comparable JSON.

This project does **not** replace Vulkan, CUDA, HIP, D3D12, LLVM, MLIR, SPIR-V Tools, or vendor drivers. It reuses them through adapters.

## Status

See **[docs/milestones.md](docs/milestones.md)** for the full roadmap (Vulkan → CUDA → `--all` → D3D12/DXIL → Metal → HIP/OpenCL → WebGPU/CPU → `csperf` integration → MLIR).

| Milestone | Status |
|-----------|--------|
| M0 Foundation + Vulkan `vector_add` | Done |
| M1 Vulkan matmul / reduction | Active |
| M2 CUDA | Planned |
| M3 `--all` cross-backend | Planned |
| M4 D3D12 + DXIL | Planned |
| M5 Metal | Planned |
| M6 HIP + OpenCL | Planned |
| M7 WebGPU + CPU | Planned |
| M8 `csperf` integration | Planned |
| M9 MLIR/LLVM layer | Future |
| M10 Platform polish | Future |

## Quick start

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
./build/bin/csrun --list-backends
./build/bin/csrun --list-devices
./build/bin/csrun examples/vector_add/vector_add.yaml --backend vulkan
./build/bin/csrun examples/vector_add/vector_add.yaml --format json
```

## Layout

- `include/csrun/` / `src/` — platform libraries
- `tools/csrun/` — CLI
- `docs/` — architecture, milestones, design
- `prompt/` — verbatim product prompts
- `tests/` — unit and workload fixtures
- `examples/` — sample workloads

## Related

- Sibling: `CompilerSutraPerfTool` (`csperf`) — CPU/GPU experiment framework (separate product)
- LLVM tree is **not** modified by this project

## License

Apache-2.0 — see `LICENSE`.
