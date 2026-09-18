# Development

## Toolchain

- CMake ≥ 3.20 + Ninja
- C++20 (`g++` / `clang++`)
- Optional backends: Vulkan, OpenGL/EGL, OpenCL, HIP/ROCm

## Configure options

| Option | Default | Meaning |
|--------|---------|---------|
| `CSRUN_ENABLE_VULKAN` | ON | Vulkan + SPIR-V path |
| `CSRUN_ENABLE_OPENGL` | ON | EGL headless OpenGL compute |
| `CSRUN_ENABLE_OPENCL` | ON | OpenCL ICD |
| `CSRUN_ENABLE_HIP` | ON | HIP/ROCm + hiprtc |
| `CSRUN_ENABLE_CUDA` | OFF | CUDA (needs toolkit + NVIDIA) |
| `CSRUN_ENABLE_D3D12` | OFF | Windows D3D12/DXIL |
| `CSRUN_ENABLE_TESTS` | ON | GoogleTest unit tests |

## Libraries

| Target | Role |
|--------|------|
| `csrun_core` | Error, Expected, Log, Process |
| `csrun_compiler` | SpirvToolchain |
| `csrun_workload` | YAML + VectorAddScaffold |
| `csrun_backend` | Registry / selector / `Backend` API |
| `csrun_runtime` | PlatformRuntime + BuiltinBackends |
| `csrun_backend_*` | Optional API implementations |

## CI

GitHub Actions builds on Ubuntu with Vulkan/OpenGL headers enabled and OpenCL/HIP off (no ROCm on runners). Unit tests must pass without a discrete GPU.

## Local GPU notes

- Prefer `/dev/dri` access (user in `video` / `render` groups).
- HIP needs `/dev/kfd` + ROCm.
- RADV may print conformance warnings to stderr; use `--log-level error` for quieter runs.
