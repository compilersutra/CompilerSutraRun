#!/usr/bin/env bash
# build-mesa26-radv.sh — build Mesa ≥26 (RADV) into a user prefix (no system replace)
#
# Use when you do not want the kisak PPA, or need bleeding-edge git.
# Prefer scripts/upgrade-mesa26-kisak.sh on Ubuntu 24.04 if available.
#
# Usage:
#   chmod +x scripts/build-mesa26-radv.sh
#   ./scripts/build-mesa26-radv.sh              # build + install to ~/mesa-radv-26
#   ./scripts/build-mesa26-radv.sh --deps-only
#   source ~/mesa-radv-26/env.sh
#   ./build/bin/csrun --list-devices

set -euo pipefail

PREFIX="${MESA_PREFIX:-$HOME/mesa-radv-26}"
SRC="${MESA_SRC:-$HOME/src/mesa}"
BRANCH="${MESA_BRANCH:-main}"
JOBS="${JOBS:-$(nproc)}"
DEPS_ONLY=0

for arg in "$@"; do
  case "$arg" in
    --deps-only) DEPS_ONLY=1 ;;
    --prefix=*) PREFIX="${arg#*=}" ;;
    --src=*) SRC="${arg#*=}" ;;
    --branch=*) BRANCH="${arg#*=}" ;;
    -h|--help)
      sed -n '2,16p' "$0"
      exit 0
      ;;
    *)
      echo "unknown arg: $arg" >&2
      exit 2
      ;;
  esac
done

echo "==> target prefix: $PREFIX"
echo "==> source tree:   $SRC (branch/tag: $BRANCH)"

need_sudo() {
  if [[ "${EUID}" -ne 0 ]]; then sudo "$@"; else "$@"; fi
}

echo "==> installing build dependencies (Ubuntu)"
need_sudo apt-get update -qq
need_sudo apt-get install -y \
  git build-essential meson ninja-build pkg-config python3-mako \
  python3-setuptools flex bison \
  libdrm-dev libwayland-dev libwayland-egl-backend-dev wayland-protocols \
  libx11-dev libxext-dev libxdamage-dev libxcb1-dev libxcb-glx0-dev \
  libxcb-keysyms1-dev libx11-xcb-dev libxcb-dri2-0-dev libxcb-dri3-dev \
  libxcb-present-dev libxcb-sync-dev libxshmfence-dev libxxf86vm-dev \
  libxrandr-dev \
  libelf-dev libunwind-dev llvm-dev libclang-dev \
  libvulkan-dev glslang-tools \
  zlib1g-dev libexpat1-dev libzstd-dev

if [[ "$DEPS_ONLY" -eq 1 ]]; then
  echo "deps installed; exiting (--deps-only)"
  exit 0
fi

if [[ ! -d "$SRC/.git" ]]; then
  echo "==> cloning Mesa"
  mkdir -p "$(dirname "$SRC")"
  git clone --depth 1 --branch "$BRANCH" https://gitlab.freedesktop.org/mesa/mesa.git "$SRC"
else
  echo "==> updating Mesa checkout"
  git -C "$SRC" fetch --depth 1 origin "$BRANCH"
  git -C "$SRC" checkout -f FETCH_HEAD || git -C "$SRC" checkout -f "$BRANCH"
fi

if [[ -f "$SRC/VERSION" ]]; then
  echo "==> Mesa VERSION: $(tr -d '\n' <"$SRC/VERSION")"
fi

BUILD="$SRC/build-radv26"
rm -rf "$BUILD"

echo "==> meson setup"
meson setup "$BUILD" "$SRC" \
  --prefix="$PREFIX" \
  --libdir=lib \
  -Dbuildtype=release \
  -Dplatforms=x11,wayland \
  -Dgallium-drivers=radeonsi \
  -Dvulkan-drivers=amd \
  -Dllvm=enabled \
  -Dshared-llvm=enabled \
  -Dgbm=enabled \
  -Degl=enabled \
  -Dglx=dri

echo "==> compile ($JOBS jobs) — this takes a while"
ninja -C "$BUILD" -j"$JOBS"
ninja -C "$BUILD" install

ICD=""
for f in \
  "$PREFIX/share/vulkan/icd.d/radeon_icd.x86_64.json" \
  "$PREFIX/share/vulkan/icd.d/radeon_icd.json"
do
  [[ -f "$f" ]] && ICD="$f" && break
done
if [[ -z "$ICD" ]]; then
  echo "ERROR: radeon ICD json not found under $PREFIX/share/vulkan/icd.d" >&2
  ls -la "$PREFIX/share/vulkan/icd.d" 2>/dev/null || true
  exit 1
fi

RADV_SO="$(find "$PREFIX/lib" -name 'libvulkan_radeon.so' | head -1)"
if [[ -z "$RADV_SO" ]]; then
  echo "ERROR: libvulkan_radeon.so not found under $PREFIX/lib" >&2
  exit 1
fi

python3 - "$ICD" "$RADV_SO" <<'PY'
import json, sys
icd, so = sys.argv[1], sys.argv[2]
with open(icd) as f:
    data = json.load(f)
data["ICD"]["library_path"] = so
with open(icd, "w") as f:
    json.dump(data, f, indent=4)
    f.write("\n")
print("ICD library_path ->", so)
PY

ENV_FILE="$PREFIX/env.sh"
cat >"$ENV_FILE" <<EOF
# source $ENV_FILE
# Built RADV first; /usr libdrm ahead of /opt/amdgpu (ldconfig prefers amdgpu).
export MESA_PREFIX="$PREFIX"
export LD_LIBRARY_PATH="$PREFIX/lib:/usr/lib/x86_64-linux-gnu\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export LIBGL_DRIVERS_PATH="$PREFIX/lib/dri"
export VK_ICD_FILENAMES="$ICD"
export DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1
unset VK_DRIVER_FILES
echo "[mesa-radv-26] \$VK_ICD_FILENAMES"
EOF

echo
echo "=============================================="
echo " Installed to $PREFIX"
echo "   source $ENV_FILE"
echo "   ./build/bin/csrun --list-devices"
echo " Expect (Mesa main ≥ 2026-09-10): RX 9060 XT ... perf_query=yes"
echo "=============================================="
