# SPIR-V / Vulkan shader examples

Compile any stage to `.spv`, then smoke-test the Vulkan API:

```bash
# .comp / .vert / .frag → .spv (glslang + spirv-val)
./build/bin/csrun --compile-spv examples/vector_add/vector_add.comp -o build/spv/vector_add.comp.spv
./build/bin/csrun --compile-spv examples/shaders/triangle.vert -o build/spv/triangle.vert.spv
./build/bin/csrun --compile-spv examples/shaders/triangle.frag -o build/spv/triangle.frag.spv

# Load SPIR-V on a real device: create modules + pipelines
./build/bin/csrun --vulkan-smoke --spv build/spv/vector_add.comp.spv
./build/bin/csrun --vulkan-smoke --vert-spv build/spv/triangle.vert.spv --frag-spv build/spv/triangle.frag.spv
```

Supported extensions for stage detection: `.comp` `.vert` `.frag` `.tesc` `.tese` `.geom` (+ ray/mesh when toolchain supports them).
