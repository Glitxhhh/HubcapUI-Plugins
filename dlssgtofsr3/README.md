# dlssg-to-fsr3 (HubcapUI plugin)

Installs [Nukem's dlssg-to-fsr3](https://www.nexusmods.com/site/mods/738) (replaces
Nvidia DLSS Frame Generation with AMD FSR 3 Frame Generation) for a selected Steam game.
Adds **Enable dlssg-to-fsr3 (variant)** / **Remove dlssg-to-fsr3** to the Library's Game
Settings menu.

Requires a Nexus API key in Hubcap Advanced Settings (shared with RenoDX/OptiScaler).

## Build

```powershell
./dlssgtofsr3/scripts/build.ps1
```

Output: `dlssgtofsr3/dist/dlssgtofsr3.dll`. Install the same way as the other plugins
here; see the [developer guide](../docs/plugin-development.md).

## Variant selection

dlssg-to-fsr3 ships three Nexus files per release: a plain build, a **Universal**
build, and a **DLSSTweaks Edition** build. The Universal and DLSSTweaks Edition builds
are not interchangeable — DLSSTweaks Edition is built specifically to coexist with
DLSSTweaks instead of both tools hooking the same things. This plugin:

1. Checks whether DLSSTweaks is present for the selected game (preferring the
   `dlsstweaks` plugin's own install manifest if it installed it, falling back to
   detecting `DLSSTweaksConfig.exe`/`dlsstweaks.ini` for a manual install).
2. Downloads the matching Nexus file: **DLSSTweaks Edition** if DLSSTweaks was
   detected, **Universal** otherwise.
3. Extracts it next to the game's executable and records what it wrote so **Remove**
   is precise. Switching variants (e.g. installing DLSSTweaks later) replaces the
   previous install rather than leaving both variants' files behind.

**Known gap:** the exact manual step (if any) each ZIP's own `READ ME.txt` still asks
for was not verified against a real download while building this plugin (Nexus
requires a logged-in session to fetch files). Verify the first real install against
that README before relying on fully unattended installs.
