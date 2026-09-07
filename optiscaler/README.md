# Hubcap OptiScaler Plugin

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
