# Workload format

Backend-independent YAML consumed by `csrun`.

## Minimal example

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

## Fields

| Field | Required | Notes |
|-------|----------|-------|
| `name` | yes | Workload id |
| `operation` | yes | e.g. `vector_add`, `matmul` |
| `dimensions` | no | `M`/`N`/`K` and/or `count` |
| `dtype` | no | scalar or `{input,output}`; default fp32 |
| `execution.warmup` | no | default 0 |
| `execution.iterations` | no | default 1; must be >= 1 |
| `validation.*` | no | tolerances, enabled flag |
| `backend` | no | preferred backend hint |

Must not contain API-specific types (`VkBuffer`, `cudaStream_t`, …).
