# CompilerSutraRun (`csrun`)

[![CI](https://github.com/compilersutra/CompilerSutraRun/actions/workflows/ci.yml/badge.svg)](https://github.com/compilersutra/CompilerSutraRun/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![Release](https://img.shields.io/badge/paper%20tag-v0.1.0--paper-green.svg)](https://github.com/compilersutra/CompilerSutraRun/releases/tag/v0.1.0-paper)

Welcome to **CompilerSutraRun** — a reusable execution and validation harness
for cross-API GPU workloads.

Describe a workload once in YAML, run it on real GPU backends, check results
against a CPU reference, and emit machine-readable timing JSON with an explicit
measurement source. The CLI is `csrun`.

Part of [CompilerSutra](https://compilersutra.com).

## What this project is

CompilerSutraRun is an **orchestration layer**. It binds a portable workload
contract to concrete APIs and keeps host scaffolding (device selection,
validation, result schema, timing provenance) out of each backend.

It does **not** invent a new GPU IR, driver, or shader compiler. It reuses
existing toolchains (glslang / SPIR-V, OpenCL ICD, HIP/hiprtc, Mesa, vendor
SDKs) and reports `UNAVAILABLE` when a path is missing. It never fakes
success.

| Supported today | Stub / planned (fail-closed) |
|-----------------|------------------------------|
| Vulkan (SPIR-V) | CUDA |
| OpenGL 4.3 compute (EGL) | D3D12 |
| OpenCL | Metal |
| HIP / ROCm | WebGPU, GLES, CPU backend |

Primary authoring path for graphics/compute shaders:
handwritten `.vert` / `.frag` / `.comp` → SPIR-V (glslang) → Vulkan.
Kernel ops such as `vector_add` sit beside that path.

## Getting started

### Requirements

- CMake ≥ 3.20, Ninja, a C++20 compiler
- Optional SDKs enable backends at configure time:
  Vulkan + `glslangValidator`, OpenGL/EGL, OpenCL ICD, ROCm/HIP

### Build and test

```bash
git clone https://github.com/compilersutra/CompilerSutraRun.git
cd CompilerSutraRun

cmake --preset default
cmake --build --preset default
ctest --preset default

./build/bin/csrun --list-backends
./build/bin/csrun --list-devices
```

Release build:

```bash
cmake --preset release && cmake --build --preset release
```

CMake options follow the usual pattern (`-DCSRUN_ENABLE_<BACKEND>=ON|OFF`).
Missing toolkits become honest stubs, not silent success.

## Quick examples

```bash
# Vector add on one backend
./build/bin/csrun examples/vector_add/vector_add.yaml --backend vulkan
./build/bin/csrun examples/vector_add/vector_add.yaml --backend opencl
./build/bin/csrun examples/vector_add/vector_add.yaml --backend hip

# All available backends
./build/bin/csrun examples/vector_add/vector_add.yaml --all

# JSON results (validation + timing.source)
./build/bin/csrun examples/vector_add/vector_add.yaml --backend vulkan --format json

# SPIR-V authoring helpers
./build/bin/csrun --compile-spv examples/vector_add/vector_add.comp -o /tmp/vadd.spv
./build/bin/csrun --inspect-spv /tmp/vadd.spv
```

Useful flags: `--backend`, `--device-index`, `--no-validate`, `--no-benchmark`,
`--format json`, `--log-level error`.

### Dual-GPU hosts

Device selection is API-specific:

| Backend | How to pick a physical GPU |
|---------|----------------------------|
| Vulkan / OpenCL / HIP | `--device-index N` (see `--list-devices`) |
| OpenGL | `DRI_PRIME=1` for discrete; omit for default (often iGPU). OpenGL does not honor `--device-index` yet |

On Mesa/AMD, force a real RADV ICD when needed:

```bash
export DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
./build/bin/csrun --list-devices
```

Paper smoke (4 APIs × discrete/integrated):

```bash
./experiments/scripts/smoke_dual_gpu.sh experiments/raw/my-smoke
```

Archived batch cited in the workshop draft:
`experiments/raw/revalidate-20261020/`.

## Documentation

| Doc | Contents |
|-----|----------|
| [Architecture](docs/architecture.md) | Control plane vs backends |
| [Workload format](docs/workload-format.md) | YAML contract |
| [Timing model](docs/timing-model.md) | Mode A / Mode B, `timing.source` |
| [Validation](docs/validation.md) | CPU-reference predicate and statuses |
| [Milestones](docs/milestones.md) | Roadmap |
| [Development](docs/development.md) | Local workflow |
| [Contributing](CONTRIBUTING.md) | PR rules and backend checklist |
| [Experiments](experiments/README.md) | Paper artifact / smoke |
| [Workshop paper](papers/csrun/v2/main.pdf) | Tool-paper draft (`v0.1.0-paper`) |

## Repository layout

```text
include/csrun/     Public headers
src/               Libraries and backend adapters
tools/csrun/       CLI entry point
examples/          Sample workloads and kernels
tests/             Unit tests and fixtures
docs/              Design and contracts
experiments/       Smoke scripts and archived JSON
papers/csrun/      Workshop paper sources + PDF
.github/           CI and templates
```

## Design rules

These match how mature compiler/runtime projects stay maintainable:

1. **One backend per API** — common code never includes vendor headers.
2. **One execute path** — `Backend::execute` is the control plane; registration is singular.
3. **Shared host prep, local dispatch** — scaffolds own validation/setup; backends own compile, buffers, submit, and timers.
4. **Reuse toolchains** — glslang, SPIR-V Tools, hiprtc, NVCC, DXC; do not reimplement them.
5. **Fail closed** — stubs and missing devices report `UNAVAILABLE`, never a fake pass.

## Status

| Capability | State |
|------------|-------|
| Workload YAML + JSON results | Supported |
| Vulkan / OpenGL / OpenCL / HIP `vector_add` | Supported |
| `csrun --all` across available backends | Supported |
| Explicit `--device-index` (Vulkan/OpenCL/HIP) | Supported |
| CUDA / D3D12 / Metal / WebGPU | Stub (`UNAVAILABLE`) |
| Multi-workload paper study | Follow-on |

## Related projects

- **CompilerSutraPerfTool** (`csperf`) — experiment / diff / report front door (separate repo; integrates later)
- This tree does **not** modify LLVM/MLIR sources; it is a consumer-style harness beside those ecosystems

## Getting involved

- Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a PR
- Prefer small, focused changes; match `.clang-format`
- GPU backend changes should be smoke-tested on real hardware when available
- Security reports: [SECURITY.md](SECURITY.md)

### Acknowledgments

Thanks to [@AdityaT11](https://github.com/AdityaT11) and **Sayu** for
collaborating on CompilerSutraRun and the workshop draft. Please review the
paper and artifact **as a team** before treating the draft as submission-ready
([paper notes](papers/csrun/THANKS.md)).

## License

Licensed under the [Apache License 2.0](LICENSE).
