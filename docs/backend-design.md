# Backend design

## Principles

- One backend per GPU ecosystem (Vulkan, CUDA, OpenCL, HIP, …).
- Common code never includes vendor API headers.
- Unavailable backends report `UNAVAILABLE` / `UNIMPLEMENTED` — never fake success.
- **`Backend` owns discovery and execute** — polymorphism is the control plane.

## Interface

```text
Backend
  info()
  enumerateDevices()
  execute(Workload, DeviceInfo, ExecuteOptions) -> RunResult
```

`ExecuteOptions` carries `--no-validate` / `--no-benchmark` (and force-on flags).

Stubs implement `execute` with `BackendUnavailable`. Real backends override it.

## Registration (single path)

`createDefaultBackendRegistry()` in `csrun_runtime` (`BuiltinBackends.cpp`) registers:

- real backends when `CSRUN_HAS_*` is set at build time
- honest stubs otherwise

`BackendRegistry` itself is a dumb container (register / find) — no ifdef switchboard.

## Shared host scaffolding

`VectorAddScaffold` owns the repeated host path:

- `requireVectorAddFp32`
- `makeVectorAddHost`
- `resolveKernelPath`
- `fillVectorAddResult` (Comparator + Statistics)

Backends keep only API-specific compile / submit / timing.

## Selection order

1. Explicit `--backend`
2. Workload `backend:` preference
3. Capability score (future)
4. Stable `defaultPriority()` tie-break

## Adding a backend (e.g. CUDA)

1. `csrun_backend_cuda` with `CudaBackend : Backend`
2. CMake `CSRUN_ENABLE_CUDA` / `CSRUN_HAS_CUDA`
3. One `registerBackend(createCudaBackend())` arm in `BuiltinBackends.cpp`
4. Kernel under `examples/` + scaffold helpers

No changes to `PlatformRuntime::run` — it already calls `backend->execute(...)`.
