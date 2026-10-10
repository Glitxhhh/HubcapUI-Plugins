#include <windows.h>
#include <cstring>

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
    const char* action = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(root, "data"), "pluginAction"));
    const bool supported = action && (std::strcmp(action, "hello") == 0 || std::strcmp(action, "state") == 0);
    yyjson_doc_free(document);
    if (!supported) return copy_result("{\"ok\":false,\"error\":{\"message\":\"Unknown plugin action.\"}}");
    return copy_result("{\"ok\":true,\"kind\":\"hello\",\"state\":{\"message\":\"Hello from a native HubcapUI plugin.\"}}");
}

void HUBCAP_PLUGIN_CALL free_string(const char* value)
{
    if (value) HeapFree(GetProcessHeap(), 0, const_cast<char*>(value));
}

void HUBCAP_PLUGIN_CALL shutdown() { g_host = nullptr; }

// Hubcap injects this into its Steam UI targets. Guard each JS context against reinjection.
const char k_ui[] = R"HUBCAPPLUGIN((() => {
    const host = globalThis.HubcapPluginHost;
    const ID = 'example.hello-hubcap';
    if (!host || host.version !== 1 || globalThis.__helloHubcapInstalled) return;
    const required = ['send', 'onResult', 'libraryAppId', 'setGameSetting', 'showConfirm', 'setModalState'];
    if (!required.every(key => typeof host[key] === 'function')) return;
    globalThis.__helloHubcapInstalled = true;
    let currentApp = '', pending = null;

    host.onResult(ID, (response, envelope) => {
        // Correlate the reply: background 'state' refreshes must not finish a user's dialog.
        if (!pending || envelope?.replyTo !== pending.id) return;
        const request = pending;
        pending = null;
        clearTimeout(request.timeout);
        host.setModalState(request.modal, {
            phase: response?.ok ? 'success' : 'error',
            title: response?.ok ? 'Hello Hubcap' : 'Plugin request failed',
            message: response?.state?.message || response?.error?.message || 'No message returned.'
        });
    });

    const tick = () => {
        const app = String(host.libraryAppId() || '');
        if (app === currentApp) return;
        if (currentApp) host.setGameSetting(ID, currentApp, null);
        currentApp = app;
        if (!app) return;
        host.setGameSetting(ID, app, {
            label: 'Hello Hubcap', checked: false,
            onActivate: () => {
                if (pending) return;
                host.showConfirm({
                    title: 'Hello Hubcap', message: 'Send a request to the example native plugin?',
                    confirmLabel: 'Say hello', cancelLabel: 'Cancel'
                }, modal => {
                    host.setModalState(modal, { phase: 'busy', title: 'Hello Hubcap', message: 'Waiting for the plugin...' });
                    const request = { modal, timeout: 0, id: '' };
                    pending = request;
                    request.timeout = setTimeout(() => {
                        if (pending !== request) return;
                        pending = null;
                        host.setModalState(modal, { phase: 'error', title: 'Plugin request timed out', message: 'Close this dialog and try again.' });
                    }, 10000);
                    request.id = host.send(ID, 'hello', { appId: app });
                    return false; // Keep the host's shared dialog open for the async response.
                });
            }
        });
    };
    tick();
    setInterval(tick, 500);
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
    plugin->id = "example.hello-hubcap";
    plugin->name = "Hello Hubcap";
    plugin->version = "0.1.0";
    plugin->description = "A minimal native plugin and Steam Library UI example.";
    plugin->ui_script = k_ui;
    plugin->handle = handle;
    plugin->free_string = free_string;
    plugin->shutdown = shutdown;
    if (host->log) host->log(plugin->id, "Hello Hubcap initialized.");
    return true;
}
