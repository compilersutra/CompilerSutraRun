# Prompt 001 — Universal GPU platform specification (verbatim)

**Date:** 2026-09-05

---

# Universal GPU Workload Portability, Execution, Validation and Benchmarking Platform

You are a senior GPU compiler, graphics API, runtime, driver-interface, performance engineering and systems engineer.

You are working on a serious open-source-quality C++ systems project.

The goal is to build a production-quality:

**Universal GPU Workload Portability, Execution, Validation and Benchmarking Platform**

The core idea is:

> A developer describes a GPU workload once. The platform discovers available GPU devices and APIs, selects an appropriate backend, uses the existing compiler/toolchain ecosystem to produce the backend's required representation, executes the workload, validates correctness against a reference implementation, benchmarks it, and produces comparable machine-readable results.

The platform must NOT assume Vulkan is the universal solution.

It must support independent GPU ecosystems through modular backends.

---

# 1. Core philosophy

The project is NOT intended to replace:

* Vulkan
* CUDA
* HIP
* DirectX 12
* Metal
* WebGPU
* OpenCL
* LLVM
* MLIR
* SPIR-V
* DXIL
* PTX
* vendor compilers
* GPU drivers

Instead, the project is an orchestration/runtime/benchmarking layer that integrates with these ecosystems.

The most important architectural principle is:

> **Do not reinvent mature compiler, IR, API, driver or validation infrastructure. Reuse it through clean adapters.**

The platform owns:

* workload description
* workload requirements
* backend discovery
* capability detection
* backend selection
* compiler/toolchain orchestration
* execution orchestration
* resource lifetime
* synchronization abstraction
* profiling
* validation
* benchmarking
* result schema
* CLI
* reproducibility
* cross-backend comparison

The platform does NOT initially own:

* a new GPU IR
* a new shader compiler
* a new GPU driver
* a new graphics API
* a new GPU programming language

---

# 2. Existing open-source ecosystem must be reused

Before implementing functionality, inspect whether an established open-source project already provides it.

Prefer integration over reimplementation.

## Vulkan ecosystem

Use where appropriate:

### Vulkan-Headers

For Vulkan API definitions.

### Vulkan-Loader

For Vulkan runtime loading.

### Vulkan Validation Layers

Use during development and validation/debug configurations.

### SPIR-V Tools

Use for:

* SPIR-V validation
* disassembly
* assembly where useful
* optimization where appropriate
* binary inspection
* SPIR-V diagnostics

Do NOT implement our own SPIR-V validator.

### glslang

Use when GLSL/HLSL → SPIR-V compilation is required.

### Shaderc

Use when a convenient compiler/library API is appropriate.

Shaderc already integrates with glslang and SPIR-V Tools.

The Vulkan compiler pipeline should therefore look approximately like:

```text
GLSL/HLSL
   |
   v
glslang / Shaderc
   |
   v
SPIR-V
   |
   v
SPIR-V Tools
   |
   +--> validate
   +--> inspect
   +--> optimize where appropriate
   |
   v
Vulkan runtime
```

Do not recreate these components.

---

# 3. DirectX ecosystem

For DirectX 12 shader compilation, investigate and integrate:

### Microsoft DirectX Shader Compiler (DXC)

Use DXC for:

```text
HLSL
 |
 v
DXC
 |
 v
DXIL
 |
 v
DirectX 12
```

Do not implement an HLSL compiler.

The D3D12 backend should own only:

* device discovery
* adapter selection
* command queues
* command allocators
* command lists
* resources
* descriptor heaps
* pipeline state
* DXIL loading
* dispatch
* synchronization
* timestamp queries
* readback
* error handling

Initially it is acceptable for the backend to be partially implemented, but NEVER fake support.

Explicitly report:

```text
NOT_IMPLEMENTED
UNSUPPORTED
UNAVAILABLE
```

where appropriate.

---

# 4. LLVM / MLIR integration

The project must NOT immediately build a new compiler.

Instead, design a future compiler integration layer around existing LLVM/MLIR infrastructure.

Investigate and reuse:

* LLVM IR
* MLIR
* MLIR GPU dialect
* MLIR SPIR-V dialect
* MLIR LLVM dialect
* MLIR NVVM-related infrastructure
* MLIR ROCDL-related infrastructure
* MLIR AMDGPU-related infrastructure
* MLIR vector/linalg/tensor dialects where appropriate

MLIR already provides GPU abstractions and lowering infrastructure.

The long-term architecture may become:

```text
                 Workload
                    |
                    v
              MLIR pipeline
                    |
        +-----------+-----------+
        |           |           |
        v           v           v
     SPIR-V       NVVM        ROCDL/
        |           |         AMDGPU
        v           v           v
     Vulkan       PTX        ROCm/HIP
```

Potential future path:

```text
MLIR
 |
 +--> SPIR-V --> Vulkan
 |
 +--> NVVM --> LLVM --> PTX --> CUDA
 |
 +--> ROCDL/AMDGPU --> LLVM --> AMDGPU/HIP
 |
 +--> LLVM/native targets
```

However:

**DO NOT implement this compiler pipeline in the MVP.**

The MVP must integrate existing compiled artifacts first.

---

# 5. CUDA and HIP

CUDA backend:

Use the NVIDIA CUDA ecosystem rather than implementing a CUDA compiler/runtime.

Potential pipeline:

```text
CUDA source
   |
   v
NVCC / CUDA toolchain
   |
   v
PTX / CUDA module
   |
   v
CUDA Runtime / Driver API
   |
   v
GPU
```

Investigate whether Driver API or Runtime API is more appropriate for each layer.

Separate:

* device discovery
* module loading
* memory management
* kernel launch
* synchronization
* profiling
* result readback

Do not make CUDA assumptions part of the common workload model.

---

## HIP / ROCm

HIP should be treated as an independent backend/future backend.

Potential architecture:

```text
HIP source
   |
   v
HIP/ROCm compiler
   |
   v
LLVM/AMD representation
   |
   v
ROCm runtime
   |
   v
AMD GPU
```

Do not assume:

```text
AMD == Vulkan only
```

or:

```text
AMD == HIP only
```

An AMD device may expose multiple execution paths.

The backend selector must work from actual capabilities.

---

# 6. Mesa

Mesa may provide useful open-source graphics/compiler infrastructure and should be studied where relevant.

Use Mesa as:

* a reference implementation
* an integration target where technically appropriate
* a source of ideas for compiler/runtime architecture
* a potential ecosystem dependency only when there is a strong engineering reason

Do NOT copy large portions of Mesa into this project.

Do NOT create a hard dependency on Mesa merely because it is open source.

The project must remain independent of any single graphics driver implementation.

---

# 7. Other open-source projects

Before writing infrastructure, search for mature existing implementations.

Potential dependencies include:

* GoogleTest
* yaml-cpp
* nlohmann/json
* CLI11
* fmt
* spdlog

Do not automatically add every dependency.

For every dependency:

1. Determine whether the repository already uses an equivalent.
2. Check license compatibility.
3. Prefer established, maintained projects.
4. Keep dependencies minimal.
5. Document why each dependency exists.
6. Prefer system packages where appropriate.
7. Avoid vendoring huge dependencies unless required.

---

# 8. What our project actually owns

The unique layer should be:

```text
             Existing GPU Ecosystems
                      |
     +----------------+----------------+
     |                |                |
   Vulkan            CUDA            D3D12
     |                |                |
 SPIR-V             PTX             DXIL
     |                |                |
     +----------------+----------------+
                      |
                      v
            Our Platform Runtime
                      |
       +--------------+--------------+
       |              |              |
       v              v              v
   Workload       Validation      Benchmark
    Model           Engine         Engine
       |              |              |
       +--------------+--------------+
                      |
                      v
                Result Database
```

Our differentiation is the orchestration layer.

---

# 9. Target architecture

Use this high-level architecture:

```text
                         gpu-run
                            |
                            v
                    Workload Definition
                            |
                            v
                    Workload Parser
                            |
                            v
                  Common Workload Model
                            |
                            v
                 Capability / Device Model
                            |
                            v
                   Backend Selection
                            |
          +-----------------+------------------+
          |                 |                  |
          v                 v                  v
       Vulkan             CUDA              D3D12
          |                 |                  |
       SPIR-V             PTX                DXIL
          |                 |                  |
      Vulkan API       CUDA API           D3D12 API
          |                 |                  |
          +-----------------+------------------+
                            |
                            v
                       GPU Device
                            |
              +-------------+-------------+
              |                           |
              v                           v
          GPU Result                 GPU Timing
              |                           |
              +-------------+-------------+
                            |
                            v
                     Validation Engine
                            |
                            v
                    Benchmark Engine
                            |
                            v
                      Result Reporter
                            |
             +--------------+--------------+
             |                             |
             v                             v
          Terminal                         JSON
```

---

# 10. Common workload model

Create a backend-independent workload representation.

Example:

```yaml
name: matmul

operation: matmul

dimensions:
  M: 4096
  N: 4096
  K: 4096

dtype:
  input: fp32
  output: fp32

execution:
  warmup: 10
  iterations: 100

validation:
  enabled: true
  tolerance: 1e-5
```

The workload model describes:

* operation
* tensor/buffer shapes
* data types
* layouts
* dimensions
* inputs
* outputs
* execution requirements
* synchronization requirements
* validation requirements
* benchmark requirements
* backend constraints when explicitly requested

It must NOT contain Vulkan-specific concepts such as:

```text
VkBuffer
VkDescriptorSet
VkPipeline
VkCommandBuffer
```

It must NOT contain CUDA-specific concepts such as:

```text
cudaStream_t
cudaEvent_t
CUmodule
```

Those belong inside backend implementations.

---

# 11. Capability model

Introduce explicit capability negotiation.

Example:

```text
Workload Requirements
        |
        v
Capability Query
        |
        +--> Vulkan
        +--> CUDA
        +--> D3D12
        |
        v
Backend Ranking
        |
        v
Selected Backend
```

Capabilities may include:

* supported data types
* FP16
* BF16
* FP32
* FP64
* integer types
* subgroup/wave operations
* maximum buffer size
* maximum workgroup size
* required extensions
* required API versions
* required device features
* required memory
* operation support

The backend selector must never select a backend that cannot satisfy the workload requirements.

---

# 12. Backend interface

Do not put all functionality into one giant virtual class.

Instead use small interfaces.

Potential design:

```cpp
class GPUBackend {
public:
    virtual ~GPUBackend() = default;

    virtual BackendInfo info() const = 0;

    virtual std::unique_ptr<DeviceManager>
    createDeviceManager() = 0;

    virtual std::unique_ptr<Compiler>
    createCompiler() = 0;

    virtual std::unique_ptr<Runtime>
    createRuntime() = 0;

    virtual std::unique_ptr<Profiler>
    createProfiler() = 0;
};
```

Then:

```text
VulkanBackend
 ├── VulkanDeviceManager
 ├── VulkanCompiler
 ├── VulkanRuntime
 └── VulkanProfiler

CudaBackend
 ├── CudaDeviceManager
 ├── CudaCompiler
 ├── CudaRuntime
 └── CudaProfiler

D3D12Backend
 ├── D3D12DeviceManager
 ├── D3D12Compiler
 ├── D3D12Runtime
 └── D3D12Profiler
```

Do not add abstraction merely for abstraction's sake.

Keep hot execution paths concrete where possible.

---

# 13. Compiled artifact abstraction

Create a backend-neutral representation of a compiled workload.

Example:

```cpp
enum class BinaryFormat {
    SPIRV,
    PTX,
    DXIL,
    UNKNOWN
};

struct BinaryArtifact {
    BinaryFormat format;
    std::vector<std::byte> data;
    std::string entryPoint;
    std::string compiler;
    std::string compilerVersion;
};
```

The actual representation remains backend-specific.

Examples:

```text
Vulkan -> SPIR-V
CUDA   -> PTX/CUDA module
D3D12  -> DXIL
```

Do not pretend these representations are interchangeable.

---

# 14. Compiler interface

Use:

```cpp
class Compiler {
public:
    virtual ~Compiler() = default;

    virtual CompileResult compile(
        const Workload& workload,
        const DeviceInfo& device,
        const CompileOptions& options) = 0;
};
```

But the implementation should primarily orchestrate existing toolchains.

For example:

```text
VulkanCompiler
    |
    +--> Shaderc/glslang
    +--> SPIR-V Tools

CudaCompiler
    |
    +--> CUDA toolchain

D3D12Compiler
    |
    +--> DXC
```

Do not write replacement compilers.

---

# 15. Execution architecture

Separate compilation from execution:

```text
Workload
   |
   v
Compiler
   |
   v
CompiledWorkload
   |
   v
Runtime
   |
   v
Execution
```

The runtime owns:

* device resources
* command submission
* synchronization
* memory transfers
* pipeline/module objects
* execution lifecycle

The benchmark engine must not know Vulkan/CUDA/D3D12 implementation details.

---

# 16. Validation architecture

Every workload may provide a reference implementation.

Example:

```text
                 Workload
                    |
        +-----------+-----------+
        |                       |
        v                       v
  Reference CPU              GPU backend
        |                       |
        v                       v
 Reference result           GPU result
        |                       |
        +-----------+-----------+
                    |
                    v
              Comparator
                    |
             +------+------+
             |             |
            PASS          FAIL
```

Support:

* integer exact comparison
* FP32 tolerance
* FP16 tolerance
* BF16 tolerance
* relative tolerance
* absolute tolerance
* NaN handling
* Inf handling
* configurable comparison policies

Validation must be deterministic.

---

# 17. Benchmark architecture

Benchmarking is a first-class subsystem.

Collect:

* kernel execution time
* end-to-end execution time
* host-to-device transfer time
* device-to-host transfer time
* kernel launch overhead where measurable
* min
* median
* mean
* max
* standard deviation
* percentile statistics
* throughput
* bandwidth
* iterations
* warmup iterations
* validation status
* backend
* API
* GPU vendor
* GPU device
* driver
* runtime version
* compiler/toolchain
* compiler version
* workload configuration

Avoid mixing CPU wall-clock timing and GPU timestamp timing without explicitly identifying them.

For GPU timing, use the native API mechanisms:

```text
Vulkan       -> Vulkan timestamp queries
CUDA         -> CUDA events / appropriate timing mechanism
D3D12        -> timestamp queries
```

The result schema must identify the timing source.

---

# 18. Result schema

Use a stable versioned JSON schema.

Example:

```json
{
  "schema_version": 1,
  "workload": {
    "name": "matmul",
    "operation": "matmul"
  },
  "backend": {
    "name": "vulkan",
    "api_version": "1.x"
  },
  "device": {
    "vendor": "AMD",
    "name": "GPU",
    "driver": "..."
  },
  "compiler": {
    "name": "shaderc",
    "version": "..."
  },
  "execution": {
    "warmup": 10,
    "iterations": 100
  },
  "timing": {
    "source": "gpu_timestamp",
    "min_ms": 1.39,
    "median_ms": 1.42,
    "mean_ms": 1.45,
    "max_ms": 1.61,
    "stddev_ms": 0.04
  },
  "validation": {
    "status": "passed"
  }
}
```

The schema must be backend-independent.

---

# 19. CLI

Executable:

```text
gpu-run
```

Examples:

```bash
gpu-run matmul.yaml
```

```bash
gpu-run --list-devices
```

```bash
gpu-run matmul.yaml --backend vulkan
```

```bash
gpu-run matmul.yaml --backend cuda
```

```bash
gpu-run matmul.yaml --backend d3d12
```

```bash
gpu-run matmul.yaml --all
```

```bash
gpu-run matmul.yaml --benchmark
```

```bash
gpu-run matmul.yaml --validate
```

```bash
gpu-run matmul.yaml --format json
```

Additional useful commands may be introduced if justified:

```bash
gpu-run --list-backends
gpu-run --inspect matmul.yaml
gpu-run --compile matmul.yaml
gpu-run --validate-binary artifact.spv
```

Do not add commands merely for appearance.

---

# 20. Automatic backend selection

Implement:

```text
gpu-run workload.yaml
        |
        v
Parse workload
        |
        v
Detect operating system
        |
        v
Discover GPUs
        |
        v
Discover available APIs
        |
        v
Query capabilities
        |
        v
Filter incompatible backends
        |
        v
Rank compatible backends
        |
        v
Select backend
        |
        v
Compile
        |
        v
Execute
        |
        v
Validate
        |
        v
Benchmark
        |
        v
Report
```

Do not hard-code:

```text
AMD -> Vulkan
NVIDIA -> CUDA
```

Instead:

```text
GPU + OS + driver + installed runtimes + workload requirements
                      |
                      v
                capability matrix
                      |
                      v
                 backend choice
```

---

# 21. Backend registry

Use a backend registry/factory.

Example:

```text
BackendRegistry
    |
    +── VulkanBackend
    +── CudaBackend
    +── D3D12Backend
    +── Future:
          +── HIPBackend
          +── MetalBackend
          +── WebGPUBackend
          +── OpenCLBackend
```

Adding a backend should NOT require modifying workload logic.

Prefer plugin-style separation where practical, but do not introduce dynamic loading complexity in the MVP unless it provides a concrete benefit.

A static registration mechanism is acceptable initially.

---

# 22. Project structure

Use a structure similar to:

```text
gpu-platform/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── LICENSE
├── CONTRIBUTING.md
├── docs/
│   ├── architecture.md
│   ├── backend-design.md
│   ├── workload-format.md
│   ├── compiler-integration.md
│   ├── benchmarking.md
│   ├── validation.md
│   └── development.md
│
├── cmake/
│   ├── FindVulkan.cmake
│   ├── FindCUDA.cmake
│   └── dependencies/
│
├── include/
│   └── gpu_platform/
│       ├── workload/
│       ├── backend/
│       ├── device/
│       ├── compiler/
│       ├── runtime/
│       ├── benchmark/
│       ├── validation/
│       └── result/
│
├── src/
│   ├── workload/
│   ├── backend/
│   ├── device/
│   ├── compiler/
│   ├── runtime/
│   ├── benchmark/
│   ├── validation/
│   ├── result/
│   │
│   └── backends/
│       ├── vulkan/
│       ├── cuda/
│       └── d3d12/
│
├── tools/
│   └── gpu-run/
│
├── tests/
│   ├── unit/
│   ├── integration/
│   ├── validation/
│   └── workloads/
│
├── examples/
│   ├── vector_add/
│   ├── matmul/
│   └── reduction/
│
└── third_party/
```

Do not create directories that do not have an actual implementation purpose.

Do not create hundreds of placeholder files.

---

# 23. Dependency strategy

Use existing system libraries where available.

Potential dependencies:

```text
Vulkan-Headers
Vulkan-Loader
Vulkan-ValidationLayers
SPIR-V Tools
glslang
Shaderc
DXC
CUDA Toolkit
HIP/ROCm
LLVM/MLIR
GoogleTest
yaml-cpp
nlohmann/json
fmt
CLI11
```

Do not require all of them for the MVP.

Classify dependencies:

```text
Required for MVP
Optional
Backend-specific
Future
Development-only
```

For example:

### Linux Vulkan MVP

Required:

```text
C++20 compiler
CMake
Ninja
Vulkan-Headers
Vulkan Loader
Vulkan runtime/driver
SPIR-V Tools
Shader compiler
yaml-cpp
JSON library
GoogleTest
```

CUDA dependencies should be discovered only when CUDA support is enabled.

D3D12 dependencies should be Windows-specific.

Do not make Vulkan installation fail because CUDA is unavailable.

Do not make CUDA installation fail because Vulkan is unavailable.

---

# 24. CMake architecture

Use modern CMake.

Prefer:

```text
gpu_platform_core
gpu_platform_workload
gpu_platform_runtime
gpu_platform_benchmark
gpu_platform_validation

gpu_backend_vulkan
gpu_backend_cuda
gpu_backend_d3d12

gpu-run
```

Use options such as:

```text
GPU_PLATFORM_ENABLE_VULKAN
GPU_PLATFORM_ENABLE_CUDA
GPU_PLATFORM_ENABLE_D3D12
GPU_PLATFORM_ENABLE_TESTS
GPU_PLATFORM_ENABLE_MLIR
```

Backend availability should be detected cleanly.

Example:

```bash
cmake -S . -B build -G Ninja \
    -DGPU_PLATFORM_ENABLE_VULKAN=ON \
    -DGPU_PLATFORM_ENABLE_CUDA=OFF
```

The project must build even when optional backends are unavailable.

---

# 25. Error handling

Do not use exceptions indiscriminately for normal runtime failures.

Use an explicit result/error model.

Errors should distinguish:

```text
INVALID_WORKLOAD
UNSUPPORTED_OPERATION
UNSUPPORTED_DTYPE
BACKEND_UNAVAILABLE
DEVICE_NOT_FOUND
COMPILATION_FAILED
RESOURCE_ALLOCATION_FAILED
EXECUTION_FAILED
VALIDATION_FAILED
BENCHMARK_FAILED
TOOLCHAIN_NOT_FOUND
DRIVER_ERROR
UNIMPLEMENTED
```

Error messages must include useful context.

Example:

```text
CUDA backend unavailable:
CUDA driver/runtime was not detected.
Install the CUDA runtime or select another backend.
```

Do not hide backend/toolchain failures.

---

# 26. Logging

Provide structured logging levels:

```text
ERROR
WARN
INFO
DEBUG
TRACE
```

Example:

```bash
gpu-run matmul.yaml --log-level debug
```

Logs must clearly identify:

```text
[workload]
[backend]
[compiler]
[runtime]
[benchmark]
[validation]
```

Avoid excessive logging in benchmark hot paths.

---

# 27. Caching

Design for compilation caching.

Example:

```text
Workload
   +
Backend
   +
Device capability
   +
Compiler version
   +
Compiler options
   +
Source/artifact hash
        |
        v
     Cache Key
        |
        v
Compiled Artifact
```

Never reuse an incompatible binary simply because the workload name matches.

Cache invalidation must account for relevant compiler/toolchain/device information.

Caching can be implemented after the first working vertical slice.

---

# 28. Security and robustness

Treat workload files and external compiler outputs as untrusted inputs.

Avoid:

* unsafe shell command construction
* arbitrary command injection
* uncontrolled temporary paths
* unchecked binary sizes
* unbounded allocations
* accidental execution of arbitrary files
* assuming compiler tools exist at hard-coded paths

Prefer subprocess execution APIs that avoid shell interpretation where possible.

---

# 29. MVP

Do NOT implement the whole vision immediately.

The first real vertical slice is:

```text
gpu-run matmul.yaml
        |
        v
Parse YAML
        |
        v
Create Workload
        |
        v
Discover Vulkan GPU
        |
        v
Select Vulkan
        |
        v
Compile/load SPIR-V
        |
        v
Validate SPIR-V
        |
        v
Create Vulkan resources
        |
        v
Create compute pipeline
        |
        v
Dispatch
        |
        v
Synchronize
        |
        v
Read result
        |
        v
CPU reference
        |
        v
Validate
        |
        v
GPU timing
        |
        v
Benchmark statistics
        |
        v
JSON result
```

This must actually execute on a real GPU.

No fake backend.

No simulated GPU output.

No hard-coded benchmark numbers.

---

# 30. MVP workload

Start with:

```text
vector_add
```

before matmul if necessary because it provides a smaller vertical slice.

Then implement:

```text
matmul
```

The implementation order should be:

```text
1. vector_add
2. matmul
3. reduction
```

This allows the runtime architecture to mature incrementally.

---

# 31. Vulkan implementation

The Vulkan backend should initially support compute only.

Implement:

* instance creation
* validation layers in development mode
* physical-device enumeration
* logical-device creation
* queue selection
* command pool
* command buffer
* storage buffers
* descriptor set layout
* descriptor pool
* descriptor sets
* compute pipeline
* pipeline layout
* dispatch
* synchronization
* staging/readback
* GPU timestamps
* cleanup

Use RAII.

Prefer a small number of well-designed classes.

Do not create a giant `VulkanContext` class containing everything.

---

# 32. SPIR-V handling

The Vulkan backend should accept either:

1. precompiled SPIR-V
2. source that can be compiled through the configured shader compiler

Pipeline:

```text
Shader Source
     |
     v
Shaderc/glslang
     |
     v
SPIR-V
     |
     v
SPIR-V Tools validation
     |
     v
Vulkan
```

If SPIR-V is supplied directly:

```text
SPIR-V
   |
   v
SPIR-V Tools validation
   |
   v
Vulkan
```

Do not implement SPIR-V parsing yourself unless the project genuinely requires metadata that existing tools cannot provide.

---

# 33. CUDA MVP

After Vulkan vector_add works:

```text
gpu-run vector_add.yaml --backend cuda
```

Implement:

* CUDA device discovery
* device properties
* module/kernel loading
* device allocation
* host/device transfer
* kernel launch
* synchronization
* timing
* readback
* validation

Use the existing CUDA ecosystem.

Do not write a CUDA compiler.

---

# 34. Cross-backend execution

Once Vulkan and CUDA independently work:

```bash
gpu-run vector_add.yaml --all
```

should produce:

```text
Backend      Device             Median       Validation
---------------------------------------------------------
Vulkan       AMD GPU             X.XX ms      PASS
CUDA         NVIDIA GPU          X.XX ms      PASS
```

If a backend is unavailable:

```text
Vulkan       AMD GPU             X.XX ms      PASS
CUDA         unavailable         --           SKIPPED
D3D12        unavailable         --           SKIPPED
```

Do not treat unavailable backends as failures unless the user explicitly requested that backend.

---

# 35. DirectX 12

Create the architecture early, but do not block Linux development.

The D3D12 backend must eventually use:

```text
HLSL
 |
 v
DXC
 |
 v
DXIL
 |
 v
D3D12
```

Support eventually:

* DXGI adapter discovery
* D3D12 device
* command queue
* command allocator
* command list
* resources
* descriptor heaps
* root signatures
* compute pipeline
* DXIL
* dispatch
* fences
* timestamp queries
* readback
* validation

For the initial Linux MVP:

```text
D3D12 backend = unavailable/not built
```

Do not fabricate implementation.

---

# 36. Future architecture

The long-term architecture should support:

```text
                    Common Workload
                           |
              +------------+-------------+
              |            |             |
              v            v             v
             MLIR       Native Source   Existing IR
              |
       +------+-------+-------+--------+
       |              |       |        |
       v              v       v        v
    SPIR-V          NVVM    ROCDL     ...
       |              |       |
       v              v       v
    Vulkan          PTX    AMDGPU
                      |       |
                      v       v
                    CUDA    ROCm
```

Future backends:

```text
Metal
WebGPU
OpenCL
HIP
```

Potential future frontend languages:

```text
GLSL
HLSL
CUDA
HIP
WGSL
custom DSL
```

But these should be integrated through existing compiler infrastructure where possible.

---

# 37. Important compiler principle

Do NOT attempt to force:

```text
Everything -> SPIR-V
```

Do NOT claim:

```text
SPIR-V runs everywhere.
```

Instead:

```text
                    Workload
                       |
       +---------------+----------------+
       |               |                |
       v               v                v
    Vulkan           CUDA             D3D12
       |               |                |
    SPIR-V            PTX              DXIL
       |               |                |
       v               v                v
    Vulkan           CUDA             D3D12
```

Each backend is responsible for producing the representation appropriate for its execution ecosystem.

---

# 38. Do not invent a universal IR

If a future common compiler layer is required, investigate:

```text
MLIR
LLVM
MLIR GPU dialect
MLIR SPIR-V dialect
MLIR NVVM
MLIR ROCDL
MLIR AMDGPU
```

before designing anything custom.

Only create a new intermediate abstraction if existing technologies cannot represent an actual platform requirement.

Even then, prefer a small semantic workload model rather than a new low-level compiler IR.

---

# 39. Testing

Testing is mandatory.

Unit tests:

```text
Workload parser
Capability matching
Backend selection
Result schema
Validation math
Benchmark statistics
Configuration
Error handling
```

Integration tests:

```text
Vulkan device discovery
SPIR-V validation
vector_add
matmul
validation
benchmarking
JSON generation
```

CUDA integration tests should only run when CUDA is available.

D3D12 integration tests should only run on appropriate Windows hardware.

CI must support:

```text
CPU-only tests
Vulkan tests where available
CUDA tests where available
D3D12 tests where available
```

Do not make GPU hardware mandatory for basic unit-test CI.

---

# 40. Reproducibility

Benchmark results must capture enough environment information to reproduce experiments.

Record:

* OS
* kernel where relevant
* CPU
* GPU
* GPU driver
* API version
* runtime version
* compiler
* compiler version
* compiler options
* workload hash
* backend
* device capabilities
* timestamp source
* warmup count
* iteration count

Eventually support:

```text
gpu-run --record
gpu-run --replay
```

but do not implement replay until the core runner is stable.

---

# 41. Documentation requirements

Before writing large amounts of implementation code:

1. Inspect the existing repository.
2. Determine language.
3. Determine build system.
4. Determine existing dependencies.
5. Determine current architecture.
6. Identify existing reusable code.
7. Identify existing GPU functionality.
8. Identify existing tests.
9. Identify existing CLI functionality.
10. Identify what should NOT be rewritten.

Then create:

```text
docs/architecture.md
docs/backend-design.md
docs/workload-format.md
docs/compiler-integration.md
docs/benchmarking.md
docs/validation.md
```

Do not overwrite working code unnecessarily.

---

# 42. Required repository assessment

Before changing code, report:

```text
Repository:
Language:
C++ standard:
Build system:
Build generator:
Existing dependencies:
Existing GPU APIs:
Existing compiler infrastructure:
Existing tests:
Existing CLI:
Existing architecture:
Reusable components:
Problems:
Missing components:
Recommended changes:
```

Then produce:

```text
Architecture assessment
        |
        v
Implementation plan
        |
        v
Dependency plan
        |
        v
Phase 1
        |
        v
Build
        |
        v
Test
        |
        v
Run
```

---

# 43. Implementation phases

Use this phased roadmap.

## Phase 0 — Repository assessment

Inspect only.

Do not make unnecessary changes.

Deliver:

* architecture assessment
* dependency assessment
* reuse assessment
* implementation plan

---

## Phase 1 — Common foundation

Implement:

* workload model
* YAML parser
* backend registry
* device model
* capability model
* result model
* error model
* configuration
* CLI skeleton

Keep the project buildable.

---

## Phase 2 — Vulkan vector_add

Implement the complete vertical slice:

```text
YAML
 ↓
Workload
 ↓
Vulkan discovery
 ↓
SPIR-V
 ↓
SPIR-V validation
 ↓
Vulkan execution
 ↓
Readback
 ↓
CPU validation
 ↓
GPU timing
 ↓
JSON
```

This is the first major success criterion.

---

## Phase 3 — Vulkan matmul

Add:

* matrix dimensions
* CPU reference
* validation
* throughput calculation
* benchmark statistics

---

## Phase 4 — CUDA vector_add

Implement:

```text
CUDA discovery
 ↓
CUDA compilation/loading
 ↓
memory
 ↓
kernel
 ↓
timing
 ↓
validation
 ↓
JSON
```

---

## Phase 5 — CUDA matmul

Add matmul and cross-backend comparison.

---

## Phase 6 — `--all`

Implement:

```bash
gpu-run matmul.yaml --all
```

with backend-independent reporting.

---

## Phase 7 — D3D12

Implement Windows backend.

Use DXC for HLSL → DXIL.

---

## Phase 8 — Advanced compiler integration

Investigate:

```text
MLIR
LLVM
GPU dialect
SPIR-V dialect
NVVM
ROCDL
AMDGPU
```

Only implement compiler functionality that provides actual value.

---

## Phase 9 — Additional APIs

Potentially:

```text
HIP
Metal
WebGPU
OpenCL
```

Each must be a separate backend.

---

# 44. Performance principles

This is a performance engineering project.

Do not put unnecessary abstraction in hot paths.

Separate:

```text
Control plane
```

from:

```text
Execution plane
```

Control plane:

```text
parsing
backend selection
compilation
configuration
reporting
```

Execution plane:

```text
resource management
command submission
kernel dispatch
synchronization
timing
```

Do not allocate unnecessarily during repeated benchmark iterations.

Do not copy large GPU buffers unless required.

Do not include compilation time in kernel timing unless explicitly requested.

Do not include initialization time in steady-state kernel timing.

Expose:

```text
compile time
initialization time
transfer time
kernel time
total time
```

separately.

---

# 45. Benchmark modes

Support eventually:

```text
--benchmark=kernel
--benchmark=end-to-end
--benchmark=transfer
```

But keep the MVP simple.

At minimum distinguish:

```text
compile
setup
H2D
kernel
D2H
total
```

where measurable.

---

# 46. Correctness before performance

The order is:

```text
Build
 ↓
Execute
 ↓
Correctness
 ↓
Stable execution
 ↓
Timing
 ↓
Benchmark
 ↓
Optimization
```

Never optimize a workload before correctness is established.

Every benchmark result must contain validation status.

---

# 47. No fake functionality

This is extremely important.

Never write:

```cpp
return true;
```

to simulate unsupported GPU functionality.

Never write:

```text
CUDA backend initialized
```

when CUDA is unavailable.

Never generate fake timing.

Never fabricate device information.

If functionality is unavailable:

```text
UNAVAILABLE
UNIMPLEMENTED
UNSUPPORTED
```

must be reported explicitly.

---

# 48. Build requirements

Use:

```text
C++20
CMake
Ninja
```

Prefer compiler warnings:

```text
-Wall
-Wextra
-Wpedantic
```

and appropriate platform/compiler equivalents.

Enable sanitizers for development where appropriate:

```text
ASan
UBSan
TSan
```

Do not enable incompatible sanitizers blindly for GPU integration binaries.

---

# 49. Code quality

Use:

* RAII
* strong types
* enum class
* constexpr where useful
* explicit ownership
* narrow interfaces
* deterministic tests
* thread-safe components where required
* const correctness
* move semantics
* clear error propagation

Avoid:

* global mutable state
* giant manager classes
* unnecessary inheritance
* unnecessary templates
* macros where normal C++ is better
* backend-specific code in common modules
* excessive abstraction
* premature plugin systems
* premature MLIR integration

---

# 50. Final engineering objective

The finished platform should eventually make this possible:

```bash
gpu-run matmul.yaml --all
```

and produce something like:

```text
Universal GPU Workload Runner

Workload: matmul
M=4096 N=4096 K=4096
dtype=fp32

Backend      Device              Compiler       Median       Validation
-----------------------------------------------------------------------
Vulkan       AMD Radeon ...      Shaderc        1.42 ms      PASS
CUDA         NVIDIA ...          NVCC           0.91 ms      PASS
D3D12        NVIDIA ...          DXC            1.18 ms      PASS
```

And:

```bash
gpu-run matmul.yaml --format json
```

must produce a stable machine-readable representation.

---

# 51. The key product boundary

The project should be thought of as:

```text
             Existing Open Source Ecosystem
                           |
     +----------+----------+----------+----------+
     |          |          |          |          |
   LLVM/MLIR  SPIR-V     DXC       CUDA      ROCm
     |          |          |          |          |
     +----------+----------+----------+----------+
                           |
                           v
                OUR PLATFORM
                           |
       +-------------------+-------------------+
       |                   |                   |
   Workload             Runtime           Benchmark
    Model               Engine              Engine
       |                   |                   |
       +-------------------+-------------------+
                           |
                           v
                     gpu-run CLI
```

The project should therefore focus engineering effort on the missing layer instead of recreating technology that already exists.

---

# 52. FIRST ACTION — DO THIS NOW

Before implementing anything:

1. Inspect the entire repository.
2. Identify existing source files.
3. Identify existing build files.
4. Identify existing dependencies.
5. Identify existing GPU code.
6. Identify existing Vulkan/CUDA/LLVM/MLIR integration.
7. Identify existing tests.
8. Identify reusable components.
9. Identify obsolete or duplicated components.
10. Do NOT rewrite existing working code.

Then create:

```text
docs/architecture.md
```

containing:

1. Current repository state
2. Existing architecture
3. Proposed architecture
4. Open-source components to reuse
5. Dependency graph
6. Backend architecture
7. Workload model
8. Compiler integration strategy
9. Runtime architecture
10. Validation architecture
11. Benchmark architecture
12. Error model
13. Testing strategy
14. Security considerations
15. Future MLIR architecture

Then create a concrete:

```text
Phase 1 implementation plan
```

with exact files to add/change.

Only after this assessment should implementation begin.

---

# 53. Absolute rule

Do not generate a huge theoretical framework.

Do not create hundreds of empty classes.

Do not create fake backends.

Do not implement a new compiler IR.

Do not duplicate SPIR-V Tools.

Do not duplicate glslang.

Do not duplicate Shaderc.

Do not duplicate DXC.

Do not duplicate LLVM.

Do not duplicate MLIR.

Do not duplicate CUDA.

Do not duplicate Vulkan.

Do not duplicate Mesa.

Use mature open-source projects as building blocks.

Build the missing orchestration layer.

The first goal is a **small, real, executable vertical slice**:

```text
gpu-run vector_add.yaml
        |
        v
Vulkan GPU discovery
        |
        v
SPIR-V compilation
        |
        v
SPIR-V validation
        |
        v
Vulkan execution
        |
        v
CPU reference validation
        |
        v
GPU timestamp benchmark
        |
        v
JSON result
```

Then:

```text
CUDA
```

Then:

```text
--all
```

Then:

```text
D3D12
```

Then:

```text
MLIR / LLVM integration
```

Then:

```text
HIP
Metal
WebGPU
OpenCL
```

The long-term objective is:

> **One GPU workload → multiple GPU APIs → multiple GPU vendors → native compiler ecosystems → execution + correctness validation + performance benchmarking in one open-source platform.**
# 53. LLVM-QUALITY DESIGN AND ENGINEERING STANDARD

This project must be engineered with a code-quality standard comparable to a mature LLVM-class infrastructure project.

Do NOT merely make the code "modern C++."

The objective is:

> **LLVM-style engineering discipline: explicit ownership, strong layering, predictable interfaces, minimal hidden state, reusable components, efficient abstractions, testability, and long-term maintainability.**

Do not copy LLVM implementation details blindly.

Study and follow the engineering principles used by LLVM where they are appropriate.

---

# 54. Design-pattern philosophy

Use design patterns only when they solve a real architectural problem.

Do NOT add design patterns for appearance.

The preferred patterns for this project are:

### 54.1 Strategy Pattern

Use for backend selection/execution strategies where appropriate.

```text
BackendStrategy
      |
      +---- VulkanStrategy
      +---- CudaStrategy
      +---- D3D12Strategy
```

However, avoid introducing an extra virtual call inside hot execution loops.

The strategy abstraction should exist at the control/configuration boundary.

---

### 54.2 Factory Pattern

Use factories for backend/runtime/compiler creation.

Example:

```cpp
class BackendFactory {
public:
    static std::unique_ptr<GPUBackend>
    create(BackendType type);
};
```

Prefer explicit factories over global singleton registries.

---

### 54.3 Registry Pattern

Use a registry for discovering available backends.

Conceptually:

```text
BackendRegistry
    |
    +-- Vulkan
    +-- CUDA
    +-- D3D12
    +-- HIP
    +-- Metal
    +-- WebGPU
```

The registry should support:

```cpp
registerBackend(...)
findBackend(...)
enumerateBackends(...)
```

Keep registration deterministic.

Avoid hidden static initialization where it makes debugging or dependency ordering difficult.

---

### 54.4 Adapter Pattern

Use adapters to integrate external ecosystems.

Examples:

```text
ShadercAdapter
SPIRVToolsAdapter
DXCAdapter
LLVMAdapter
MLIRAdapter
CUDAToolchainAdapter
ROCmAdapter
```

The platform should NOT expose third-party APIs throughout the entire codebase.

Instead:

```text
Our Interface
     |
     v
Adapter
     |
     v
External Library
```

This is especially important for compiler/toolchain dependencies.

---

### 54.5 Facade Pattern

Use a small facade at the CLI/application boundary.

Example:

```text
gpu-run
   |
   v
PlatformRuntime
   |
   +-- WorkloadManager
   +-- BackendSelector
   +-- CompilerManager
   +-- ExecutionManager
   +-- ValidationManager
   +-- BenchmarkManager
```

The CLI should not directly manipulate Vulkan/CUDA/D3D12 objects.

---

### 54.6 RAII

RAII is mandatory for resource ownership.

Examples:

```cpp
class VulkanBuffer;
class VulkanCommandPool;
class VulkanCommandBuffer;
class CudaModule;
class CudaDeviceMemory;
```

Resources must have clear ownership and deterministic destruction.

Avoid manual:

```cpp
create()
...
destroy()
```

pairs spread throughout unrelated code.

Use RAII wrappers where appropriate.

---

### 54.7 PImpl

Use PImpl selectively.

It may be appropriate for:

* hiding heavy third-party headers
* reducing compile dependencies
* stabilizing public interfaces
* isolating platform-specific implementation

Do NOT use PImpl everywhere.

---

### 54.8 Value-oriented design

Prefer small value types for:

* dimensions
* data types
* backend identifiers
* device capabilities
* benchmark configuration
* validation configuration
* workload descriptions
* result metadata

Example:

```cpp
struct Dimensions {
    uint64_t m;
    uint64_t n;
    uint64_t k;
};
```

Avoid pointer-heavy designs for simple immutable data.

---

# 55. LLVM-inspired class design

Classes should generally have:

* one clear responsibility
* explicit ownership
* small public interfaces
* private implementation details
* predictable lifetime
* minimal mutable state

Avoid classes such as:

```text
UniversalGPUManager
GPUEverythingManager
MegaRuntime
BackendManagerManager
```

Do not create giant classes containing every subsystem.

Prefer:

```text
WorkloadParser
BackendRegistry
CapabilityMatcher
BackendSelector
Compiler
Runtime
Profiler
Validator
BenchmarkRunner
ResultWriter
```

with clear responsibilities.

---

# 56. LLVM-style layering

Enforce strict dependency direction.

Preferred:

```text
Application
    |
    v
Runtime orchestration
    |
    +---- Workload
    +---- Backend
    +---- Compiler
    +---- Validation
    +---- Benchmark
    |
    v
Backend implementations
    |
    v
External APIs/toolchains
```

Common layers must NOT depend on:

```text
VkBuffer
VkDevice
cudaStream_t
ID3D12Device
```

For example:

```text
include/gpu_platform/workload/
```

must not include:

```cpp
#include <vulkan/vulkan.h>
```

The common workload model must remain API-independent.

---

# 57. Dependency inversion

Common code must depend on interfaces/semantic contracts, not concrete GPU APIs.

Bad:

```cpp
class Workload {
    VkBuffer buffer;
};
```

Good:

```cpp
class Workload {
    BufferSpec output;
};
```

Then:

```text
VulkanRuntime
    |
    +--> BufferSpec -> VkBuffer

CudaRuntime
    |
    +--> BufferSpec -> CUDA allocation

D3D12Runtime
    |
    +--> BufferSpec -> ID3D12Resource
```

---

# 58. Avoid inheritance-heavy architecture

Do not build a deep inheritance hierarchy.

Avoid:

```text
GPUObject
  |
  +-- GPUResource
       |
       +-- GPUBuffer
            |
            +-- VulkanBuffer
                 |
                 +-- ...
```

unless there is a real semantic requirement.

Prefer:

* narrow interfaces
* composition
* value types
* backend-local concrete classes

Use virtual dispatch primarily at subsystem boundaries.

Do not use virtual calls in performance-critical benchmark loops unless necessary.

---

# 59. Backend isolation

Each backend must be independently compilable where practical.

Example:

```text
gpu_backend_vulkan
gpu_backend_cuda
gpu_backend_d3d12
```

The common code should not need to know internal backend details.

A backend should expose only:

```text
DeviceManager
Compiler
Runtime
Profiler
CapabilityProvider
```

Internally it may have any number of concrete implementation classes.

---

# 60. External dependency isolation

Do not allow third-party libraries to leak unnecessarily into public APIs.

Bad:

```cpp
std::optional<shaderc::CompileResult>
```

Good:

```cpp
CompileResult
```

Internally:

```text
CompileResult
      |
      v
ShadercAdapter
      |
      v
shaderc
```

Likewise:

```text
SPIRVToolsAdapter
DXCAdapter
CUDAAdapter
LLVMAdapter
MLIRAdapter
```

should translate external errors/results into platform-native types.

This prevents the entire codebase from becoming coupled to one dependency.

---

# 61. Error handling

Use explicit, structured error handling.

Prefer an LLVM-like style of returning errors rather than relying on exceptions for normal expected failures.

For example:

```cpp
Expected<CompiledWorkload>
compile(const Workload&, const CompileOptions&);
```

or an equivalent project-local `Result<T>` / `Expected<T>` implementation if an existing dependency is not appropriate.

Errors should carry:

* error category
* human-readable message
* backend
* operation
* optional underlying error
* relevant context

Example:

```text
CompilationFailed
Backend: Vulkan
Compiler: shaderc
Stage: compute
EntryPoint: main
Reason: ...
```

Do not discard underlying errors.

---

# 62. Assertions and invariants

Use assertions for programmer invariants.

Use runtime errors for external/environment failures.

Example:

```cpp
assert(device.isValid());
```

for an invariant that should be impossible to violate.

But:

```text
CUDA driver unavailable
```

must be a runtime error, not an assertion.

Document important invariants.

---

# 63. Strong types

Avoid primitive obsession.

Do not pass:

```cpp
int backend;
int device;
int iterations;
double tolerance;
```

everywhere.

Prefer:

```cpp
enum class BackendType;

struct DeviceId;
struct WorkloadId;

struct BenchmarkConfig;
struct ValidationConfig;
```

Use strong types where they prevent invalid combinations.

---

# 64. Enums

Use:

```cpp
enum class BackendType {
    Vulkan,
    CUDA,
    D3D12,
    HIP,
    Metal,
    WebGPU,
    OpenCL
};
```

Avoid unscoped enums.

Provide explicit string conversion for CLI/JSON.

Do not rely on enum integer values as persistent serialization formats.

---

# 65. Ownership rules

Every resource must have exactly one obvious owner.

Document ownership for:

* Vulkan device
* Vulkan command pool
* buffers
* descriptor resources
* pipelines
* CUDA modules
* CUDA memory
* D3D12 resources
* compiler artifacts

Prefer:

```cpp
std::unique_ptr
```

for exclusive ownership.

Use:

```cpp
std::shared_ptr
```

only when shared ownership is actually required.

Do not use smart pointers merely because they are modern C++.

For lightweight non-owning references, use references/pointers/span/views appropriately.

---

# 66. Avoid unnecessary allocations

Especially in:

```text
benchmark loop
execution loop
dispatch loop
validation loop
```

Do not repeatedly allocate vectors, strings or objects unnecessarily.

Preallocate where appropriate.

Example:

```text
Benchmark setup
    |
    +-- allocate resources
    +-- prepare command
    |
    v
Benchmark loop
    |
    +-- submit
    +-- timestamp
    +-- synchronize
```

The benchmark loop should contain only operations necessary for the benchmark being measured.

---

# 67. Compile-time versus runtime abstraction

Use compile-time mechanisms when they provide real benefits.

Use runtime polymorphism where backend selection genuinely occurs at runtime.

For example:

```text
Backend selection
    |
    v
runtime polymorphism
```

is appropriate.

But inside:

```text
Vulkan command submission
```

do not repeatedly dispatch through generic interfaces if concrete types can be used safely.

---

# 68. API stability

Treat public headers as contracts.

Keep implementation details out of public headers.

Avoid unnecessary includes.

Prefer forward declarations where appropriate.

Public headers should be lightweight.

For example:

```text
include/gpu_platform/workload/
```

should not transitively include:

```text
vulkan.h
cuda.h
d3d12.h
llvm headers
```

unless absolutely required.

---

# 69. Header hygiene

Follow strict include discipline.

Every `.cpp` file should include its corresponding header first where practical.

Avoid relying on transitive includes.

Avoid:

```cpp
#include "everything.h"
```

umbrella headers unless genuinely useful.

Do not expose implementation-only dependencies through public headers.

---

# 70. Namespace discipline

Use one project namespace.

For example:

```cpp
namespace gpu_platform {
```

Then subsystem namespaces where useful:

```cpp
namespace gpu_platform::workload
namespace gpu_platform::backend
namespace gpu_platform::vulkan
namespace gpu_platform::cuda
namespace gpu_platform::benchmark
```

Do not pollute the global namespace.

Never put:

```cpp
using namespace std;
```

in headers.

Prefer explicit standard-library names in public interfaces.

---

# 71. File naming

Use consistent LLVM-like naming.

Prefer:

```text
Workload.h
Workload.cpp

Backend.h
Backend.cpp

BackendRegistry.h
BackendRegistry.cpp

VulkanRuntime.h
VulkanRuntime.cpp
```

or the repository's existing naming convention if one already exists.

Do not mix:

```text
workload.hpp
Workload.h
gpu_workload.hxx
```

randomly.

If the existing repository already has a convention, preserve it.

---

# 72. Formatting

Use a project-wide `.clang-format`.

Use:

* clang-format
* clang-tidy where practical
* compiler warnings
* static analysis

Do not introduce formatting changes unrelated to the current feature.

Do not reformat the entire repository merely because a new style was chosen.

---

# 73. LLVM-style source organization

Prefer implementation files that are readable from top to bottom.

Typical structure:

```cpp
//===----------------------------------------------------------------------===//
// Includes
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Implementation
//===----------------------------------------------------------------------===//
```

Use section comments where they improve navigation.

Do not add excessive decorative comments.

Comments should explain:

* why something exists
* important invariants
* non-obvious GPU/API behavior
* synchronization requirements
* lifetime assumptions
* performance constraints

Do NOT comment obvious code.

Bad:

```cpp
// Increment i
i++;
```

Good:

```cpp
// Keep the staging allocation alive until the queue has completed
// the copy operation.
```

---

# 74. Documentation comments

Public APIs should have useful documentation.

Document:

* ownership
* lifetime
* thread safety
* error behavior
* synchronization expectations
* performance characteristics where relevant

Example:

```cpp
/// Executes the compiled workload.
///
/// The caller must ensure that all input resources remain valid until
/// execution completes. This method does not implicitly synchronize
/// unrelated queues.
Expected<ExecutionResult>
execute(...);
```

---

# 75. Thread safety

Explicitly document whether each component is:

```text
Thread-safe
Thread-compatible
Single-threaded
```

Do not add mutexes everywhere.

Avoid global locks.

Do not make GPU execution globally serialized unless required.

Design for future concurrent workload execution.

---

# 76. Determinism

The following must be deterministic where possible:

* workload parsing
* backend selection given identical capabilities
* JSON schema
* validation
* benchmark configuration
* test behavior

If backend selection involves ranking, define a stable ordering.

Example:

```text
explicit user backend
        >
workload preferred backend
        >
capability score
        >
stable backend priority
```

---

# 77. Testing philosophy

Tests should test behavior, not implementation details.

Prefer:

```text
Workload parses correctly
```

over:

```text
WorkloadParser::privateMethodX was called
```

Backend tests should verify:

```text
discover
compile
execute
validate
benchmark
```

where hardware is available.

---

# 78. LLVM-style testing organization

Prefer small deterministic tests.

Example:

```text
tests/
├── unit/
│   ├── workload/
│   ├── backend/
│   ├── validation/
│   └── benchmark/
│
├── integration/
│   ├── vulkan/
│   ├── cuda/
│   └── d3d12/
│
└── workloads/
    ├── vector_add.yaml
    ├── matmul.yaml
    └── reduction.yaml
```

Tests must be runnable independently.

GPU tests should gracefully report unavailable hardware rather than corrupting results.

---

# 79. No premature abstraction

This is critical.

Before creating:

```text
GenericGPUThing
AbstractGPUResource
UniversalKernel
UniversalCompilerIR
UniversalMemoryObject
```

ask:

1. What concrete problem does this abstraction solve?
2. Which backends actually share this semantic?
3. Does it reduce duplication?
4. Does it improve correctness?
5. Does it preserve backend-specific capabilities?
6. Does it introduce runtime overhead?
7. Is it stable enough to become an API?

If the answer is unclear, keep the implementation concrete.

---

# 80. Preserve backend-specific power

The common abstraction must not become the lowest common denominator.

For example:

```text
Common Workload
       |
       +--> Vulkan-specific options
       +--> CUDA-specific options
       +--> D3D12-specific options
```

must be possible through an explicit extension mechanism.

Do not force all APIs into identical semantics.

The platform should abstract common semantics while allowing backend-specific capabilities.

---

# 81. Avoid "lowest common denominator" architecture

This is a major design requirement.

Bad:

```text
GPUBackend
    |
    +-- allocate
    +-- copy
    +-- execute
```

and nothing else.

Better:

```text
Common semantics
        +
Backend capabilities
        +
Backend-specific extensions
```

For example, CUDA may expose features that Vulkan does not.

Vulkan may expose features that D3D12 does not.

The abstraction must not prevent access to those capabilities.

---

# 82. Performance measurement integrity

Never allow the abstraction layer to silently distort benchmark results.

For example, do not benchmark:

```text
generic dispatch wrapper
+
allocation
+
logging
+
dynamic dispatch
+
GPU execution
```

when the user requested kernel execution time.

Clearly separate:

```text
Framework overhead
Backend setup
Memory transfer
Kernel execution
Synchronization
```

Where possible expose both:

```text
native GPU timing
```

and:

```text
end-to-end host timing
```

---

# 83. Build hygiene

Every major change must leave the project buildable.

After each phase:

```bash
cmake --build build
ctest --test-dir build
```

or the repository's equivalent.

Do not accumulate broken intermediate commits.

When adding dependencies:

1. Update CMake.
2. Update documentation.
3. Add dependency detection.
4. Add tests.
5. Verify clean build.

---

# 84. Code review mindset

Before considering a feature complete, review the change as if submitting it to LLVM.

Ask:

### Correctness

* Are all resources released?
* Are synchronization rules correct?
* Are errors propagated?
* Are edge cases handled?

### API design

* Is the interface minimal?
* Does ownership make sense?
* Is the abstraction actually necessary?

### Performance

* Are there unnecessary allocations?
* Are copies avoidable?
* Is synchronization excessive?
* Is benchmark timing contaminated?

### Maintainability

* Is the code understandable?
* Are dependencies isolated?
* Are public headers lightweight?

### Testing

* Is there a deterministic unit test?
* Is there an integration test where applicable?
* Are failure cases tested?

### Portability

* Does this accidentally introduce a Vulkan/CUDA assumption into common code?
* Does Linux still build when CUDA is unavailable?
* Does Windows architecture remain viable?

---

# 85. Design review before implementation

For every major subsystem, first produce a short design note:

```text
Problem
Constraints
Existing open-source solutions
Chosen design
Alternatives considered
Ownership
Threading
Performance
Error handling
Testing
Future extension
```

Do not write hundreds of lines of code before the design is understood.

---

# 86. Final coding principle

The target quality bar is:

```text
Readable
+
Minimal
+
Explicit
+
Testable
+
Performant
+
Portable
+
Maintainable
```

not:

```text
Maximum abstraction
+
Maximum templates
+
Maximum classes
```

The project should feel like infrastructure that could eventually grow to tens or hundreds of thousands of lines without collapsing under its own architecture.

When choosing between a clever abstraction and a boring explicit implementation:

> **Prefer the boring implementation unless the abstraction clearly improves long-term architecture.**

When choosing between rewriting existing code and adapting it:

> **Preserve working code and integrate incrementally.**

When choosing between implementing technology ourselves and integrating a mature open-source project:

> **Integrate the mature project.**

When choosing between performance and abstraction in the hot path:

> **Keep the abstraction outside the hot path.**
