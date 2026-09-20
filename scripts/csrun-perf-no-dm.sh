#!/usr/bin/env bash
# Run csrun with RADV HW counters while the display manager is stopped.
# Use from SSH (preferred) or a VT (Ctrl+Alt+F3) — stopping GDM blanks the GUI.
#
# SSH does NOT replace stopping the DM when the monitor is on the dGPU.
#
# Usage:
#   ./scripts/csrun-perf-no-dm.sh
#   ./scripts/csrun-perf-no-dm.sh examples/shaders/plasma_bench.yaml --format json
#   CSRUN_VENDOR_COUNTERS=all ./scripts/csrun-perf-no-dm.sh   # try full set (may hang GFX12)
#
# Env:
#   CSRUN_BIN              default: <repo>/build/bin/csrun
#   CSRUN_DEVICE_INDEX     default: 0
#   CSRUN_VENDOR_COUNTERS  default: single-pass  (all | single-pass | off)
#   CSRUN_EXTRA_ARGS       extra flags, default: --no-benchmark
#   SKIP_DM=1              skip stop/start
#   CSRUN_LOG_DIR          default: <repo>/.cache

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# shellcheck source=env-mesa-main-radv.sh
source "$ROOT/scripts/env-mesa-main-radv.sh"

CSRUN_BIN="${CSRUN_BIN:-$ROOT/build/bin/csrun}"
DEVICE_INDEX="${CSRUN_DEVICE_INDEX:-0}"
VENDOR_COUNTERS="${CSRUN_VENDOR_COUNTERS:-single-pass}"
# shellcheck disable=SC2206
EXTRA_ARGS=(${CSRUN_EXTRA_ARGS---no-benchmark})

if [[ ! -x "$CSRUN_BIN" ]]; then
  echo "csrun-perf-no-dm: missing $CSRUN_BIN — build first" >&2
  exit 1
fi

# Refuse 'all' unless CSRUN_ALLOW_ALL=1 — it hangs GFX12 (DEVICE_LOST).
if [[ "$VENDOR_COUNTERS" == "all" && "${CSRUN_ALLOW_ALL:-0}" != "1" ]]; then
  echo "[csrun-perf-no-dm] refusing CSRUN_VENDOR_COUNTERS=all (GFX12 hang)." >&2
  echo "[csrun-perf-no-dm] using single-pass instead. Set CSRUN_ALLOW_ALL=1 to override." >&2
  VENDOR_COUNTERS=single-pass
fi

if [[ $# -eq 0 ]]; then
  set -- examples/shaders/plasma_bench.yaml --no-show --format json \
    --device-index "$DEVICE_INDEX" \
    --vendor-counters "$VENDOR_COUNTERS" \
    "${EXTRA_ARGS[@]}"
else
  # If caller passed a yaml/flags but forgot --vendor-counters, force safe default
  # (csrun C++ default is still 'all').
  has_vc=0
  for a in "$@"; do
    [[ "$a" == "--vendor-counters" ]] && has_vc=1 && break
  done
  if [[ "$has_vc" -eq 0 ]]; then
    set -- "$@" --vendor-counters "$VENDOR_COUNTERS"
  fi
fi

echo "[csrun-perf-no-dm] vendor-counters=$VENDOR_COUNTERS"
dm_was_active=0
restore_dm() {
  if [[ "${SKIP_DM:-0}" == "1" ]]; then
    return 0
  fi
  if [[ "$dm_was_active" -eq 1 ]]; then
    echo "[csrun-perf-no-dm] starting display-manager..."
    sudo systemctl start display-manager || true
  fi
}
trap restore_dm EXIT

if [[ "${SKIP_DM:-0}" != "1" ]]; then
  if systemctl is-active --quiet display-manager 2>/dev/null; then
    dm_was_active=1
    echo "[csrun-perf-no-dm] stopping display-manager (GUI will blank)..."
    sudo systemctl stop display-manager
    # Give amdgpu time to release CRTCs / settle after stop.
    sleep 3
  else
    echo "[csrun-perf-no-dm] display-manager already inactive"
  fi
fi

LOG_DIR="${CSRUN_LOG_DIR:-$ROOT/.cache}"
mkdir -p "$LOG_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$LOG_DIR/csrun-perf-${STAMP}.log"
JSON_OUT="$LOG_DIR/csrun-perf-${STAMP}.json"

echo "[csrun-perf-no-dm] $CSRUN_BIN $*"
echo "[csrun-perf-no-dm] log → $LOG"
echo "[csrun-perf-no-dm] json → $JSON_OUT"

set +e
"$CSRUN_BIN" "$@" 2>&1 | tee "$LOG"
rc=${PIPESTATUS[0]}
set -e

python3 - "$LOG" "$JSON_OUT" <<'PY' || true
import json, sys
log, out = sys.argv[1], sys.argv[2]
text = open(log, errors="replace").read()
obj = None
# Pretty-printed JSON spans multiple lines — take first '{' .. last '}'.
i, j = text.find("{"), text.rfind("}")
if i >= 0 and j > i:
    try:
        obj = json.loads(text[i : j + 1])
    except json.JSONDecodeError:
        obj = None
if obj is None:
    if "VkResult=-4" in text:
        print("[csrun-perf-no-dm] no JSON (DEVICE_LOST / GPU hang) — reboot, then re-run", flush=True)
    else:
        print("[csrun-perf-no-dm] no JSON object found in log", flush=True)
else:
    with open(out, "w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")
    print(f"[csrun-perf-no-dm] wrote {out}", flush=True)
    device = (obj.get("device") or {}).get("name", "?")
    timing = obj.get("timing") or {}
    print(f"[csrun-perf-no-dm] device={device} median_ms={timing.get('median_ms')}", flush=True)
    counters = obj.get("counters") or {}
    vendor = (counters.get("vendor") or {}) if isinstance(counters, dict) else {}
    vlist = vendor.get("counters") or []
    print(
        f"[csrun-perf-no-dm] vendor_hw={len(vlist)} available={vendor.get('available')} note={vendor.get('note')}",
        flush=True,
    )
    if "VkResult=-4" in text or vendor.get("available") is False:
        if "2 pass" in text or ", all)" in text:
            print("[csrun-perf-no-dm] tip: use default single-pass — 'all' hangs GFX12", flush=True)
PY

echo "[csrun-perf-no-dm] csrun exit=$rc"
exit "$rc"
