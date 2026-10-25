# Hello Hubcap

A small HubcapUI ABI v1 plugin that adds **Hello Hubcap** to a game's per-game actions in Library — the Game Settings dropdown or the native right-click **Manage** submenu, whichever the user has selected in Hubcap Settings (one `host.setManageMenuEntry` call supports both automatically). Selecting it opens the host's shared confirmation dialog and sends a JSON request to the native DLL. The reply appears in the same dialog. It does not modify game files.

From the repository root, with PowerShell 7:

```powershell
./examples/hello-hubcap/scripts/build.ps1
./scripts/test-plugin-template.ps1
```

The build requires Visual Studio C++ Build Tools with the Windows SDK, or a Windows-capable Zig toolchain (`HUBCAP_ZIG` can point to `zig.exe`). Output: `examples/hello-hubcap/dist/hello-hubcap.dll`. Building and testing do not install the example or restart Steam.

To try it manually, fully exit Steam and copy the DLL to `<Steam root>/hubcapui-plugins/hello-hubcap.dll`. Start Steam, enable the plugin system and **Hello Hubcap** in Hubcap Settings, save, and restart when prompted. Right-click a game in Library and open **Manage → Hello Hubcap**, or, with Hubcap Settings > Advanced Settings > "Plugin/Lua Menu" set to "Game Settings," open that game's Game Settings dropdown instead — both show the same entry. Remove it by disabling and saving, exiting Steam, deleting the example DLL, and starting Steam again.

For your own plugin, change the ID, name, description and version in both `src/plugin.json` and `src/plugin.cpp`, the JavaScript ID and guard, and the output DLL name in `scripts/build.ps1`. Use a unique ID rather than the example ID. Extend `handle()` with your own `data.pluginAction` commands; parse arguments from `data.pluginData` using yyjson.

The build uses the repository's [SDK header](../../sdk/hubcap_plugin.h) and [yyjson](../../renodx/third_party/yyjson.h), whose license is in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md). When moving the template into a separate repository, carry those files and the yyjson license and adjust the include paths. Release your own raw DLL; the sample is excluded from this repository's RenoDX/OptiScaler release workflow.

See the [developer guide](../../docs/plugin-development.md) for the native and UI contracts, compatibility limits and optional updates.
