# CompilerSutraRun

**CLI:** `csrun`  
**Brand:** [CompilerSutra](https://compilersutra.com)

Universal GPU workload portability, execution, validation, and benchmarking orchestration layer.

Describe a workload once (YAML). Discover devices/APIs. Select a backend. Orchestrate existing toolchains (Shaderc, DXC, CUDA, …). Execute. Validate against a CPU reference. Benchmark with native GPU timing. Emit comparable JSON.

This project does **not** replace Vulkan, CUDA, HIP, D3D12, LLVM, MLIR, SPIR-V Tools, or vendor drivers. It reuses them through adapters.

## Status

| Phase | Status |
|-------|--------|
| Phase 0 — Assessment | Done |
| Phase 1 — Common foundation | In progress |
| Phase 2 — Vulkan `vector_add` | Planned |
| Phase 4 — CUDA | Planned |
| Phase 7 — D3D12 | Planned |

## Quick start

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
./build/bin/csrun --help
./build/bin/csrun --list-backends
./build/bin/csrun examples/vector_add/vector_add.yaml --inspect
```

## Layout

- `include/csrun/` / `src/` — platform libraries
- `tools/csrun/` — CLI
- `docs/` — architecture and design
- `prompt/` — verbatim product prompts
- `tests/` — unit and workload fixtures
- `examples/` — sample workloads

## Related

- Sibling: `CompilerSutraPerfTool` (`csperf`) — CPU/GPU experiment framework (separate product)
- LLVM tree is **not** modified by this project

## License

Apache-2.0 — see `LICENSE`.
