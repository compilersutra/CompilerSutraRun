#!/usr/bin/env bash
# Compile GLSL from shader-corpus git submodules with csrun (--compile-spv).
# Reports PASS/FAIL overall and by stage (.vert / .frag / .comp / other).
# Execute gate: in-tree examples + lit corpus graphics probes.
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
    "$ROOT/Vulkan/shaders/glsl"
  )
fi

# Optional: CSRUN_CORPUS_STAGES="vert,frag,comp" (default: all of those + tesc/tese/geom)
STAGES_FILTER="${CSRUN_CORPUS_STAGES:-}"

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
xfail=0
xpass=0
skip=0
limit="${CSRUN_CORPUS_LIMIT:-0}" # 0 = no limit
seen=0

declare -A pass_by=([vert]=0 [frag]=0 [comp]=0 [other]=0)
declare -A fail_by=([vert]=0 [frag]=0 [comp]=0 [other]=0)

# Known expected compile failures (tracked in tests/lit as XFAIL).
# Match against path relative to shader-corpus root (e.g. Vulkan/shaders/glsl/...).
is_xfail() {
  case "$1" in
    */descriptorheapuntyped/cube.vert|*/descriptorheapuntyped/cube.frag) return 0 ;;
    Vulkan/shaders/glsl/descriptorheapuntyped/cube.vert|Vulkan/shaders/glsl/descriptorheapuntyped/cube.frag) return 0 ;;
    *) return 1 ;;
  esac
}

stage_of() {
  case "$1" in
    *.vert) echo vert ;;
    *.frag) echo frag ;;
    *.comp) echo comp ;;
    *) echo other ;;
  esac
}

want_stage() {
  local st="$1"
  if [[ -z "$STAGES_FILTER" ]]; then
    return 0
  fi
  [[ ",${STAGES_FILTER}," == *",${st},"* ]]
}

echo "[scan] csrun=$CSRUN limit=${limit:-none} stages=${STAGES_FILTER:-all}" | tee -a "$REPORT"
for d in "${CORPUS_DIRS[@]}"; do
  echo "[scan] corpus=$d" | tee -a "$REPORT"
done

while IFS= read -r -d '' src; do
  rel="${src#"$ROOT/"}"
  base="$(basename "$src")"
  case "$base" in
    *.vert|*.frag|*.comp|*.tesc|*.tese|*.geom) ;;
    *) skip=$((skip + 1)); continue ;;
  esac

  st="$(stage_of "$base")"
  if ! want_stage "$st"; then
    # also allow tesc/tese/geom under "other" when filter empty only
    if [[ -n "$STAGES_FILTER" ]]; then
      skip=$((skip + 1))
      continue
    fi
  fi

  if [[ "$limit" -gt 0 && "$seen" -ge "$limit" ]]; then
    break
  fi
  seen=$((seen + 1))

  out="$OUT_DIR/spv/${rel}.spv"
  mkdir -p "$(dirname "$out")"
  if "$CSRUN" --compile-spv "$src" -o "$out" >/dev/null 2>"$OUT_DIR/spv/${rel}.err"; then
    if is_xfail "$rel"; then
      echo "XPASS $rel" | tee -a "$REPORT"
      xpass=$((xpass + 1))
    else
      echo "PASS  $rel" | tee -a "$REPORT"
      pass=$((pass + 1))
      pass_by[$st]=$((pass_by[$st] + 1))
    fi
  else
    if is_xfail "$rel"; then
      echo "XFAIL $rel" | tee -a "$REPORT"
      xfail=$((xfail + 1))
    else
      echo "FAIL  $rel" | tee -a "$REPORT"
      fail=$((fail + 1))
      fail_by[$st]=$((fail_by[$st] + 1))
    fi
  fi
done < <(
  for d in "${CORPUS_DIRS[@]}"; do
    find "$d" -type f -print0
  done | sort -z
)

echo | tee -a "$REPORT"
echo "[scan] PASS=$pass FAIL=$fail XFAIL=$xfail XPASS=$xpass SKIP=$skip" | tee -a "$REPORT"
echo "[scan] by stage:" | tee -a "$REPORT"
for st in vert frag comp other; do
  echo "  $st  PASS=${pass_by[$st]} FAIL=${fail_by[$st]}" | tee -a "$REPORT"
done
echo "[scan] report -> $REPORT"
echo "[scan] tip: regression gate is ./tools/csrun-lit (llvm-lit style)"

echo
echo "Execute gate (in-tree + lit corpus graphics):"
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
if [[ -f tests/lit/Inputs/corpus/texture.yaml ]]; then
  "$CSRUN" tests/lit/Inputs/corpus/texture.yaml --format json --vendor-counters off --no-benchmark >/dev/null \
    && echo "PASS  tests/lit/Inputs/corpus/texture.yaml" \
    || echo "FAIL  tests/lit/Inputs/corpus/texture.yaml"
fi
if [[ -f tests/lit/Inputs/corpus/phong.yaml ]]; then
  "$CSRUN" tests/lit/Inputs/corpus/phong.yaml --format json --vendor-counters off --no-benchmark >/dev/null \
    && echo "PASS  tests/lit/Inputs/corpus/phong.yaml" \
    || echo "FAIL  tests/lit/Inputs/corpus/phong.yaml"
fi
