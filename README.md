# HubcapUI Plugins

Independent plugins for [HubcapUI](https://github.com/oogiefied/HubcapUI).

| Plugin | DLL | Purpose |
| --- | --- | --- |
| RenoDX | `renodx.dll` | HDR mod discovery, installation, configuration, and removal. |
| OptiScaler | `optiscaler.dll` | Install and configure OptiScaler for supported games. |

## Install

Download the DLLs you want from [Releases](https://github.com/YaBoiDaddy/HubcapUI-Plugins/releases/latest). Fully exit Steam, copy them into `Steam/hubcapui-plugins`, then restart Steam. Enable the plugin system and your selected plugins in Hubcap Settings, save, and restart when prompted.

## Automatic updates

These plugins declare their own update source. A Hubcap host that implements the [plugin update standard](docs/plugin-updates.md) checks for stable GitHub releases in the background and installs verified DLLs before loading plugins on the next Steam restart. Plugin releases are independent of the main Hubcap DLL. Older hosts can continue using manual DLL installation.

The initial host support is being developed against upstream HubcapUI v1.0.9; installing a stock older host does not add the updater by itself.

RenoDX also refreshes its compatibility list from the RenoDX wiki; that is separate from updating the plugin DLL.

## Build

Use Windows x64, PowerShell 7, and Visual Studio C++ Build Tools with the Windows SDK:

```powershell
./build-all.ps1
```

Each plugin builds independently with `./scripts/build.ps1` from its directory. Outputs go in each plugin's `dist` folder. Only use `-Install` when Steam is fully stopped.

## Release

Set root `VERSION`, every embedded `src/plugin.json` version, and every plugin's runtime version to the same semantic version. Run `./scripts/verify-release.ps1`. Push a `vX.Y.Z` tag; the release workflow builds both DLLs and publishes them with dependency notices. The updater uses GitHub's SHA-256 asset digests, not a mutable checksum file.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for bundled dependencies. RenoDX, ReShade, and OptiScaler game modifications are obtained separately by their respective plugins.
