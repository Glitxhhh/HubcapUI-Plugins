# DLSS Enabler (HubcapUI plugin)

Installs [artur07305's DLSS Enabler](https://www.nexusmods.com/site/mods/757) (DLSS
Multi-Frame Generation on any DirectX 12 GPU) for a selected Steam game. Adds
**Enable DLSS Enabler** / **Remove DLSS Enabler** to the Library's Game Settings menu.

Requires a Nexus API key in Hubcap Advanced Settings (shared with RenoDX/OptiScaler).

**Do not use in multiplayer games** — anti-cheat may flag the frame-generation
injection as cheating. The plugin's confirmation dialog repeats this warning.

## Build

```powershell
./dlssenabler/scripts/build.ps1
```

Output: `dlssenabler/dist/dlssenabler.dll`. Install the same way as the other plugins
here; see the [developer guide](../docs/plugin-development.md).

## Proxy DLL naming

DLSS Enabler ships as a single proxy DLL that has to be renamed to one of a handful of
well-known Windows DLL names next to the game's executable. This plugin downloads the
Nexus file (handling it whether it's the raw DLL or a ZIP containing it), then places
it under the first available name in the mod's own documented fallback order:
`dxgi.dll`, `version.dll`, `winmm.dll`, `dbghelp.dll`, `psapi.dll`, `winhttp.dll`. If
every one of those already exists in the game folder (most likely `dxgi.dll` from
ReShade), install is refused rather than overwriting something else's proxy DLL.

**Known gap:** the exact filename of the DLL inside the Nexus ZIP was not verified
against a real download while building this plugin (Nexus requires a logged-in
session to fetch files); the plugin picks the largest non-`nvapi`/`nvngx` `.dll` entry
as a heuristic. Verify the first real install.
