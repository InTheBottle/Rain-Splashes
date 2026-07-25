# Rain Splashes

An SKSE plugin that adds rain splash effects to Skyrim.

> Early scaffolding — the plugin currently loads, sets up logging, and does nothing else.

## Requirements

- SKSE64 (SE/AE) or SKSEVR — the plugin is built multi-runtime.
- Address Library for SKSE Plugins.

## Building

Requires Visual Studio 2022, CMake 3.21+, and vcpkg (`VCPKG_ROOT` set). CommonLibSSE-NG is a
submodule under `extern/`.

```
git submodule update --init --recursive
cmake --preset release
cmake --build build/release --config Release --target RainSplashes
```

Set the `SKYRIM_MODS_FOLDER` environment variable to have the built DLL copied into a mod
folder automatically.

## Install

Copy `RainSplashes.dll` to `Data/SKSE/Plugins/`. Runtime logs are written to
`Documents/My Games/Skyrim Special Edition/SKSE/RainSplashes.log`.
