#!/usr/bin/env bash
# Phase-1 / CI-style smoke: vector_add on discrete + integrated AMD GPUs.
# Does NOT produce paper tables. Writes JSON under experiments/raw/smoke-$$/
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CSRUN="${CSRUN:-$ROOT/build/bin/csrun}"
WL="$ROOT/examples/vector_add/vector_add.yaml"
OUT="${1:-$ROOT/experiments/raw/smoke-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"

export DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/radeon_icd.json}"

echo "Writing to $OUT"
"$CSRUN" --list-devices --log-level error | tee "$OUT/list-devices.txt"

run() {
  local label=$1; shift
  echo "==> $label: $*"
  "$@" --format json --log-level error >"$OUT/${label}.json" 2>"$OUT/${label}.err" || {
    echo "FAILED $label" >&2
    cat "$OUT/${label}.err" >&2
    return 1
  }
}

# Indices from --list-devices (physical). On this host: vulkan 0=dGPU, 1=iGPU;
# hip/opencl 0=dGPU, 1=iGPU. Re-check list-devices.txt after driver updates.
run vulkan-dgpu   "$CSRUN" "$WL" --backend vulkan --device-index 0
run vulkan-igpu   "$CSRUN" "$WL" --backend vulkan --device-index 1
run hip-dgpu      "$CSRUN" "$WL" --backend hip --device-index 0
run hip-igpu      "$CSRUN" "$WL" --backend hip --device-index 1
run opencl-dgpu   "$CSRUN" "$WL" --backend opencl --device-index 0
run opencl-igpu   "$CSRUN" "$WL" --backend opencl --device-index 1
DRI_PRIME=1 run opengl-dgpu "$CSRUN" "$WL" --backend opengl
# Default GL context is typically the iGPU; do not set DRI_PRIME=0 (pollutes stdout).
unset DRI_PRIME || true
run opengl-igpu "$CSRUN" "$WL" --backend opengl || true

python3 - <<PY
import json,glob,os,re
out="$OUT"
print(f"{'label':22} {'val':8} {'med_ms':>10} {'src':14} device")
for p in sorted(glob.glob(out+"/*.json")):
    text=open(p).read()
    # Tolerate driver noise before the JSON object (e.g. DRI_PRIME warnings).
    m=re.search(r"\{[\s\S]*\}\s*$", text)
    if not m:
        print(f"{os.path.basename(p)[:-5]:22} INVALID_JSON")
        continue
    d=json.loads(m.group(0))
    if "backend" not in d: continue
    t=d["timing"]
    print(f"{os.path.basename(p)[:-5]:22} {d['validation']['status']:8} {t['median_ms']:10.4f} {t['source']:14} {d['device']['name'][:40]}")
PY
echo "Done. Raw JSON in $OUT"
