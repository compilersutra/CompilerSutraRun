# Shader corpus

External GLSL / SPIR-V trees used to stress **compile** coverage for CompilerSutraRun.

Upstream sources are **git submodules** under `external/shader-corpus/`. The parent repo only stores commit pointers — not the shader files.

```text
external/shader-corpus/
  README.md
  scripts/scan.sh
  glslang/     # submodule → KhronosGroup/glslang
  Vulkan/      # submodule → SaschaWillems/Vulkan
```

## Setup

After clone (or when submodules are missing):

```bash
git submodule update --init --depth 1 -- \
  external/shader-corpus/glslang \
  external/shader-corpus/Vulkan
```

Or once for every submodule in the repo:

```bash
git submodule update --init --depth 1
```

## Scan (compile gate)

```bash
./external/shader-corpus/scripts/scan.sh
# optional: CSRUN_CORPUS_LIMIT=100 ./external/shader-corpus/scripts/scan.sh
```

Looks at `glslang/Test` and `Vulkan/data/shaders`.

## Sources

| Upstream | Submodule path | Shader tree |
|----------|----------------|-------------|
| [KhronosGroup/glslang](https://github.com/KhronosGroup/glslang) | `glslang/` | `Test/` |
| [SaschaWillems/Vulkan](https://github.com/SaschaWillems/Vulkan) | `Vulkan/` | `data/shaders/` |

Full sample **applications** stay upstream. This is for **shaders**, not host demos.

## License

Submodule trees keep upstream licenses. Not CompilerSutra IP.
