# Contributing to CompilerSutraRun

## Principles

1. Keep PRs focused; no drive-by reformatting.
2. Never fake GPU success — use `UNAVAILABLE` / `UNIMPLEMENTED`.
3. Prefer existing toolchains (glslang, SPIR-V Tools, hiprtc, NVCC, DXC) over reimplementation.
4. Archive product-direction prompts verbatim under `prompt/`.
5. Match existing C++ style (`.clang-format`); LLVM-ish commit subjects welcome.

## Workflow

```bash
cmake --preset default
cmake --build --preset default
ctest --preset default
./build/bin/csrun --list-backends
```

If you touch a GPU backend, run the relevant path on real hardware when available:

```bash
./build/bin/csrun examples/vector_add/vector_add.yaml --backend <name>
```

## Adding a backend

1. Implement `Backend` (`info`, `enumerateDevices`, `execute`).
2. Wire CMake `CSRUN_ENABLE_*` / `CSRUN_HAS_*`.
3. Register once in `src/runtime/BuiltinBackends.cpp`.
4. Reuse `VectorAddScaffold` for host prep / validation / result fill.
5. Document status in `docs/milestones.md`.

Do **not** add a `dynamic_cast` switch in `PlatformRuntime`.

## Commit messages

Short, imperative, area-tagged when useful:

```text
[OpenCL] Add vector_add execute path

Online compile via ICD; validate against CPU reference.
```

No AI co-author trailers.

## License

Contributions are under Apache-2.0 (see `LICENSE`).
