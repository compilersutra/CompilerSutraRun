# Backend design

## Principles

- One backend per GPU ecosystem (Vulkan, CUDA, D3D12, …).
- Common code never includes vendor API headers.
- Unavailable backends report `UNAVAILABLE` / `UNIMPLEMENTED` — never fake success.
- Narrow interfaces: `Backend` → device enumeration today; Compiler/Runtime/Profiler in Phase 2+.

## Registry

`BackendRegistry::createDefault()` registers honest stubs in Phase 1.

Phase 2 replaces the Vulkan stub with a real `VulkanBackend` behind `CSRUN_ENABLE_VULKAN`.

## Selection order

1. Explicit `--backend`
2. Workload `backend:` preference
3. Capability score (future)
4. Stable `defaultPriority()` tie-break
