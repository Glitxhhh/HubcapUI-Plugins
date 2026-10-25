#include <windows.h>
#include <cstring>
#include <string>

#include "hubcap_plugin.h"
#include "yyjson.h"

namespace {
const HubcapPluginHostV1* g_host = nullptr;

// Allocate and release responses in this DLL; never cross C++ allocator boundaries.
const char* copy_result(const char* json)
{
    const auto size = std::strlen(json) + 1;
    auto* result = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, size));
    if (result) std::memcpy(result, json, size);
    return result;
}

const char* HUBCAP_PLUGIN_CALL handle(const char* request_json)
{
    yyjson_doc* document = request_json ? yyjson_read(request_json, std::strlen(request_json), 0) : nullptr;
    if (!document) return copy_result("{\"ok\":false,\"error\":{\"message\":\"Invalid plugin request.\"}}");
    yyjson_val* root = yyjson_doc_get_root(document);
    yyjson_val* data = yyjson_obj_get(root, "data");
    const char* action = yyjson_get_str(yyjson_obj_get(data, "pluginAction"));
    const char* app_id = yyjson_get_str(yyjson_obj_get(root, "appId"));
    const char* source_value = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(data, "pluginData"), "source"));
    const bool supported = action && std::strcmp(action, "hello") == 0;
    const std::string app = app_id ? app_id : "";
    const std::string source = source_value ? source_value : "unknown surface";
    yyjson_doc_free(document);
    if (!supported) return copy_result("{\"ok\":false,\"error\":{\"message\":\"Unknown plugin action.\"}}");

    // Exercises host->send_notification — a real Windows notification, native-only
    // (there is no JS-side equivalent; it can't be called from a plugin's ui_script
    // directly, only from native handle() code like this). Size-gate it: an older
    // host build won't have this field at all.
    if (g_host && g_host->size >= sizeof(*g_host) && g_host->send_notification) {
        const std::string body = "Triggered from: " + source + (app.empty() ? "" : (", app " + app)) + ".";
        g_host->send_notification("Hello Test", body.c_str());
    }

    const std::string message = "Hello from Hello Test's native plugin (via " + source + ").";
    return copy_result(("{\"ok\":true,\"kind\":\"hello\",\"state\":{\"message\":\"" + message + "\"}}").c_str());
}

void HUBCAP_PLUGIN_CALL free_string(const char* value)
{
    if (value) HeapFree(GetProcessHeap(), 0, const_cast<char*>(value));
}

void HUBCAP_PLUGIN_CALL shutdown() { g_host = nullptr; }

// Hello Test exercises every UI bridge surface HubcapUI plugins can use, in one
// place — the plugin to come back to and extend whenever the SDK grows further, to
// confirm nothing regressed:
//   - setPluginSettingsEntry: a global entry on this plugin's own row in Hubcap
//     Settings (see docs/plugin-development.md's "Choosing where a UI contribution
//     goes" section).
//   - setManageMenuEntry: a per-game entry in the Library grid's native right-click
//     Manage submenu.
//   - host->send_notification: a real Windows notification, fired natively on every
//     successful "hello" round trip from either entry above.
// Deliberately does NOT demonstrate setGameSetting — see examples/hello-hubcap for
// that, which remains the canonical starting-point template for new plugins.
const char k_ui[] = R"HUBCAPPLUGIN((() => {
    const host = globalThis.HubcapPluginHost;
    const ID = 'example.hello-test';
    if (!host || host.version !== 1 || globalThis.__helloTestInstalled) return;
    const required = ['send', 'onResult', 'showConfirm', 'setModalState'];
    if (!required.every(key => typeof host[key] === 'function')) return;
    globalThis.__helloTestInstalled = true;
    let pending = null;

    host.onResult(ID, (response, envelope) => {
        if (!pending || envelope?.replyTo !== pending.id) return;
        const request = pending;
        pending = null;
        clearTimeout(request.timeout);
        host.setModalState(request.modal, {
            phase: response?.ok ? 'success' : 'error',
            title: response?.ok ? 'Hello Test' : 'Plugin request failed',
            message: response?.state?.message || response?.error?.message || 'No message returned.'
        });
    });

    const openDialog = (source, appId) => {
        if (pending) return;
        host.showConfirm({
            title: 'Hello Test',
            message: `Send a request to the example native plugin (via ${source}${appId ? `, app ${appId}` : ''})? This also fires a real Windows notification.`,
            confirmLabel: 'Say hello', cancelLabel: 'Cancel'
        }, modal => {
            host.setModalState(modal, { phase: 'busy', title: 'Hello Test', message: 'Waiting for the plugin...' });
            const request = { modal, timeout: 0, id: '' };
            pending = request;
            request.timeout = setTimeout(() => {
                if (pending !== request) return;
                pending = null;
                host.setModalState(modal, { phase: 'error', title: 'Plugin request timed out', message: 'Close this dialog and try again.' });
            }, 10000);
            request.id = host.send(ID, 'hello', { appId, source });
            return false; // Keep the host's shared dialog open for the async response.
        });
    };

    // Both entries are global/per-right-click, not per-selected-game, so both are
    // registered once here — neither needs a libraryAppId()-driven tick() loop the
    // way a setGameSetting toggle would. Feature-detect each bridge method
    // individually: both were added after the original bridge, so either (or both)
    // may be missing on an older host. Fail quietly per-feature, not all-or-nothing.
    if (typeof host.setPluginSettingsEntry === 'function') {
        host.setPluginSettingsEntry(ID, { label: 'Say Hello (Settings)', onActivate: () => openDialog('Plugin Settings') });
    }
    if (typeof host.setManageMenuEntry === 'function') {
        host.setManageMenuEntry(ID, { label: 'Say Hello (Manage)', onActivate: ({ appId }) => openDialog('Manage submenu', appId) });
    }
})())HUBCAPPLUGIN";
}

extern "C" __declspec(dllexport) bool HUBCAP_PLUGIN_CALL HubcapPluginInitializeV1(
    const HubcapPluginHostV1* host, HubcapPluginV1* plugin)
{
    if (!host || host->size < sizeof(*host) || host->abi_version != HUBCAP_PLUGIN_ABI_VERSION ||
        !plugin || plugin->size < sizeof(*plugin)) return false;
    g_host = host;
    plugin->size = sizeof(*plugin);
    plugin->abi_version = HUBCAP_PLUGIN_ABI_VERSION;
    plugin->id = "example.hello-test";
    plugin->name = "Hello Test";
    plugin->version = "0.1.0";
    plugin->description = "Exercises setPluginSettingsEntry, setManageMenuEntry, and host->send_notification in one plugin.";
    plugin->ui_script = k_ui;
    plugin->handle = handle;
    plugin->free_string = free_string;
    plugin->shutdown = shutdown;
    if (host->log) host->log(plugin->id, "Hello Test initialized.");
    return true;
}
