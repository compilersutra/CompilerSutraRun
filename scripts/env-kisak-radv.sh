# source this file (do not execute):  source scripts/env-kisak-radv.sh
#
# Forces kisak/system Mesa RADV + libdrm ahead of /opt/amdgpu.
# Without LD_LIBRARY_PATH=/usr/lib/..., ldconfig prefers amdgpu's older
# libdrm_amdgpu and kisak RADV 26 aborts (no Vulkan devices).

_CSRUN_RADV_SO="${CSRUN_RADV_SO:-/usr/lib/x86_64-linux-gnu/libvulkan_radeon.so}"
_CSRUN_ICD_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/csrun"
_CSRUN_ICD="$_CSRUN_ICD_DIR/radeon_icd_kisak_abs.json"

if [[ ! -f "$_CSRUN_RADV_SO" ]]; then
  echo "env-kisak-radv: missing $_CSRUN_RADV_SO (install mesa-vulkan-drivers from kisak)" >&2
  return 1 2>/dev/null || exit 1
fi

mkdir -p "$_CSRUN_ICD_DIR"
cat >"$_CSRUN_ICD" <<EOF
{
    "file_format_version": "1.0.1",
    "ICD": {
        "library_path": "$_CSRUN_RADV_SO",
        "api_version": "1.4.354"
    }
}
EOF

export DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1
export VK_ICD_FILENAMES="$_CSRUN_ICD"
# Must be first so libdrm_amdgpu resolves to kisak 2.4.134, not /opt/amdgpu 2.4.124
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
unset VK_DRIVER_FILES

echo "[kisak-radv] VK_ICD_FILENAMES=$VK_ICD_FILENAMES"
echo "[kisak-radv] LD_LIBRARY_PATH starts with /usr/lib/x86_64-linux-gnu"
echo "[kisak-radv] Note: Mesa 26.2.3 still has perf_query=no on GFX1200; iGPU is yes."
