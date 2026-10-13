# DLSSTweaks (HubcapUI plugin)

Installs [emoose's DLSSTweaks](https://www.nexusmods.com/site/mods/550) for a selected
Steam game. Adds **Enable DLSSTweaks** / **Remove DLSSTweaks** to the Library's Game
Settings menu.

Requires a Nexus API key in Hubcap Advanced Settings (shared with RenoDX/OptiScaler).

## Build

```powershell
./dlsstweaks/scripts/build.ps1
```

Output: `dlsstweaks/dist/dlsstweaks.dll`. Install by copying it into
`<Steam root>/hubcapui-plugins` with Steam fully closed, then enable it in Hubcap
Settings and restart Steam. See the [developer guide](../docs/plugin-development.md)
for the full install/verify steps.

## What it does

1. Resolves the selected Library game's install folder through Steam's own
   `libraryfolders.vdf`/`appmanifest_*.acf` files.
2. Downloads DLSSTweaks' single Nexus file (a ZIP containing `dxgi.dll`,
   `dlsstweaks.ini`, and `DLSSTweaksConfig.exe`) via the Nexus Mods API.
3. Extracts it next to the game's executable and launches `DLSSTweaksConfig.exe` so the
   user can finish the NVIDIA signature-override setup themselves (that step needs a
   GUI and isn't something this plugin should do silently).
4. Records exactly which files it wrote in a small per-game manifest, so **Remove**
   deletes only those files, never anything that was already in the folder.

If `dxgi.dll` already exists in the game folder from something else (most likely
ReShade), install is refused rather than overwritten.
