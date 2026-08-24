# Native Linux port notes

## Architecture

The host build is intentionally separate from the original matching GameCube build.

- **Aurora** replaces low-level GameCube SDK services used by the native target.
- **GX/GD** commands are translated through Aurora and rendered through Dawn/WebGPU (Vulkan on Linux).
- **DVD/Nod** mounts the user's disc image directly.
- **PAD/SDL3** supplies host controller input.
- **CARD** uses Aurora's persistent GCI-folder backend with a small compatibility patch for Pikmin's transactional save flow.
- **STX audio** is decoded by the native port and streamed through SDL3.
- **JAudio** is optional and uses a software BMS/sample-bank bridge based on pinned resource_dasm/smssynth code.

The original PowerPC DSP/ARAM implementation is not executed directly on x86-64.

## Build modes

Normal Release build:

```bash
./build.sh
```

Portable build suitable for CI/release artifacts:

```bash
./build.sh --portable
```

Debuggable build:

```bash
./build.sh --debug --no-lto
```

Fallback without software JAudio:

```bash
./build.sh --clean --no-jaudio
```

## Runtime

```bash
./run.sh /path/to/Pikmin.iso
```

`F11` and `Alt+Enter` toggle fullscreen. The native lifecycle code suppresses rendering while the host surface is unavailable/minimized to avoid submitting frames to a destroyed Dawn/Vulkan surface.

## Important host-port fixes

The native port contains explicit fixes for several assumptions that are valid on GameCube but invalid on little-endian 64-bit hosts:

- FourCC / `ID32` byte order
- P2D pane tags
- host-size GX opaque objects
- 3x4 affine matrix semantics
- matrix/envelope accumulation on non-PowerPC builds
- 64-bit animation frame-cache pointer layout
- `AyuCache` allocator units based on native `sizeof(MemHead)`
- main-thread VI/retrace callbacks
- main-thread Aurora frame ownership during loading
- synchronous CARD operations where the original passes null async callbacks

## Aurora patches

`build.sh` checks out a pinned Aurora revision and idempotently applies:

- `patches/aurora-optional-haptic.patch`
- `patches/aurora-gci-folder-card.patch`

The patches are intentionally kept outside `extern/aurora` so the Git repository contains only project-owned integration changes; the third-party checkout is generated on first build.

## Audio

### STX

STX streams use their real source sample rate (44.1 or 48 kHz) and SDL3 handles conversion to the host device. Queue targets are kept small to avoid the large latency of the early native prototype.

### Software JAudio

When `PIKMIN_NATIVE_JAUDIO=ON` (the `build.sh` default), the build fetches pinned phosg/resource_dasm revisions and compiles `src/port/jaudio_smssynth_bridge.cpp` as C++23. The bridge loads Pikmin's `.jam`, `pikibank.bx` and `.aw` data extracted at runtime from the user's disc image, then feeds synthesized stereo float PCM to SDL3.

Pikmin-specific application-port handling for the core SE sequence is implemented in the bridge. This path remains experimental and can be disabled at build time with `--no-jaudio`.

## Current status

The native port reaches gameplay; the first gameplay area has been exercised successfully during development. Remaining issues should be treated as port compatibility bugs rather than proof of full-game completion.
