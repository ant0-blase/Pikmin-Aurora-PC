# Pikmin — Native Linux / Aurora Port

Experimental native Linux port of **Pikmin (GameCube)** built from the projectPiki decompilation and an Aurora compatibility layer.

The repository keeps the original matching-decomp sources while adding a separate host build for Linux. The native target uses Aurora for GameCube OS/GX/PAD/DVD/CARD compatibility, Vulkan/WebGPU for rendering, SDL3 for host input/audio, and a software JAudio path for Pikmin's sequence/sample data.

> **No game disc image or copyrighted game assets are included.** You must provide your own legally obtained Pikmin GameCube disc image at runtime.

## Status

The native port reaches gameplay and the first area has been exercised successfully during development. Current work is focused on native audio fidelity, host lifecycle robustness, and remaining game-specific compatibility issues.

Implemented native-port work includes:

- Vulkan/WebGPU rendering through Aurora
- direct ISO access through Aurora DVD/Nod
- SDL3 gamepad input
- persistent GCI-folder memory card support
- host 64-bit ABI fixes for matrices, animation caches, allocators, texture objects, IDs and P2D data
- fullscreen/windowed switching and safer focus/minimize lifecycle handling
- native STX playback with SDL3 resampling/low-latency buffering
- experimental software JAudio synthesis for `.jam`, `pikibank.bx` and `.aw` resources
- Release builds with `-O3`, optional LTO, ccache, and optional native CPU tuning

See [docs/NATIVE_LINUX.md](docs/NATIVE_LINUX.md) for technical details and current limitations.

## Requirements

Linux x86-64 is the primary development target.

Required tools:

- CMake 3.25+
- Ninja
- Git
- Clang or GCC with C++23 support
- zlib development files
- Vulkan loader/driver
- an Internet connection on the first build so pinned third-party dependencies can be fetched

Common packages can be installed automatically:

```bash
./build.sh --deps
```

## Build

```bash
git clone <your-repository-url>
cd <repository-directory>
./build.sh
```

The first build fetches pinned revisions of Aurora, phosg and resource_dasm. Generated dependencies and build products live under `extern/` and `build/` and are ignored by Git.

Useful build options:

```bash
./build.sh --clean          # clean native build
./build.sh --debug          # RelWithDebInfo
./build.sh --portable       # disable -march=native/-mtune=native
./build.sh --no-lto         # disable IPO/LTO
./build.sh --no-jaudio      # build without the experimental software JAudio synth
./build.sh --jobs=8         # override parallel job count
```

`./build.sh` enables the native JAudio bridge by default.

## Run

```bash
./run.sh /path/to/Pikmin.iso
```

Window modes:

```bash
./run.sh /path/to/Pikmin.iso --fullscreen
./run.sh /path/to/Pikmin.iso --windowed
```

At runtime, **F11** or **Alt+Enter** toggles fullscreen.

Useful environment variables:

```bash
PIKMIN_VSYNC=1 ./run.sh /path/to/Pikmin.iso
PIKMIN_MSAA=4 PIKMIN_ANISO=16 ./run.sh /path/to/Pikmin.iso
PIKMIN_JAUDIO=0 ./run.sh /path/to/Pikmin.iso
```

The application caches extracted runtime audio resources under the user's cache directory; those files are generated from the user's own disc image and are never part of this repository.

## Native JAudio

The GameCube DSP/JAudio hardware path is not executed directly on the host. Instead, the native target uses a software bridge based on pinned `resource_dasm`/`smssynth` code to interpret Pikmin BMS/JAM sequences and sample banks while SDL3 owns host playback.

This path is still experimental. If it causes a build or runtime regression, build the graphics/gameplay port without it:

```bash
./build.sh --clean --no-jaudio
```

STX streaming audio remains available through the native SDL3 path.

## Original decompilation workflow

The projectPiki matching workflow remains separate from the native CMake target:

```bash
python configure.py
ninja
```

The default native revision is `GPIE01_01` (USA Rev 1). Matching/decomp configuration files for the other projectPiki revisions remain under `config/` and `orig/`.

## Repository layout

```text
src/port/                  Linux/Aurora host bridges
cmake/PikminNativeSources.cmake
patches/                   patches applied to the pinned Aurora checkout
build.sh                   dependency bootstrap + native build
run.sh                     ISO launcher
config/, orig/             original projectPiki matching/decomp workflow
docs/NATIVE_LINUX.md       native-port implementation notes
```

## CI

`.github/workflows/native-linux.yml` builds the native Linux target on pushes and pull requests. The original matching-decomp workflow is kept as a manual workflow.

## Credits

- **projectPiki** and its contributors — original Pikmin decompilation project
- **encounter/aurora** — GameCube/Wii host compatibility layer
- **fuzziqersoftware/phosg**
- **fuzziqersoftware/resource_dasm / smssynth** — JAudio data decoding and software synthesis work used by the experimental native audio bridge

See [THIRD_PARTY.md](THIRD_PARTY.md) for pinned dependency revisions.

## License

This repository retains the upstream project license in [LICENSE.md](LICENSE.md). Third-party dependencies retain their own licenses and are fetched separately during the build.
