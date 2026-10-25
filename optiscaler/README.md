# Hubcap OptiScaler Plugin

> [!WARNING]
> **Alpha software — expect bugs.** These plugins are in active development. Features may be incomplete or behave unexpectedly.

OptiScaler brings a one-click installation experience to any installed game. Right-click it in your Steam library and open **Manage → Enable OptiScaler** (or, with Hubcap Settings > Advanced Settings > "Plugin/Lua Menu" set to "Game Settings," that game's Game Settings dropdown instead) to start setup directly, without manually placing the OptiScaler files in the game folder.

Self-contained working directory for `hubcap.optiscaler`. Its manifest, JavaScript UI, and SVG assets are embedded into the resulting `optiscaler.dll`.

## Layout

- `src/plugin.cpp` — plugin ABI wrapper and embedded Steam UI
- `src/feature_engine.cpp` — OptiScaler detection/install/remove backend
- `src/plugin.json` and `src/plugin.rc` — embedded plugin metadata
- `sdk/` — snapshot of the Hubcap plugin ABI
- `third_party/` — native source dependencies
- `scripts/build.ps1` — independent MSVC Release build
- `dist/` — build output

## Build

```powershell
pwsh -NoProfile -File .\scripts\build.ps1
```

The DLL is written to `dist\optiscaler.dll`. Close Steam and add `-Install` to copy it into `Steam\hubcapui-plugins`.

After changing plugin metadata, keep `src/plugin.json` and the values returned by `HubcapPluginInitializeV1` in `src/plugin.cpp` synchronized.
