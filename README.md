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

Requires SKSE64 and Address Library for SKSE Plugins. Copy all three trees into `Data/`:

```
Data/SKSE/Plugins/RainSplashes.dll
Data/SKSE/Plugins/RainSplashes.ini
Data/meshes/RainSplash/soggyfeet_splash.nif
Data/textures/RainSplash/*.dds
```

The meshes and textures are not optional — the DLL spawns
`meshes/RainSplash/soggyfeet_splash.nif`, which references the textures by path. With the
DLL alone the plugin loads and logs normally but no splash is ever visible.

Runtime logs are written to
`Documents/My Games/Skyrim Special Edition/SKSE/RainSplashes.log`.

## Settings

`Data/SKSE/Plugins/RainSplashes.ini` is read once at startup, so edits need a restart.
The ini is optional; deleting a key or the whole file falls back to defaults.

| Key | Default | Range | Meaning |
| --- | --- | --- | --- |
| `Scale` | `1.0` | 0.1 - 5.0 | Splash size multiplier, on top of the per-splash variation. |
| `MaxNPCs` | `12` | 0 - 200 | How many NPCs may splash at once. The player is never counted and never dropped. |

The loaded values are logged on startup, which is the quickest way to confirm the ini is
being picked up.

## Packaging

`cmake --build build/release --config Release --target RainSplashes`, then zip the DLL
plus the `SKSE/`, `meshes/` and `textures/` folders with the archive rooted at `Data/`, so
mod managers install it without repackaging.
