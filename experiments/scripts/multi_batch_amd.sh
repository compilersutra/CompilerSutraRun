#!/usr/bin/env bash
# Multi-run timing on one AMD backend/device (experiment-level dispersion).
# Usage: multi_batch_amd.sh [batches=5] [backend=vulkan] [device_index=0]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CSRUN="${CSRUN:-$ROOT/build/bin/csrun}"
WL="$ROOT/examples/vector_add/vector_add.yaml"
BATCHES="${1:-5}"
BACKEND="${2:-vulkan}"
DEV="${3:-0}"
OUT="${4:-$ROOT/experiments/raw/multi-batch-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"

export DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1
export VK_ICD_FILENAMES="${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/radeon_icd.json}"

echo "batches=$BATCHES backend=$BACKEND device-index=$DEV -> $OUT"
for i in $(seq 1 "$BATCHES"); do
  label=$(printf 'run-%02d' "$i")
  echo "==> $label"
  if [[ "$BACKEND" == "opengl" ]]; then
    if [[ "$DEV" == "0" ]]; then
      DRI_PRIME=1 "$CSRUN" "$WL" --backend opengl --format json --log-level error \
        >"$OUT/${label}.json" 2>"$OUT/${label}.err"
    else
      unset DRI_PRIME || true
      "$CSRUN" "$WL" --backend opengl --format json --log-level error \
        >"$OUT/${label}.json" 2>"$OUT/${label}.err"
    fi
  else
    "$CSRUN" "$WL" --backend "$BACKEND" --device-index "$DEV" --format json --log-level error \
      >"$OUT/${label}.json" 2>"$OUT/${label}.err"
  fi
done

python3 - <<PY
import json,glob,os,re,statistics
out="$OUT"
meds=[]
print(f"{'run':8} {'val':8} {'med_ms':>10} {'within_std':>10}")
for p in sorted(glob.glob(out+"/run-*.json")):
    text=open(p).read()
    m=re.search(r"\{[\s\S]*\}\s*$", text)
    if not m:
        print(os.path.basename(p), "INVALID")
        continue
    d=json.loads(m.group(0))
    t=d["timing"]
    med=t["median_ms"]
    meds.append(med)
    print(f"{os.path.basename(p)[:-5]:8} {d['validation']['status']:8} {med:10.4f} {t.get('stddev_ms', float('nan')):10.4f}")
if len(meds)>=2:
    print(f"multi-run median-of-medians={statistics.median(meds):.4f}  "
          f"stdev-of-medians={statistics.stdev(meds):.4f}  n={len(meds)}")
    print("Note: stdev-of-medians is experiment-level; JSON stddev_ms is within-run only.")
PY
