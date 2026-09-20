# source this file:  source scripts/env-mesa-main-radv.sh
#
# Side-by-side Mesa main RADV (GFX12 VK_KHR_performance_query).
# Built into ~/mesa-radv-26 via scripts/build-mesa26-radv.sh

_CSRUN_MESA_PREFIX="${MESA_PREFIX:-$HOME/mesa-radv-26}"
_CSRUN_RADV_SO="$_CSRUN_MESA_PREFIX/lib/libvulkan_radeon.so"
_CSRUN_ICD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/.cache"
_CSRUN_ICD="$_CSRUN_ICD_DIR/radeon_icd_mesa_main.json"

if [[ ! -f "$_CSRUN_RADV_SO" ]]; then
  echo "env-mesa-main-radv: missing $_CSRUN_RADV_SO — run scripts/build-mesa26-radv.sh first" >&2
  return 1 2>/dev/null || exit 1
fi

mkdir -p "$_CSRUN_ICD_DIR"
cat >"$_CSRUN_ICD" <<EOF
{
    "file_format_version": "1.0.1",
    "ICD": {
        "library_path": "$_CSRUN_RADV_SO",
        "api_version": "1.4.335"
    }
}
EOF

export DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1
export VK_ICD_FILENAMES="$_CSRUN_ICD"
export LD_LIBRARY_PATH="$_CSRUN_MESA_PREFIX/lib:/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
unset VK_DRIVER_FILES

echo "[mesa-main-radv] $VK_ICD_FILENAMES"
echo "[mesa-main-radv] Expect: RX 9060 XT ... perf_query=yes (Mesa 26.3-devel)"

# Warn if a connected display is on the discrete AMD PCI (same die as csrun device 0).
# Perf-query / mesa-devel on that GPU has been observed to trip optc401_disable_crtc
# even with --no-show. Safe setup: plug the monitor into motherboard HDMI (iGPU).
_csrun_warn_display_on_dgpu() {
  local dgpu_pci="" conn card pci st
  for card in /sys/class/drm/card[0-9]; do
    [[ -e "$card/device/vendor" ]] || continue
    [[ "$(cat "$card/device/vendor" 2>/dev/null)" == "0x1002" ]] || continue
    # Prefer the non-APU / first discrete: PCI with larger bus often dGPU; match by
    # having HDMI connected AND being the primary xrandr provider later.
    pci="$(basename "$(readlink -f "$card/device" 2>/dev/null)" 2>/dev/null)"
    for conn in "$card"-HDMI-* "$card"-DP-*; do
      [[ -e "$conn/status" ]] || continue
      st="$(cat "$conn/status" 2>/dev/null)"
      [[ "$st" == "connected" ]] || continue
      # Heuristic: card with connected HDMI on bus != iGPU APU (15:00 on this box)
      if [[ "$pci" != *":15:00."* ]]; then
        dgpu_pci="$pci"
        echo "[mesa-main-radv] WARNING: display $(basename "$conn") is on dGPU $pci" >&2
        echo "[mesa-main-radv] WARNING: csrun/perf_query on that GPU can freeze the desktop" >&2
        echo "[mesa-main-radv] WARNING: move HDMI to motherboard (iGPU), then use --device-index 0" >&2
        return
      fi
    done
  done
}
_csrun_warn_display_on_dgpu
unset -f _csrun_warn_display_on_dgpu
