#!/usr/bin/env bash
# Compile GLSL from shader-corpus git submodules with csrun (--compile-spv).
# Reports compile PASS/FAIL. Execute gate uses in-tree examples only.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
CSRUN="${CSRUN_BIN:-$REPO/build/bin/csrun}"
OUT_DIR="${CSRUN_CORPUS_OUT:-$REPO/.cache/corpus-scan}"
REPORT="$OUT_DIR/report.txt"

# Submodule shader trees (override with CSRUN_CORPUS_DIRS="path1:path2")
if [[ -n "${CSRUN_CORPUS_DIRS:-}" ]]; then
  IFS=':' read -r -a CORPUS_DIRS <<< "$CSRUN_CORPUS_DIRS"
else
  CORPUS_DIRS=(
    "$ROOT/glslang/Test"
    "$ROOT/Vulkan/data/shaders"
  )
fi

if [[ ! -x "$CSRUN" ]]; then
  echo "missing $CSRUN — build csrun first" >&2
  exit 1
fi

missing=0
for d in "${CORPUS_DIRS[@]}"; do
  if [[ ! -d "$d" ]]; then
    echo "missing $d" >&2
    missing=1
  fi
done
if [[ "$missing" -ne 0 ]]; then
  echo "run: git submodule update --init --depth 1 -- external/shader-corpus/glslang external/shader-corpus/Vulkan" >&2
  exit 1
fi

mkdir -p "$OUT_DIR/spv"
: >"$REPORT"

pass=0
fail=0
skip=0
limit="${CSRUN_CORPUS_LIMIT:-0}" # 0 = no limit
seen=0

echo "[scan] csrun=$CSRUN limit=${limit:-none}" | tee -a "$REPORT"
for d in "${CORPUS_DIRS[@]}"; do
  echo "[scan] corpus=$d" | tee -a "$REPORT"
done

while IFS= read -r -d '' src; do
  # Prefer short path relative to shader-corpus root
  rel="${src#"$ROOT/"}"
  base="$(basename "$src")"
  case "$base" in
    *.vert|*.frag|*.comp|*.tesc|*.tese|*.geom) ;;
    *) skip=$((skip + 1)); continue ;;
  esac

  if [[ "$limit" -gt 0 && "$seen" -ge "$limit" ]]; then
    break
  fi
  seen=$((seen + 1))

  out="$OUT_DIR/spv/${rel}.spv"
  mkdir -p "$(dirname "$out")"
  if "$CSRUN" --compile-spv "$src" -o "$out" >/dev/null 2>"$OUT_DIR/spv/${rel}.err"; then
    echo "PASS  $rel" | tee -a "$REPORT"
    pass=$((pass + 1))
  else
    echo "FAIL  $rel" | tee -a "$REPORT"
    fail=$((fail + 1))
  fi
done < <(
  for d in "${CORPUS_DIRS[@]}"; do
    find "$d" -type f -print0
  done | sort -z
)

echo | tee -a "$REPORT"
echo "[scan] PASS=$pass FAIL=$fail SKIP=$skip" | tee -a "$REPORT"
echo "[scan] report -> $REPORT"

echo
echo "Execute gate (in-tree only — layouts csrun supports):"
cd "$REPO"
"$CSRUN" examples/shaders/plasma_bench.yaml --no-show --format json --vendor-counters off --no-benchmark >/dev/null \
  && echo "PASS  examples/shaders/plasma_bench.yaml" \
  || echo "FAIL  examples/shaders/plasma_bench.yaml"
"$CSRUN" examples/matmul/matmul.yaml --format json --vendor-counters off --no-benchmark >/dev/null \
  && echo "PASS  examples/matmul/matmul.yaml" \
  || echo "FAIL  examples/matmul/matmul.yaml"
"$CSRUN" examples/vector_add/vector_add.yaml --format json --vendor-counters off --no-benchmark >/dev/null \
  && echo "PASS  examples/vector_add/vector_add.yaml" \
  || echo "FAIL  examples/vector_add/vector_add.yaml"
