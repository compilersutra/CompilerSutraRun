# csrun-lit

llvm-lit style regression suite for `csrun`.

```bash
./tools/csrun-lit
./tools/csrun-lit --filter 'compile/examples'
./tools/csrun-lit --filter 'execute/corpus'
./tools/csrun-lit --filter 'compile/corpus/texture'
```


## Breadth-first (do this before deep feature work)

1. Run execute breadth across all Vulkan sample folders:
   ```bash
   . scripts/env-mesa-main-radv.sh
   ./tools/csrun-breadth
   ```
2. Read `BREADTH.md` — pick the **largest fail bucket**, not a random sample.
3. Only then open/fix a milestone issue and implement that capability.
4. Re-run breadth to confirm the bucket shrinks.

## Layout

```text
tests/lit/
  compile/
    examples/          # in-tree
    corpus/<sample>/   # mirrors Sascha GLSL tree
  execute/
    examples/          # in-tree YAML
    corpus/            # working corpus execute
  todo/<feature>/      # XFAIL until feature ships → then move to execute/corpus/
  Inputs/
    corpus/
    todo/
  GAPS.md
  README.md
```

Promote path: `todo/<feature>/` → `execute/corpus/<feature>/` (drop `XFAIL`).

Corpus compile paths match upstream sample folders (`bloom/`, `ssao/`, …).

## Test format

| Directive | Meaning |
|-----------|---------|
| `RUN:` | Shell command (exit 0) |
| `REQUIRES:` | Skip if feature missing → `UNSUPPORTED` |
| `XFAIL:` | Expect fail → `Expectedly Failed` |

Substitutions: `%csrun` `%repo` `%s` `%S` `%t` `%T`

## Features

`host`, `glslang`, `vulkan`, `gpu`, `examples`, `corpus`, …
