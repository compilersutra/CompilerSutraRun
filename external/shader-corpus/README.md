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

Looks at `Vulkan/shaders/glsl` by default (real sample shaders).

Optional — include glslang’s Test tree (many files are **expected** compile failures):

```bash
CSRUN_CORPUS_DIRS="$PWD/external/shader-corpus/Vulkan/shaders/glsl:$PWD/external/shader-corpus/glslang/Test" \
  ./external/shader-corpus/scripts/scan.sh
```

## Sources

| Upstream | Submodule path | Shader tree |
|----------|----------------|-------------|
| [SaschaWillems/Vulkan](https://github.com/SaschaWillems/Vulkan) | `Vulkan/` | `shaders/glsl/` (default scan) |
| [KhronosGroup/glslang](https://github.com/KhronosGroup/glslang) | `glslang/` | `Test/` (optional; lots of negative tests) |

Full sample **applications** stay upstream. This is for **shaders**, not host demos.

## License

Submodule trees keep upstream licenses. Not CompilerSutra IP.
