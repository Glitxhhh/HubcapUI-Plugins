# Hello Test

The SDK test suite plugin — exercises every HubcapUI UI bridge surface beyond the basic one `hello-hubcap` demonstrates, in one place, so there's a single plugin to rebuild and click through whenever the SDK grows further:

- **`host.setPluginSettingsEntry(id, spec)`** — a "Say Hello (Settings)" entry on this plugin's own row in Hubcap Settings.
- **`host.setManageMenuEntry(id, spec)`** — a "Say Hello (Manage)" entry for whichever game is selected, shown in the Game Settings dropdown or the Library grid's native right-click Manage submenu depending on which one the user has active in Hubcap Settings > Advanced Settings > "Plugin/Lua Menu" — one registration, both surfaces.
- **`host->send_notification(title, body)`** — fired natively on every successful round trip from either entry above, showing a real Windows notification. This one is native-only (no JS-side equivalent); it's only callable from a plugin's own `handle()`.

Deliberately does **not** demonstrate `host.setGameSetting` — [`hello-hubcap`](../hello-hubcap) remains the canonical starting-point template for that and for new plugins generally. This plugin is for exercising the newer, additional surfaces, not for copying as a starter.

From the repository root, with PowerShell 7:

```powershell
./examples/hello-test/scripts/build.ps1
./scripts/test-plugin-template.ps1
```

The build requires Visual Studio C++ Build Tools with the Windows SDK, or a Windows-capable Zig toolchain (`HUBCAP_ZIG` can point to `zig.exe`). Output: `examples/hello-test/dist/hello-test.dll`. Building and testing do not install the example or restart Steam.

To try it manually, fully exit Steam and copy the DLL to `<Steam root>/hubcapui-plugins/hello-test.dll`. Start Steam, enable the plugin system and **Hello Test** in Hubcap Settings, save, and restart when prompted. Then either:
- Open Hubcap Settings, click the gear next to **Hello Test**, and click **Say Hello (Settings)**, or
- Right-click any game in your Library grid, hover **Manage**, and click **Say Hello (Manage)** — or, with Hubcap Settings > Advanced Settings > "Plugin/Lua Menu" set to "Game Settings," open that game's Game Settings dropdown instead.

Either path opens a confirm dialog, does a native round trip, and — on success — pops a real Windows notification.

`setPluginSettingsEntry`, `setManageMenuEntry`, and `send_notification` are all newer than the original bridge — an older host build may be missing one, two, or all three. This example feature-detects each independently (`typeof host.setPluginSettingsEntry === 'function'`, same for `setManageMenuEntry`, and a `g_host->size`/null check before calling `send_notification` natively) and simply skips whichever isn't present, rather than assuming all are available together. Do the same in your own plugin.

`setManageMenuEntry` specifically is reached by patching a method Steam's own menu system calls internally (not a native ABI hook), so it depends on Steam client internals that can shift between client updates — see `docs/context-menu-and-route-proposal.md` in the HubcapUI host repo for exactly how, if it ever needs re-verifying against a newer Steam client.

For your own plugin, change the ID, name, description and version in both `src/plugin.json` and `src/plugin.cpp`, the JavaScript ID and guard, and the output DLL name in `scripts/build.ps1`. Use a unique ID rather than the example ID.

The build uses the repository's [SDK header](../../sdk/hubcap_plugin.h) and [yyjson](../../renodx/third_party/yyjson.h), whose license is in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md). When moving the template into a separate repository, carry those files and the yyjson license and adjust the include paths. Release your own raw DLL; the sample is excluded from this repository's RenoDX/OptiScaler release workflow.

See the [developer guide](../../docs/plugin-development.md) for the native and UI contracts, compatibility limits and optional updates.
