# Execute breadth matrix (Vulkan corpus)

One representative workload **per sample folder**. **Scan breadth-first**, then pick features by bucket size.

```bash
. scripts/env-mesa-main-radv.sh
./tools/csrun-breadth
```

Last run: **PASS=78 FAIL=17 SKIP=0** (137.1s)

## Buckets (priority = fail count)

| Bucket | Count | Meaning | Next action |
|--------|------:|---------|-------------|
| `PASS` | 78 | Works today | Keep in execute/corpus regression |
| `FAIL_BLACK` | 7 | Runs but black / wrong | data: push/UBO/draw path |
| `FAIL_STAGE` | 4 | Missing pipeline stage | todo: geometry / tess / mesh / raytracing |
| `FAIL_OTHER` | 3 | other | inspect .cache/breadth-scan/<sample>.log |
| `FAIL_COMPILE` | 1 | glslang fail | compile XFAIL or toolchain |
| `FAIL_COMPUTE_RES` | 1 | compute resources | extend compute generic |
| `FAIL_DESC_KIND` | 1 | Unknown descriptor type | todo: ray_query / accel / new kinds |

## Samples by bucket

### PASS (78)

- `base`
- `bloom`
- `computecloth`
- `computecullandlod`
- `computeheadless`
- `computenbody`
- `computeparticles`
- `computeraytracing`
- `computeshader`
- `conditionalrender`
- `conservativeraster`
- `debugprintf`
- `debugutils`
- `deferredmultisampling`
- `deferredshadows`
- `descriptorbuffer`
- `descriptorheap`
- `descriptorindexing`
- `descriptorsets`
- `distancefieldfonts`
- `dynamicrendering`
- `dynamicrenderinglocalread`
- `dynamicuniformbuffer`
- `fragmentshaderbarycentrics`
- `gears`
- `geometryshader`
- `gltfloading`
- `gltfscenerendering`
- `gltfskinning`
- `graphicspipelinelibrary`
- `hdr`
- `imgui`
- `indirectdraw`
- `inlineuniformblocks`
- `inputattachments`
- `instancing`
- `meshshader`
- `multisampling`
- `multisamplingalphatocoverage`
- `multithreading`
- `multiview`
- `negativeviewportheight`
- `occlusionquery`
- `parallaxmapping`
- `particlesystem`
- `pbribl`
- `pbrtexture`
- `pipelines`
- `pipelinestatistics`
- `pushconstants`
- `pushdescriptors`
- `radialblur`
- `raytracingbasic`
- `raytracingpositionfetch`
- `renderheadless`
- `screenshot`
- `shaderobjects`
- `shadowmapping`
- `shadowmappingcascade`
- `shadowmappingomni`
- `specializationconstants`
- `sphericalenvmapping`
- `stencilbuffer`
- `subpasses`
- `tessellation`
- `textoverlay`
- `texture`
- `texture3d`
- `texturearray`
- `texturecubemap`
- `texturecubemaparray`
- `texturemipmapgen`
- `texturesparseresidency`
- `triangle`
- `variablerateshading`
- `vertexattributes`
- `viewportarray`
- `vulkanscene`

### FAIL_BLACK (7)

- `bufferdeviceaddress` — `* FAIL: rendered frame failed evidence checks (coverage/colours/determinism)`
- `deferred` — `* FAIL: rendered frame failed evidence checks (coverage/colours/determinism)`
- `displacement` — `* FAIL: rendered frame failed evidence checks (coverage/colours/determinism)`
- `offscreen` — `* FAIL: rendered frame failed evidence checks (coverage/colours/determinism)`
- `pbrbasic` — `* FAIL: rendered frame failed evidence checks (coverage/colours/determinism)`
- `ssao` — `* FAIL: rendered frame failed evidence checks (coverage/colours/determinism)`
- `terraintessellation` — `* FAIL: rendered frame failed evidence checks (coverage/colours/determinism)`

### FAIL_STAGE (4)

- `raytracingcallable` — `UNSUPPORTED_OPERATION: Vulkan execute does not support shader stage 'callable' yet (callable1.rcall)`
- `raytracinggltf` — `UNSUPPORTED_OPERATION: Vulkan execute does not support shader stage 'anyhit' yet (anyhit.rahit) [bac`
- `raytracingintersection` — `UNSUPPORTED_OPERATION: Vulkan execute does not support shader stage 'intersection' yet (intersection`
- `raytracingtextures` — `UNSUPPORTED_OPERATION: Vulkan execute does not support shader stage 'anyhit' yet (anyhit.rahit) [bac`

### FAIL_OTHER (3)

- `raytracingreflections` — `nonzero`
- `raytracingsbtdata` — `nonzero`
- `raytracingshadows` — `nonzero`

### FAIL_COMPILE (1)

- `descriptorheapuntyped` — `COMPILATION_FAILED: glslangValidator failed for external/shader-corpus/Vulkan/shaders/glsl/descripto`

### FAIL_COMPUTE_RES (1)

- `oit` — `UNSUPPORTED_OPERATION: graphics smoke does not bind storage images yet (set=0 binding=0)`

### FAIL_DESC_KIND (1)

- `rayquery` — `UNSUPPORTED_OPERATION: unsupported descriptor kind at set=0 binding=1`

## Workflow

1. **Breadth** — `./tools/csrun-breadth` (refreshes this file)
2. **Pick largest actionable bucket**
3. **todo/ XFAIL** for that capability
4. **Implement** → promote to `execute/corpus/`
5. Re-scan; watch the bucket shrink

