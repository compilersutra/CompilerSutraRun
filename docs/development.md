# Contributing

1. Keep changes focused; do not reformat unrelated files.
2. Archive new product-direction prompts under `prompt/` (verbatim).
3. Do not fake GPU results.
4. Prefer integrating mature OSS (Shaderc, SPIR-V Tools, DXC, CUDA) over reimplementation.
5. Run:

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
```

License: Apache-2.0.
