#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT}/build/linux"
AURORA_DIR="${ROOT}/extern/aurora"
AURORA_REF="8b690b60e699c92e3327886ebd84cf7f05c5d36c"
PHOSG_DIR="${ROOT}/extern/phosg"
PHOSG_REF="891c3444b2084947eebf705c167b6279438114e9"
RESOURCE_DASM_DIR="${ROOT}/extern/resource_dasm"
RESOURCE_DASM_REF="6ea3bd51ca4f9782d6c3c74003df1a4c8a0c4798"
AUDIO_PREFIX="${ROOT}/extern/audio-prefix"
AUDIO_BUILD_DIR="${ROOT}/build/audio-deps"

TYPE=Release
CLEAN=0
CLEAN_ALL=0
INSTALL=0
NATIVE_CPU=ON
LTO=ON
JAUDIO=ON
JOBS="$(nproc 2>/dev/null || echo 4)"

usage() {
  cat <<'USAGE'
Usage: ./build.sh [options]

Options:
  --clean           remove the native build directory before building
  --clean-all       also remove bootstrapped audio dependency builds
  --debug           use RelWithDebInfo
  --deps             install common build/Vulkan packages
  --portable         disable -march=native/-mtune=native
  --no-lto           disable IPO/LTO
  --no-jaudio        build without the experimental software JAudio bridge
  --jobs=N           set parallel build jobs
  -h, --help         show this help
USAGE
}

for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN=1 ;;
    --clean-all) CLEAN=1; CLEAN_ALL=1 ;;
    --debug) TYPE=RelWithDebInfo ;;
    --deps|--install-deps) INSTALL=1 ;;
    --portable) NATIVE_CPU=OFF ;;
    --no-lto) LTO=OFF ;;
    --no-jaudio) JAUDIO=OFF ;;
    --jobs=*) JOBS="${arg#*=}" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "Invalid --jobs value: $JOBS" >&2
  exit 2
fi

if [[ $INSTALL -eq 1 ]]; then
  if command -v pacman >/dev/null 2>&1; then
    sudo pacman -S --needed base-devel cmake ninja git clang ccache zlib vulkan-icd-loader vulkan-tools
  elif command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y \
      build-essential clang cmake ninja-build git ccache zlib1g-dev pkg-config \
      libvulkan1 vulkan-tools mesa-vulkan-drivers \
      libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
      libxss-dev libwayland-dev libxkbcommon-dev libdrm-dev libgbm-dev libudev-dev \
      libasound2-dev libpulse-dev libpipewire-0.3-dev libegl1-mesa-dev
  elif command -v dnf >/dev/null 2>&1; then
    sudo dnf install -y \
      gcc-c++ clang cmake ninja-build git ccache zlib-devel pkgconf-pkg-config \
      vulkan-loader vulkan-tools libX11-devel libXext-devel libXrandr-devel \
      libXcursor-devel libXfixes-devel libXi-devel wayland-devel libxkbcommon-devel
  else
    echo "Unsupported package manager. Install CMake >=3.25, Ninja, Git, Clang/GCC, zlib and Vulkan manually." >&2
    exit 2
  fi
fi

for cmd in cmake ninja git; do
  command -v "$cmd" >/dev/null 2>&1 || {
    echo "Missing dependency: $cmd (try ./build.sh --deps)" >&2
    exit 2
  }
done

CC_BIN="${CC:-$(command -v clang || command -v gcc)}"
CXX_BIN="${CXX:-$(command -v clang++ || command -v g++)}"

if [[ $CLEAN_ALL -eq 1 ]]; then
  rm -rf "$AUDIO_BUILD_DIR" "$AUDIO_PREFIX"
fi

ensure_pinned_repo() {
  local url="$1" dir="$2" ref="$3" label="$4"
  if [[ ! -d "${dir}/.git" ]]; then
    echo "Fetching ${label}..."
    rm -rf "$dir"
    if ! git clone --filter=blob:none "$url" "$dir"; then
      echo "Failed to fetch ${label}. Check Internet/DNS access and retry." >&2
      exit 2
    fi
  fi

  local current
  current="$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)"
  if [[ "$current" != "$ref" ]]; then
    echo "Pinning ${label} to ${ref}"
    git -C "$dir" fetch --depth=1 origin "$ref"
    git -C "$dir" checkout --detach "$ref"
  fi
}

ensure_pinned_repo "https://github.com/encounter/aurora.git" "$AURORA_DIR" "$AURORA_REF" "Aurora"

apply_aurora_patch() {
  local patch_path="$1" label="$2"
  [[ -f "$patch_path" ]] || return 0
  if git -C "$AURORA_DIR" apply --reverse --check "$patch_path" >/dev/null 2>&1; then
    return 0
  fi
  if git -C "$AURORA_DIR" apply --check "$patch_path" >/dev/null 2>&1; then
    echo "Applying Aurora ${label} patch"
    git -C "$AURORA_DIR" apply "$patch_path"
    return 0
  fi
  echo "Aurora ${label} patch does not apply cleanly to ${AURORA_REF}." >&2
  exit 2
}

apply_aurora_patch "${ROOT}/patches/aurora-optional-haptic.patch" "optional-haptic compatibility"
apply_aurora_patch "${ROOT}/patches/aurora-gci-folder-card.patch" "GCI-folder CARD persistence"

if [[ "$JAUDIO" == ON ]]; then
  ensure_pinned_repo "https://github.com/fuzziqersoftware/phosg.git" "$PHOSG_DIR" "$PHOSG_REF" "phosg"
  ensure_pinned_repo "https://github.com/fuzziqersoftware/resource_dasm.git" "$RESOURCE_DASM_DIR" "$RESOURCE_DASM_REF" "resource_dasm"

  PHOSG_STAMP="${AUDIO_PREFIX}/.phosg-${PHOSG_REF}"
  if [[ ! -f "$PHOSG_STAMP" ]]; then
    echo "Building native audio dependency: phosg"
    rm -rf "${AUDIO_BUILD_DIR}/phosg" "$AUDIO_PREFIX"
    cmake -S "$PHOSG_DIR" -B "${AUDIO_BUILD_DIR}/phosg" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$AUDIO_PREFIX" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_C_COMPILER="$CC_BIN" \
      -DCMAKE_CXX_COMPILER="$CXX_BIN"
    cmake --build "${AUDIO_BUILD_DIR}/phosg" --target install --parallel "$JOBS"
    touch "$PHOSG_STAMP"
  fi

  RESOURCE_STAMP="${AUDIO_BUILD_DIR}/resource_dasm/.resource-dasm-${RESOURCE_DASM_REF}"
  if [[ ! -f "$RESOURCE_STAMP" ]]; then
    echo "Building native audio dependency: resource_dasm"
    rm -rf "${AUDIO_BUILD_DIR}/resource_dasm"
    cmake -S "$RESOURCE_DASM_DIR" -B "${AUDIO_BUILD_DIR}/resource_dasm" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$AUDIO_PREFIX" \
      -DCMAKE_PREFIX_PATH="$AUDIO_PREFIX" \
      -DCMAKE_INSTALL_LIBDIR=lib \
      -DCMAKE_C_COMPILER="$CC_BIN" \
      -DCMAKE_CXX_COMPILER="$CXX_BIN" \
      -DDISABLE_SDL=ON
    cmake --build "${AUDIO_BUILD_DIR}/resource_dasm" --target resource_file --parallel "$JOBS"
    touch "$RESOURCE_STAMP"
  fi
fi

[[ $CLEAN -eq 1 ]] && rm -rf "$BUILD_DIR"

EXTRA=()
if command -v ccache >/dev/null 2>&1; then
  EXTRA+=("-DCMAKE_C_COMPILER_LAUNCHER=ccache" "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache")
fi

cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$TYPE" \
  -DCMAKE_C_COMPILER="$CC_BIN" \
  -DCMAKE_CXX_COMPILER="$CXX_BIN" \
  -DPIKMIN_NATIVE_CPU="$NATIVE_CPU" \
  -DPIKMIN_LTO="$LTO" \
  -DPIKMIN_NATIVE_JAUDIO="$JAUDIO" \
  "${EXTRA[@]}"

cmake --build "$BUILD_DIR" --parallel "$JOBS"
echo "Built: ${BUILD_DIR}/bin/pikmin"
