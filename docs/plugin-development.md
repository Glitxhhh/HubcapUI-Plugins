# Develop a HubcapUI plugin

HubcapUI plugins are Windows x64 DLLs with an embedded JSON manifest, a small native ABI, and optional JavaScript UI. Start with [Hello Hubcap](../examples/hello-hubcap/) instead of copying the RenoDX or OptiScaler feature engines. The shared [SDK header](../sdk/hubcap_plugin.h) defines the binary contract; the existing [OptiScaler implementation](../optiscaler/src/plugin.cpp) demonstrates a larger plugin.

This guide describes the ABI v1 loader and `HubcapPluginHost.version === 1` UI bridge. A host must implement both to run the example's UI. Installing a plugin does not add plugin support to an older host. ABI version 1, manifest `schemaVersion: 1`, and a plugin's own release version are separate values.

## Build and check the starter

Use Windows x64, PowerShell 7, and Visual Studio C++ Build Tools with the Windows SDK, or a Windows-capable Zig toolchain (`HUBCAP_ZIG` can point to `zig.exe`). The test script also requires Node.js. From the repository root:

```powershell
pwsh -NoProfile -File ./examples/hello-hubcap/scripts/build.ps1
pwsh -NoProfile -File ./scripts/test-plugin-template.ps1
```

The starter produces `examples/hello-hubcap/dist/hello-hubcap.dll`. The test script builds the starter and checks its manifest, initializer, native request/response behavior, and embedded UI without loading it into Steam. Passing these checks establishes the tested contract; it does not establish that Steam's current UI renders correctly.

To create your own plugin, copy the example to your own repository and preserve its SDK include and resource build steps. Carry the shared SDK, the yyjson source/header used by the build, and its license from [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). Rename the DLL and choose an ID unique to you, such as `yourname.myplugin`. Update the ID, name, version, and description in both the embedded manifest and the runtime initializer, and replace the example's JavaScript ID and installation marker. Update build and test paths if you move the files. Keep your plugin's version independent of this repository's RenoDX/OptiScaler release version.

## Manifest and discovery

The DLL must embed UTF-8 JSON as **Windows RCDATA resource ID 1**. The host discovers `*.dll` directly inside `Steam/hubcapui-plugins`; a loose `plugin.json` alongside a DLL does not satisfy discovery. The starter's [resource file](../examples/hello-hubcap/src/plugin.rc) embeds its [manifest](../examples/hello-hubcap/src/plugin.json):

```rc
#include <windows.h>
1 RCDATA "plugin.json"
```

```json
{
  "schemaVersion": 1,
  "id": "yourname.myplugin",
  "name": "My Plugin",
  "version": "0.1.0",
  "description": "Adds my plugin's features to HubcapUI.",
  "order": 200
}
```

| Field | Contract |
| --- | --- |
| `schemaVersion` | Use `1` for this manifest format. The current ABI v1 scanner does not itself reject an absent or different schema value; do not rely on that permissiveness. |
| `id` | Required, 1–96 characters using only lowercase ASCII letters, digits, `.`, `-`, or `_`. It must equal the runtime `HubcapPluginV1.id`. Keep it stable across releases. |
| `name` | Required, nonempty string shown in Settings. |
| `version` | Required, nonempty string. Use semantic versions, and keep the runtime version equal to it. Automatic updates require the stricter release format below. |
| `description` | Description shown by the host; include it even though the current scanner permits an empty description. |
| `order` | Optional integer; lower values sort first, then name. |

The current scanner reads manifests without executing plugin code and ignores invalid JSON, missing resources, resources over 1 MiB, and manifests missing valid required fields. Duplicate IDs are ignored after the first discovered instance; do not install two DLLs with the same ID. Discovery is cached for the process lifetime, so restart after adding or replacing DLLs.

## Native ABI v1

Export this unmangled C symbol, using the SDK's calling convention:

```cpp
extern "C" __declspec(dllexport) bool HUBCAP_PLUGIN_CALL
HubcapPluginInitializeV1(const HubcapPluginHostV1* host,
                         HubcapPluginV1* plugin);
```

Do not alter the SDK's struct layout, field order, packing, or ABI constant. Check both input pointers, `host->abi_version`, `host->size`, and the caller-provided `plugin->size` before reading or writing fields. Populate the complete `HubcapPluginV1`, set `size = sizeof(HubcapPluginV1)` and `abi_version = HUBCAP_PLUGIN_ABI_VERSION`, and return `true` only on success. Use `false` for an unsupported host or output buffer.

The host currently rejects an initializer that returns `false`, an ABI other than 1, a returned struct smaller than `HubcapPluginV1`, a runtime ID different from the manifest, or missing `handle`/`free_string` callbacks. `ui_script` and `shutdown` can be null. Runtime metadata should match the manifest even where the current loader only checks ID equality.

| Host member | Use |
| --- | --- |
| `log(plugin_id, message)` | Write a diagnostic through Hubcap's runtime logger. |
| `steam_root()` | Borrow a null-terminated wide string containing the Steam root. |
| `local_app_data()` | Borrow a null-terminated wide string containing the local application data root. Use a plugin-specific subdirectory for your own data. |

### Ownership and lifetime

- Treat request JSON as borrowed input valid only during `handle`. Copy anything needed after the call.
- Return a null-terminated UTF-8 JSON **object** from `handle`. The host copies the returned bytes and calls your `free_string` exactly once for a non-null response. Allocate and release the response with the same allocator inside your plugin; the host must not free it with its own C++ runtime. The starter demonstrates this pairing.
- The loader stores pointers to runtime metadata and `ui_script`. Keep them valid for the DLL lifetime, for example as string literals or static arrays; do not return pointers into local variables.
- Host path strings are borrowed. Do not free or modify them. Copy them if your code needs independently owned storage.
- A null response becomes a host error. Malformed JSON or a non-object response is rejected by the UI command service, so return a valid error object on failures.

### Execution and threading

Keep `DllMain` minimal; do not perform networking, start complex work, or acquire application locks under the Windows loader lock. Initialize through the exported initializer and handle requests through the ABI.

The current host serializes plugin initialization and dispatch with a plugin-manager mutex and holds it while calling `handle` and `free_string`. Slow synchronous work delays other plugin commands and manager queries. There is no ABI v1 promise that callbacks run on Steam's UI thread, and there is no native asynchronous result callback. For long operations, use your own worker with a request/status polling protocol and protect shared state. Do not retain borrowed request pointers or attempt to re-enter the plugin manager while a callback holds its lock.

The SDK declares `shutdown`, but the current loader does not call it. Do not depend on it to save data or stop essential work. Save durable state as operations complete; keep failure paths safe if initialization is rejected and Windows immediately unloads the DLL.

## UI bridge and command format

`ui_script` is JavaScript source, not a filename. The host appends enabled plugins' scripts after its UI bridge in injected Steam contexts. Wrap the script in an IIFE, check the bridge and its version, and use a unique per-context marker so repeated injection does not add duplicate observers or timers:

```javascript
(() => {
  const host = globalThis.HubcapPluginHost;
  if (!host || host.version !== 1 || globalThis.__myPluginInstalled) return;
  globalThis.__myPluginInstalled = true;
  const ID = 'yourname.myplugin';
  host.onResult(ID, (response, envelope) => {
    // Handle plugin errors as well as successful responses.
    console.log(response);
  });
  host.send(ID, 'state', {appId: host.libraryAppId()});
})();
```

`send` returns a request ID, not a Promise. Register `onResult` before sending. The host passes the full request envelope to native `handle`, not just your payload:

```json
{
  "v": 1,
  "id": "hc-example-1",
  "action": "pluginCommand",
  "appId": "123",
  "href": "https://example.invalid/",
  "data": {
    "pluginId": "yourname.myplugin",
    "pluginAction": "state",
    "pluginData": {"appId": "123"},
    "appId": "123"
  }
}
```

Read your operation from `data.pluginAction`, custom arguments from `data.pluginData`, and the selected app from top-level `appId`. App IDs are strings. Validate all input before performing work. The command service limits the full request to 64 KiB. Return your own object, for example `{"ok":true,"kind":"myPluginState","state":{"message":"Hello"}}` or `{"ok":false,"error":{"message":"Unknown action"}}`. The bridge passes that object to `onResult` as `response`; the second argument is the host envelope, whose `replyTo` can correlate concurrent requests. One callback is registered per plugin ID per context; registering again replaces it.

| UI bridge member | Behavior |
| --- | --- |
| `version` | Currently `1`; feature-detect required functions as well. |
| `send(id, action, data = {})` | Dispatch a native plugin command. The app ID defaults to the selected Library app, then the current app. |
| `onResult(id, callback)` | Receive valid plugin result objects with their host envelopes. Transport failures are not guaranteed to reach this callback; use a timeout for pending UI. |
| `appId()` / `libraryAppId()` | Get the current app / selected Library app; either may be empty. |
| `documents()` | Get distinct current and Steam desktop documents available in this context. |
| `setGameSetting(id, appId, spec)` | Add or update a plugin item in the shared Game Settings menus. Pass `null` to remove your contribution for that app and context. |
| `showConfirm(options, onConfirm)` | Use the shared Library confirmation renderer. The callback receives the modal; return `false` to keep it open for progress. |
| `setModalState(modal, state)` | Render `busy`, `success`, or an error phase with `title`, `message`, and optional `onClose`. |
| `refreshLibrary(appId)` | Request refreshed Library state. |

A game-setting spec accepts `label`, optional trusted `icon` markup, optional boolean `checked`, optional `disabled`, and `onActivate({state, button, document})`. The host renders both standard and sticky menus, closes the menu before activation, and uses checkbox semantics when `checked` is boolean. Supply plain text labels and avoid interpolating untrusted data into icon markup.

Confirmation options accept `title`, `message` (or `renderMessage(element)`), `confirmLabel`, `cancelLabel`, and optional `kind`. Use these bridge helpers to share menu styling and modal behavior. Remove stale app contributions when selection changes. UI source can run in multiple contexts, so a global marker only prevents duplicates within its own context. Check for an empty app ID and detached elements before updating UI.

Avoid depending on private `__hubcap*` globals, Steam's generated CSS selectors, or assumptions about a single document. RenoDX and OptiScaler contain additional badge/DOM integration specific to their features; those selectors are not part of the native ABI or a stable plugin API. A native plugin runs inside Steam's process with the user's privileges, and its UI source runs in an injected context. These are trusted extensions, not a sandbox.

## Install and verify in Steam

1. Fully exit Steam and verify it is stopped before copying a DLL. Use your actual Steam installation path.
2. Copy `hello-hubcap.dll` (or your own DLL) into `Steam/hubcapui-plugins`.
3. Start Steam, open Hubcap Settings, enable the plugin system and your plugin, and **Save**.
4. Restart Steam when prompted. Plugin activation uses the settings captured for that process; saving does not hot-load or unload native DLLs.
5. Select a game in Library and exercise the example's Game Settings action. Verify its response, selection changes, and the standard and sticky menus on the Steam build you intend to support.

Repeat the stop/copy/start sequence for each rebuilt DLL. To uninstall, disable and save, exit Steam fully, remove the DLL, then restart. The repository's automated template checks do not install plugins or restart Steam.

If a plugin does not appear, check the folder, embedded resource, manifest JSON, required fields, and duplicate IDs. If Settings shows a load error, check the x64 target, initializer export, ABI/struct sizes, manifest/runtime ID match, and missing DLL dependencies. If native commands work but the UI does not, confirm `HubcapPluginHost` is present, inspect script errors, and check the selected app and current document.

## Publishing and compatibility

Distribute a Windows x64 DLL built against the shared v1 SDK. Keep the manifest and runtime metadata synchronized, include licensing for dependencies, and document the HubcapUI and Steam versions you actually tested. Prefer a self-contained DLL or explicitly document required runtime dependencies; the loader searches the plugin DLL directory and Windows System32 for dependencies.

For automatic updates, follow [the plugin update standard](plugin-updates.md) and declare your own public GitHub repository and exact raw-DLL asset name. The starter omits update metadata and uses manual installation. Host updater support is a separate capability; ABI v1 compatibility alone does not imply automatic updates are available. Never point a forked plugin at this repository's updater unless its release assets and identity are intentionally maintained here.

There is no claimed minimum public host release in this guide: the integration requirements above must be present in the installed host. The native harness verifies the bundled starter against the ABI and protocol it tests. Live Steam verification remains necessary for UI rendering and for compatibility with future Steam or HubcapUI changes.
