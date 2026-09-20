# Workload format

Backend-independent YAML consumed by `csrun`.

## 1) Kernel workloads (e.g. vector_add)

```yaml
name: vector_add
operation: vector_add
dimensions:
  count: 1048576
dtype:
  input: fp32
  output: fp32
execution:
  warmup: 10
  iterations: 100
validation:
  enabled: true
  tolerance: 1.0e-5
```

## 2) Handwritten shaders → SPIR-V → Vulkan (primary authoring path)

Write your own `.vert` / `.frag` / `.comp` (and other stages), list them in YAML:

```yaml
name: triangle_handwritten
operation: spirv          # aliases: shader, shader_pipeline, vulkan_shaders
backend: vulkan
shaders:
  - triangle.vert
  - triangle.frag
# or map form:
# shaders:
#   vertex: triangle.vert
#   fragment: triangle.frag
#   compute: my_kernel.comp
# spv_output_dir: spv
execution:
  iterations: 1
validation:
  enabled: false
```

What `csrun` does:

1. Resolve paths next to the YAML
2. `glslangValidator` → `.spv` (+ `spirv-val`)
3. Read SPIR-V header (version / words / bound); optional `spirv-dis` head
4. Load modules on a real Vulkan device and create compute or graphics pipeline

```bash
./build/bin/csrun examples/shaders/triangle.yaml --backend vulkan
./build/bin/csrun --compile-spv examples/shaders/triangle.vert -o /tmp/t.vert.spv
./build/bin/csrun --inspect-spv /tmp/t.vert.spv
./build/bin/csrun --vulkan-smoke --vert-spv /tmp/t.vert.spv --frag-spv /tmp/t.frag.spv
```

## Fields

| Field | Required | Notes |
|-------|----------|-------|
| `name` | yes | Workload id |
| `operation` | yes | `vector_add`, `spirv` / `shader`, … |
| `shaders` | for spirv | list of paths or `{stage: path}` map |
| `spv_output_dir` | no | where to write `.spv` |
| `dimensions` | no | `M`/`N`/`K` (matmul) and/or `count` (vector) |
| `dtype` | no | scalar or `{input,output}`; default fp32 |
| `execution.*` | no | warmup / iterations / show / **width** / **height** (or `extent: [W,H]`) |
| `validation.*` | no | tolerances, enabled flag |
| `backend` | no | preferred backend hint |

Must not embed API handles (`VkBuffer`, …) in YAML — only paths and portable fields.
