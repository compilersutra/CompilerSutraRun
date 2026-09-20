# Handwritten shaders → SPIR-V → Vulkan

Industry path:

```text
.vert / .frag / .comp
        │
        ▼
  glslangValidator (+ spirv-val)
        │
        ▼
      .spv
        │
   ┌────┴────┐
   │ show    │ --no-show / CI
   ▼         ▼
 Xlib WSI   offscreen
 surface    color target
 swapchain  vkCmdDraw
 present    + readback
```

## Quick use

```bash
# YAML workload — WSI present (window) or headless bench
./build/bin/csrun examples/shaders/triangle.yaml --backend vulkan
./build/bin/csrun examples/shaders/triangle_bench.yaml --backend vulkan --format json

./build/bin/csrun --compile-spv examples/shaders/triangle.vert -o build/spv/triangle.vert.spv
./build/bin/csrun --inspect-spv build/spv/triangle.vert.spv
```

YAML:

```yaml
execution:
  show: true      # WSI present (default)
  show_ms: 3000   # present duration; Esc / close also exits
```

Stage detection from extension: `.comp` `.vert` `.frag` `.tesc` `.tese` `.geom`.
