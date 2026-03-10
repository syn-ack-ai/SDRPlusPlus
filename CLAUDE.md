# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

SDR++ is a cross-platform SDR (Software Defined Radio) application written in C++17. It uses a modular plugin architecture with a core shared library and dynamically-loaded modules.

## Build Commands

### macOS Development Build (default for all local work)
After any code change, rebuild and re-bundle with:
```sh
./make_macos_bundle_local.sh
```
This configures CMake, builds all enabled targets, creates the `dist/SDR++.app` bundle, fixes dylib dependencies, and codesigns. It auto-detects Homebrew libraries and enables matching modules. Run the app with `open dist/SDR++.app`.

### Manual CMake Build (no bundle)
```sh
mkdir build && cd build
cmake .. -DUSE_BUNDLE_DEFAULTS=ON    # macOS bundle mode
cmake --build . -j$(sysctl -n hw.ncpu)
```

### Build a Single Module
```sh
cmake --build build --target <module_name>   # e.g. radio, rtl_sdr_source, recorder
```

### Key CMake Options
- `-DOPT_BUILD_<MODULE_NAME>=ON` — enable optional modules (most are OFF by default)
- `-DUSE_BUNDLE_DEFAULTS=ON` — use macOS bundle-relative paths
- `-DOPT_OVERRIDE_STD_FILESYSTEM=ON` — needed for macOS < Catalina

### Core Dependencies (macOS via Homebrew)
`fftw`, `glfw`, `volk`, `libzstd`, `pkg-config`, `cmake`

## Architecture

### Core Library (`core/src/`)
Builds `libsdrpp_core` (shared library). Key subsystems:
- **`dsp/`** — Signal processing blocks: filters, demodulation, resampling, math
- **`gui/`** — ImGui-based UI: main window, widgets, dialogs, themes
- **`signal_path/`** — VFO management, sink/source routing, IQ frontend
- **`backends/`** — Platform backends (GLFW for desktop, Android)
- **`module.h`** — Module manager: dynamic loading of plugins via dlopen/LoadLibrary
- **`config.h`** — JSON-based configuration system

### Module System
All modules are shared libraries loaded at runtime. They follow a standard interface defined via macros:

```cpp
SDRPP_MOD_INFO { name, description, author, versionMajor, versionMinor, versionBuild, maxInstances }
MOD_EXPORT void _INIT_()
MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name)
MOD_EXPORT void _DELETE_INSTANCE_(void* instance)
MOD_EXPORT void _END_()
```

Module build boilerplate is in `sdrpp_module.cmake` — each module's CMakeLists.txt defines `SRC` and includes this file.

### Module Categories
| Directory | Purpose | Examples |
|-----------|---------|---------|
| `source_modules/` | Hardware/network SDR sources | `rtl_sdr_source`, `airspy_source`, `file_source` |
| `sink_modules/` | Audio/network output | `portaudio_sink`, `new_portaudio_sink`, `network_sink` |
| `decoder_modules/` | Signal decoders | `radio` (AM/FM/SSB), `meteor_demodulator`, `m17_decoder` |
| `misc_modules/` | Utilities | `recorder`, `frequency_manager`, `rigctl_server`, `scanner` |

### Entry Point
`src/main.cpp` calls `sdrpp_main()` from the core library. CLI args: `-r <root_dir>` (config directory), `-c` (keep console on Windows).

### Resources (`root/res/`)
- `bandplans/` — Band frequency allocations (JSON)
- `colormaps/` — Waterfall color schemes (JSON)
- `themes/` — ImGui themes
- `fonts/`, `icons/`

## Code Style

- `.clang-format` in repo root (LLVM-based, 4-space indent, no column limit, custom brace wrapping with `else`/`catch` on new line)
- C++17 standard
- Pointer alignment: left (`int* ptr`)
- Short functions/lambdas/blocks allowed on single line
- Namespace contents are indented

## Important Notes

- The upstream project's contributing policy explicitly forbids AI-generated code in pull requests
- ImGui version: 1.92.6 (vendored in `core/src/imgui/`)
- Modules install to `lib/sdrpp/plugins`
- macOS bundle uses `macos/bundle_utils.sh` for dylib dependency resolution and relinking
