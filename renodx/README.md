# Hubcap RenoDX Plugin

> [!WARNING]
> **Alpha software — expect bugs.** These plugins are in active development. Features may be incomplete or behave unexpectedly.

RenoDX automatically detects supported games in your Steam library by matching them against the latest RenoDX compatibility list. Library badges make those games easy to spot; green badges show where RenoDX is enabled.

![RenoDX badges beside games in the Steam library](../docs/images/renodx-library-badges.png)

Open **Game Settings → Enable RenoDX** for a one-click installation workflow. The plugin handles the RenoDX mod, the required ReShade build, and supported profile settings. Once installed, the same menu offers **Remove RenoDX**, as shown below.

![Game Settings showing an installed RenoDX mod and its removal option](../docs/images/renodx-game-settings.png)

Mods hosted exclusively on Nexus Mods or Discord may require an additional download step.

Self-contained working directory for `hubcap.renodx`. Its manifest, JavaScript UI, and SVG assets are embedded into the resulting `renodx.dll`.

## Layout

- `src/plugin.cpp` — plugin ABI wrapper and embedded Steam UI
- `src/feature_engine.cpp` — RenoDX detection/install/remove backend
- `src/plugin.json` and `src/plugin.rc` — embedded plugin metadata
- `sdk/` — snapshot of the Hubcap plugin ABI
- `third_party/` — native source dependencies
- `scripts/build.ps1` — independent MSVC Release build
- `dist/` — build output

## Build

```powershell
pwsh -NoProfile -File .\scripts\build.ps1
```

The DLL is written to `dist\renodx.dll`. Close Steam and add `-Install` to copy it into `Steam\hubcapui-plugins`.

After changing plugin metadata, keep `src/plugin.json` and the values returned by `HubcapPluginInitializeV1` in `src/plugin.cpp` synchronized.

## Compatibility refresh

The plugin fetches the RenoDX wiki catalog on demand and checks it once per minute while the UI is loaded. Successful catalogs are reused for 15 minutes. Failed or empty downloads preserve the last successful in-memory catalog, with automatic retries limited to once per minute. The cache does not persist across Steam restarts.

Under Hubcap Settings > Advanced Settings > Plugins > RenoDX, **Refresh compatibility** bypasses the cache. The status shows the last successful download time and indicates when a failed refresh is using the previous list. Library badges update when a refreshed catalog arrives.

## Refresh verification

Run `node tests/catalog-ui.test.cjs` and `pwsh -NoProfile -File tests/run-catalog-tests.ps1` from this plugin directory. The UI test uses an isolated host; the native test exercises the production cache function with a simulated clock and HTTP responses, including failed and empty refreshes, cache expiry, retry delays, and forced refreshes. Neither test controls Steam.
