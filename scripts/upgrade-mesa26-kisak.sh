#!/usr/bin/env bash
# upgrade-mesa26-kisak.sh — Ubuntu 24.04: install Mesa ≥26 from kisak-mesa PPA
#
# Gives RADV GFX12 (RX 9060 XT) a chance at perf_query=yes.
# WARNING: replaces system Mesa packages. Have a plan to remove the PPA if needed.
#
# Usage:
#   chmod +x scripts/upgrade-mesa26-kisak.sh
#   ./scripts/upgrade-mesa26-kisak.sh
#   # reboot (or at least restart display/session)
#   DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1 \
#     VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json \
#     ./build/bin/csrun --list-devices

set -euo pipefail

if [[ ! -f /etc/os-release ]]; then
  echo "unsupported: no /etc/os-release" >&2
  exit 1
fi
# shellcheck disable=SC1091
. /etc/os-release
if [[ "${ID:-}" != "ubuntu" || "${VERSION_ID:-}" != "24.04" ]]; then
  echo "This script targets Ubuntu 24.04 (got ${ID:-?} ${VERSION_ID:-?})." >&2
  echo "For other releases, use scripts/build-mesa26-radv.sh (side-by-side build)." >&2
  exit 1
fi

need_sudo() {
  if [[ "${EUID}" -ne 0 ]]; then sudo "$@"; else "$@"; fi
}

echo "==> current Mesa packages"
dpkg -l 'mesa-vulkan-drivers' 'mesa-amdgpu-vulkan-drivers' 2>/dev/null | awk '/^ii/{print}' || true
echo

echo "==> adding ppa:kisak/kisak-mesa"
need_sudo apt-get update -qq
need_sudo apt-get install -y software-properties-common
need_sudo add-apt-repository -y ppa:kisak/kisak-mesa
need_sudo apt-get update

echo "==> candidate mesa-vulkan-drivers:"
apt-cache policy mesa-vulkan-drivers | sed -n '1,12p'

CAND="$(apt-cache policy mesa-vulkan-drivers | awk '/Candidate:/{print $2; exit}')"
if [[ "$CAND" != *kisak* ]]; then
  echo "ERROR: candidate is not from kisak ($CAND). Aborting." >&2
  exit 1
fi
if [[ "$CAND" != 26.* && "$CAND" != *26.* ]]; then
  echo "WARN: candidate $CAND may be < 26. Continuing anyway." >&2
fi

echo
echo "==> upgrading Mesa stack (system-wide)"
# mesa-va-drivers may be absorbed into mesa-libgallium on Mesa 26 packaging
need_sudo apt-get install -y --allow-downgrades \
  mesa-vulkan-drivers mesa-libgallium libgl1-mesa-dri mesa-utils \
  || need_sudo apt-get dist-upgrade -y

echo
echo "==> installed versions"
dpkg -l 'mesa-vulkan-drivers' 'mesa-libgallium' 2>/dev/null | awk '/^ii/{print}'

# Absolute ICD + /usr libdrm ahead of /opt/amdgpu (relative ICD + ldconfig = 25.2 / init fail)
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_HINT="$HOME/.config/csrun-radv26.env"
mkdir -p "$(dirname "$ENV_HINT")"
cp "$REPO_ROOT/scripts/env-kisak-radv.sh" "$ENV_HINT"

echo
echo "=============================================="
echo " Done. Reboot (recommended), then:"
echo "   source $ENV_HINT   # or: source scripts/env-kisak-radv.sh"
echo "   cd ~/compilersutra/universalrunner"
echo "   ./build/bin/csrun --list-devices"
echo " Expect: Mesa 26.2.x on both GPUs."
echo " Note: GFX1200 often still has perf_query=no; iGPU (Raphael) has yes."
echo " For bleeding-edge GFX12 counters: scripts/build-mesa26-radv.sh"
echo " Rollback PPA later:"
echo "   sudo apt install ppa-purge && sudo ppa-purge ppa:kisak/kisak-mesa"
echo "=============================================="
