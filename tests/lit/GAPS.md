# Feature status

Missing execute capabilities: **`tests/lit/todo/<feature>/`** (XFAIL).  
How / where to add: **`tests/lit/todo/README.md`**.

## Done (smoke)

| Feature | Where |
|---------|--------|
| UBO / sampler / vertex attrs + multi-set | `execute/corpus/` |
| Push-constant smoke defaults | `execute/corpus/push_constants/` |
| Compute UBO + SSBO + storage image | `execute/corpus/compute_ubo/`, `storage_image/` |
| Runtime descriptor arrays | `execute/corpus/descriptor_indexing/` |
| Vulkan sample corpus (instancing, parallax, deferred MRT, …) | `execute/corpus/` |
| In-tree benches (triangle/plasma/matmul/vector_add + 1080p) | `execute/examples/` |

## Compile XFAIL (glslang)

| Shader | Path |
|--------|------|
| `GL_EXT_descriptor_heap` | `compile/corpus/descriptorheapuntyped/` |
