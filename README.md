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
