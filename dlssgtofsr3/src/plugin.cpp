// HubcapUI ABI v1 plugin: installs Nukem's dlssg-to-fsr3 (Nexus Mods "site" mod 738) into a
// selected Steam game. Picks the "DLSSTweaks Edition" Nexus file when DLSSTweaks is detected
// for that game, or the "Universal" build otherwise (the user's own requirement: Universal
// and DLSSTweaks-Edition are not interchangeable, since DLSSTweaks chainloads the latter
// itself instead of dlssg-to-fsr3 hooking nvngx directly).
//
// NOTE: the exact file layout inside each Nexus ZIP (whether a rename or registry step is
// still required beyond extraction) is documented in that ZIP's own READ ME.txt, which this
// plugin cannot read ahead of a real download. Verify the first real install against that
// README before relying on this for unattended installs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstring>
#include <string>
#include <vector>

#include "hubcap_plugin.h"
#include "yyjson.h"
#include "dlss_common.h"

namespace {
using namespace dlsscommon;

const HubcapPluginHostV1* g_host = nullptr;
constexpr const char* kPluginId = "hubcap.dlssgtofsr3";
constexpr const char* kNexusModId = "738";
constexpr const wchar_t* kMarkerName = L"hubcap-dlssgtofsr3-install.json";
constexpr const wchar_t* kDlssTweaksMarkerName = L"hubcap-dlsstweaks-install.json";
constexpr const char* kVersion = "0.1.0";

const char* copy_result(const std::string& json)
{
    const auto size = json.size() + 1;
    auto* result = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, size));
    if (result) std::memcpy(result, json.c_str(), size);
    return result;
}

void log(const char* message)
{
    if (g_host && g_host->log) g_host->log(kPluginId, message);
}

struct AppContext {
    std::string app_id;
    std::wstring steam_root;
    std::wstring local_app_data;
    std::wstring game_dir;
    std::string game_name;
    bool resolved = false;
};

bool resolve_app(const std::string& app_id, AppContext& context, std::string& error)
{
    context.app_id = app_id;
    context.steam_root = g_host && g_host->steam_root ? g_host->steam_root() : L"";
    context.local_app_data = g_host && g_host->local_app_data ? g_host->local_app_data() : L"";
    if (!numeric_app_id(app_id)) { error = "No game is selected."; return false; }
    if (context.steam_root.empty()) { error = "Could not determine the Steam install path."; return false; }
    context.game_name = installed_game_name(context.steam_root, app_id);
    if (!resolve_steam_game_dir(context.steam_root, app_id, context.game_dir)) {
        error = "Install this game through Steam before installing dlssg-to-fsr3.";
        return false;
    }
    context.resolved = true;
    return true;
}

// Detect DLSSTweaks for this game: prefer our own sibling plugin's install manifest (it
// knows exactly what it placed), and fall back to a presence check for a manual install.
bool dlsstweaks_detected(const std::wstring& game_dir)
{
    std::vector<std::wstring> manifest_files;
    std::string manifest_version;
    if (read_install_manifest(game_dir, kDlssTweaksMarkerName, manifest_files, manifest_version)) return true;
    std::wstring found;
    return find_file_in_tree(game_dir, L"DLSSTweaksConfig.exe", found) || find_file_in_tree(game_dir, L"dlsstweaks.ini", found);
}

std::string state_json(const AppContext& context)
{
    std::vector<std::wstring> manifest_files;
    std::string manifest_version;
    const bool installed_by_us = context.resolved &&
        read_install_manifest(context.game_dir, kMarkerName, manifest_files, manifest_version);
    const bool has_dlsstweaks = context.resolved && dlsstweaks_detected(context.game_dir);
    const std::string api_key = read_nexus_api_key(context.local_app_data);
    return "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"gameResolved\":" + (context.resolved ? "true" : "false") +
        ",\"installed\":" + (installed_by_us ? "true" : "false") +
        ",\"installedVersion\":" + json_escape(manifest_version) +
        ",\"dlssTweaksDetected\":" + (has_dlsstweaks ? "true" : "false") +
        ",\"variant\":" + json_escape(has_dlsstweaks ? "DLSSTweaks Edition" : "Universal") +
        ",\"hasNexusApiKey\":" + (!api_key.empty() ? "true" : "false") + "}";
}

std::string handle_state(const std::string& app_id)
{
    AppContext context;
    std::string error;
    resolve_app(app_id, context, error);
    return reply_ok("dlssgToFsr3State", state_json(context));
}

std::string handle_install(const std::string& app_id)
{
    AppContext context;
    std::string error;
    if (!resolve_app(app_id, context, error)) return reply_error("dlssgtofsr3.unresolved", error);

    std::vector<std::wstring> existing_manifest;
    std::string existing_version;
    read_install_manifest(context.game_dir, kMarkerName, existing_manifest, existing_version);

    const std::string api_key = read_nexus_api_key(context.local_app_data);
    if (api_key.empty()) return reply_error("dlssgtofsr3.noApiKey", "Add a Nexus API key in Hubcap Advanced Settings to install dlssg-to-fsr3.");

    const bool want_dlsstweaks_edition = dlsstweaks_detected(context.game_dir);
    const std::string variant_filter = want_dlsstweaks_edition ? "DLSSTweaks Edition" : "Universal";

    std::vector<NexusFile> files;
    if (!nexus_list_files(api_key, kNexusModId, files, error)) return reply_error("dlssgtofsr3.nexusList", error);
    NexusFile selected;
    if (!nexus_select_file(files, variant_filter, selected)) {
        return reply_error("dlssgtofsr3.noFile", "Nexus did not return a downloadable \"" + variant_filter + "\" build of dlssg-to-fsr3.");
    }

    std::string download_url;
    if (!nexus_download_link(api_key, kNexusModId, selected.file_id, download_url, error)) return reply_error("dlssgtofsr3.nexusLink", error);

    const HttpResult response = http_get_url(download_url, 64 * 1024 * 1024, 120000, L"User-Agent: HubcapUI-DLSS-Plugins\r\n");
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        return reply_error("dlssgtofsr3.download", response.error.empty() ? http_error(response.status) : response.error);
    }

    // Remove a previous install of the *other* variant first: Universal and DLSSTweaks
    // Edition are not meant to coexist.
    if (!existing_version.empty()) remove_installed_files(context.game_dir, kMarkerName);

    std::vector<std::wstring> written;
    if (!extract_zip_to_directory(response.body, context.game_dir, written, error)) {
        return reply_error("dlssgtofsr3.extract", error);
    }
    if (written.empty()) return reply_error("dlssgtofsr3.extract", "The dlssg-to-fsr3 download did not contain any files.");

    if (!write_install_manifest(context.game_dir, kMarkerName, written, selected.name.empty() ? selected.file_name : selected.name)) {
        log("Could not write the dlssg-to-fsr3 install manifest; 'remove' may not find every file later.");
    }

    return reply_ok("dlssgToFsr3Installed", "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"variant\":" + json_escape(variant_filter) +
        ",\"statusText\":\"dlssg-to-fsr3 (" + variant_filter + ") was installed. Check the extracted READ ME.txt for any remaining manual step.\"}");
}

std::string handle_remove(const std::string& app_id)
{
    AppContext context;
    std::string error;
    if (!resolve_app(app_id, context, error)) return reply_error("dlssgtofsr3.unresolved", error);
    if (!remove_installed_files(context.game_dir, kMarkerName)) {
        return reply_error("dlssgtofsr3.removeFailed", "dlssg-to-fsr3 was not installed by this plugin, or some files could not be removed.");
    }
    return reply_ok("dlssgToFsr3Removed", "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"statusText\":\"dlssg-to-fsr3 was removed.\"}");
}

const char* HUBCAP_PLUGIN_CALL handle(const char* request_json)
{
    yyjson_doc* document = request_json ? yyjson_read(request_json, std::strlen(request_json), 0) : nullptr;
    if (!document) return copy_result(reply_error("request.invalid", "Invalid plugin request."));
    yyjson_val* root = yyjson_doc_get_root(document);
    yyjson_val* data = yyjson_obj_get(root, "data");
    const char* action = yyjson_get_str(yyjson_obj_get(data, "pluginAction"));
    const char* app_id_value = yyjson_get_str(yyjson_obj_get(root, "appId"));
    const std::string app_id = app_id_value ? app_id_value : "";
    std::string result;
    if (!action) {
        result = reply_error("request.invalid", "Missing plugin action.");
    } else if (std::strcmp(action, "state") == 0) {
        result = handle_state(app_id);
    } else if (std::strcmp(action, "install") == 0) {
        result = handle_install(app_id);
    } else if (std::strcmp(action, "remove") == 0) {
        result = handle_remove(app_id);
    } else {
        result = reply_error("request.unknown", "Unknown plugin action.");
    }
    yyjson_doc_free(document);
    return copy_result(result);
}

void HUBCAP_PLUGIN_CALL free_string(const char* value)
{
    if (value) HeapFree(GetProcessHeap(), 0, const_cast<char*>(value));
}

void HUBCAP_PLUGIN_CALL shutdown() { g_host = nullptr; }

const char k_ui[] = R"HUBCAPPLUGIN((() => {
    const host = globalThis.HubcapPluginHost;
    const ID = 'hubcap.dlssgtofsr3';
    if (!host || host.version !== 1 || globalThis.__hubcapDlssgToFsr3Installed) return;
    const required = ['send', 'onResult', 'libraryAppId', 'setGameSetting', 'showConfirm', 'setModalState'];
    if (!required.every(key => typeof host[key] === 'function')) return;
    globalThis.__hubcapDlssgToFsr3Installed = true;
    let currentApp = '', pending = null, lastState = null;

    const command = (action) => host.send(ID, action, { appId: currentApp });

    host.onResult(ID, (response, envelope) => {
        if (response && response.kind === 'dlssgToFsr3State') {
            lastState = response.state;
            return;
        }
        if (!pending || envelope?.replyTo !== pending.id) return;
        const request = pending;
        pending = null;
        clearTimeout(request.timeout);
        host.setModalState(request.modal, {
            phase: response?.ok ? 'success' : 'error',
            title: response?.ok ? 'dlssg-to-fsr3' : 'dlssg-to-fsr3 request failed',
            message: response?.state?.statusText || response?.error?.message || 'No message returned.',
            onClose: () => command('state')
        });
    });

    const send = (action, modal) => {
        const request = { modal, timeout: 0, id: '' };
        pending = request;
        request.timeout = setTimeout(() => {
            if (pending !== request) return;
            pending = null;
            host.setModalState(modal, { phase: 'error', title: 'dlssg-to-fsr3 request timed out', message: 'Close this dialog and try again.' });
        }, 60000);
        request.id = host.send(ID, action, { appId: currentApp });
    };

    const tick = () => {
        const app = String(host.libraryAppId() || '');
        if (app === currentApp) { if (app) command('state'); return; }
        if (currentApp) host.setGameSetting(ID, currentApp, null);
        currentApp = app;
        if (!app) return;
        command('state');
        const installed = !!lastState?.installed;
        const variant = lastState?.variant || 'Universal';
        host.setGameSetting(ID, app, {
            label: installed ? 'Remove dlssg-to-fsr3' : `Enable dlssg-to-fsr3 (${variant})`,
            checked: installed,
            onActivate: () => {
                if (pending) return;
                const remove = installed;
                host.showConfirm({
                    kind: remove ? 'dlssgtofsr3-remove' : 'dlssgtofsr3-install',
                    title: remove ? 'Remove dlssg-to-fsr3?' : 'Enable dlssg-to-fsr3?',
                    message: remove
                        ? 'Remove the dlssg-to-fsr3 files this plugin installed for this game?'
                        : `Download and install dlssg-to-fsr3 (Nukem) for this game from Nexus Mods, using the ${variant} build? Requires a Nexus API key in Hubcap Advanced Settings.`,
                    confirmLabel: remove ? 'Remove' : 'Install',
                    cancelLabel: 'Cancel'
                }, modal => {
                    host.setModalState(modal, { phase: 'busy', title: 'dlssg-to-fsr3', message: remove ? 'Removing dlssg-to-fsr3...' : `Downloading dlssg-to-fsr3 (${variant}) from Nexus Mods...` });
                    send(remove ? 'remove' : 'install', modal);
                    return false;
                });
            }
        });
    };
    tick();
    setInterval(tick, 1000);
})())HUBCAPPLUGIN";

} // namespace

extern "C" __declspec(dllexport) bool HUBCAP_PLUGIN_CALL HubcapPluginInitializeV1(
    const HubcapPluginHostV1* host, HubcapPluginV1* plugin)
{
    if (!host || host->size < sizeof(*host) || host->abi_version != HUBCAP_PLUGIN_ABI_VERSION ||
        !plugin || plugin->size < sizeof(*plugin)) return false;
    g_host = host;
    plugin->size = sizeof(*plugin);
    plugin->abi_version = HUBCAP_PLUGIN_ABI_VERSION;
    plugin->id = kPluginId;
    plugin->name = "dlssg-to-fsr3";
    plugin->version = kVersion;
    plugin->description = "Installs Nukem's dlssg-to-fsr3 into a selected Steam game via Nexus Mods, picking the DLSSTweaks Edition build automatically when DLSSTweaks is present.";
    plugin->ui_script = k_ui;
    plugin->handle = handle;
    plugin->free_string = free_string;
    plugin->shutdown = shutdown;
    if (host->log) host->log(plugin->id, "dlssg-to-fsr3 plugin initialized.");
    return true;
}
