#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${ROOT}/build/linux/bin/pikmin"
ISO="${PIKMIN_ISO:-}"
if [[ $# -gt 0 && "$1" != --* ]]; then ISO="$1"; shift; fi
if [[ -z "$ISO" ]]; then
  echo "Usage: ./run.sh /path/to/Pikmin.iso [--fullscreen|--windowed] [extra args]" >&2
  exit 2
fi
ISO="$(realpath "$ISO")"
[[ -f "$ISO" ]] || { echo "Disc image not found: $ISO" >&2; exit 2; }
[[ -x "$BIN" ]] || "${ROOT}/build.sh"
if command -v vulkaninfo >/dev/null 2>&1; then vulkaninfo --summary >/dev/null 2>&1 || { echo "Vulkan is not available. Check your Vulkan ICD/driver." >&2; exit 3; }; fi
export PIKMIN_ISO="$ISO"
export PIKMIN_ANISO="${PIKMIN_ANISO:-16}"
export PIKMIN_MSAA="${PIKMIN_MSAA:-1}"
export PIKMIN_VSYNC="${PIKMIN_VSYNC:-0}"
export PIKMIN_AUDIO_PLACEHOLDER_TONES="${PIKMIN_AUDIO_PLACEHOLDER_TONES:-0}"
exec "$BIN" --iso "$ISO" "$@"
