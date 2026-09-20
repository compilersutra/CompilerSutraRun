# CompilerSutraRun

**CLI:** `csrun`  
**Product:** [CompilerSutra](https://compilersutra.com)

Cross-API GPU workload runner: describe once in YAML, execute on real backends, validate against a CPU reference, and emit comparable timing JSON.

**Primary authoring path:** handwritten `.vert` / `.frag` / `.comp` (and other stages) → SPIR-V (glslang) → load/read on Vulkan. Kernel ops like `vector_add` sit beside that.

This is an **orchestration layer**. It does not invent a new IR, driver, or shader compiler. It reuses Vulkan, OpenGL, OpenCL, HIP/ROCm, and (planned) CUDA / D3D12 / Metal / WebGPU toolchains — and reports `UNAVAILABLE` when a path is missing. Never fakes results.

## Features

| Capability | Status |
|------------|--------|
| Workload YAML + JSON results | Done |
| Vulkan (SPIR-V / glslang) `vector_add` | Done |
| OpenGL 4.3 compute (EGL headless) | Done |
| OpenCL ICD `vector_add` | Done |
| HIP / ROCm (hiprtc) `vector_add` | Done |
| `csrun --all` across available backends | Done |
| Virtual `Backend::execute` + shared host scaffold | Done |
| CUDA / D3D12 / Metal / WebGPU / CPU / GLES | Planned (honest stubs) |

Roadmap: [`docs/milestones.md`](docs/milestones.md) · Architecture: [`docs/architecture.md`](docs/architecture.md)

## Requirements

- CMake ≥ 3.20, Ninja, C++20 compiler
- Optional: Vulkan loader + glslangValidator, OpenGL/EGL, OpenCL ICD, ROCm/HIP

## Build

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Release:

```bash
cmake --preset release && cmake --build --preset release
```

## Usage

```bash
./build/bin/csrun --list-backends
./build/bin/csrun --list-devices
./build/bin/csrun examples/vector_add/vector_add.yaml --backend vulkan
./build/bin/csrun examples/vector_add/vector_add.yaml --backend opencl
./build/bin/csrun examples/vector_add/vector_add.yaml --backend hip
./build/bin/csrun examples/vector_add/vector_add.yaml --all
./build/bin/csrun examples/shaders/triangle.yaml --backend vulkan
./build/bin/csrun examples/shaders/triangle.yaml --backend vulkan --no-show
./build/bin/csrun --compile-spv examples/shaders/triangle.vert -o /tmp/t.vert.spv
./build/bin/csrun --inspect-spv /tmp/t.vert.spv
./build/bin/csrun examples/vector_add/vector_add.yaml --format json
./build/bin/csrun --compile-spv examples/vector_add/vector_add.comp -o /tmp/vadd.spv
```

Useful flags: `--no-validate`, `--no-benchmark`, `--backend <name>`, `--log-level error`.

### CUDA note

This product builds CUDA only when the toolkit and an NVIDIA device are present (`-DCSRUN_ENABLE_CUDA=ON`). On AMD-only hosts the backend stays `UNAVAILABLE`. Test on an NVIDIA workstation or cloud GPU when implementing that path.

## Layout

```text
include/csrun/     public headers
src/               libraries + backends
tools/csrun/       CLI
examples/          sample workloads / kernels
tests/             unit + fixtures
docs/              architecture, milestones, design
prompt/            product briefs (archived)
.github/           CI, templates, CODEOWNERS
```

## Design rules

- One backend per API; common code never includes vendor headers
- `Backend::execute` is the control plane; registration is a single path
- Shared host prep lives in `VectorAddScaffold` — backends own compile/submit/timing only
- Integrate mature OSS; do not reimplement glslang / DXC / nvcc

## Related

- **CompilerSutraPerfTool** (`csperf`) — experiment / diff / report front door (separate product; integrates later via M8)
- LLVM/MLIR trees are **not** modified by this repository

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`docs/development.md`](docs/development.md).

## Security

See [`SECURITY.md`](SECURITY.md).

## License

Apache-2.0 — see [`LICENSE`](LICENSE).
