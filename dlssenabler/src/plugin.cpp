// HubcapUI ABI v1 plugin: installs artur07305's DLSS Enabler (Nexus Mods "site" mod 757)
// into a selected Steam game. DLSS Enabler ships as a single proxy DLL that must be renamed
// to one of a handful of well-known Windows DLL names next to the game's executable
// (dxgi.dll is the common choice, with version.dll/winmm.dll/dbghelp.dll/psapi.dll/
// winhttp.dll as fallbacks per the mod's own instructions). This plugin downloads the
// Nexus file (which may be the raw DLL or a ZIP containing it alongside a README),
// locates the actual DLL, and places it under the first proxy name that isn't already
// used by something else in that game's folder.
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
constexpr const char* kPluginId = "hubcap.dlssenabler";
constexpr const char* kNexusModId = "757";
constexpr const wchar_t* kMarkerName = L"hubcap-dlssenabler-install.json";
constexpr const char* kVersion = "0.1.0";
// Order matches the mod's own documented fallback order.
const wchar_t* const kProxyNames[] = {L"dxgi.dll", L"version.dll", L"winmm.dll", L"dbghelp.dll", L"psapi.dll", L"winhttp.dll"};

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

bool looks_like_pe_dll(const std::string& bytes)
{
    return bytes.size() >= 2 && bytes[0] == 'M' && bytes[1] == 'Z';
}

// Finds the DLSS Enabler DLL inside a downloaded ZIP: the largest .dll entry whose name
// does not look like a dependency (nvapi/nvngx shims the mod ships alongside it).
bool find_enabler_dll_in_zip(const std::string& zip_bytes, std::string& dll_bytes, std::string& error)
{
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zip_bytes.data(), zip_bytes.size(), 0)) {
        error = "The DLSS Enabler download was neither a DLL nor a valid ZIP archive.";
        return false;
    }
    mz_uint best_index = static_cast<mz_uint>(-1);
    mz_uint64 best_size = 0;
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint index = 0; index < count; ++index) {
        if (mz_zip_reader_is_file_a_directory(&archive, index)) continue;
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) continue;
        const std::string entry = stat.m_filename;
        if (entry.size() < 4 || _stricmp(entry.c_str() + entry.size() - 4, ".dll") != 0) continue;
        const std::string lower = lower_narrow(entry);
        if (lower.find("nvapi") != std::string::npos || lower.find("nvngx") != std::string::npos) continue;
        if (static_cast<mz_uint64>(stat.m_uncomp_size) > best_size) {
            best_size = stat.m_uncomp_size;
            best_index = index;
        }
    }
    bool ok = false;
    if (best_index != static_cast<mz_uint>(-1)) {
        size_t size = 0;
        void* extracted = mz_zip_reader_extract_to_heap(&archive, best_index, &size, 0);
        if (extracted) {
            dll_bytes.assign(static_cast<char*>(extracted), static_cast<char*>(extracted) + size);
            mz_free(extracted);
            ok = true;
        }
    }
    mz_zip_reader_end(&archive);
    if (!ok) error = "The DLSS Enabler ZIP did not contain a recognizable DLL.";
    return ok;
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
        error = "Install this game through Steam before installing DLSS Enabler.";
        return false;
    }
    context.resolved = true;
    return true;
}

// Picks the first proxy filename not already present in the game folder. If this plugin
// already installed one (per its manifest), reuse that same name on a re-install/update.
bool pick_proxy_name(const std::wstring& game_dir, const std::vector<std::wstring>& existing_manifest,
    std::wstring& chosen, std::string& error)
{
    if (!existing_manifest.empty()) { chosen = existing_manifest.front(); return true; }
    for (const wchar_t* name : kProxyNames) {
        const std::wstring candidate = join(game_dir, name);
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) { chosen = candidate; return true; }
    }
    error = "Every known proxy DLL name (dxgi.dll, version.dll, winmm.dll, dbghelp.dll, psapi.dll, winhttp.dll) "
        "already exists in this game's folder. Remove one of those (or whatever already provides DLSS) before installing DLSS Enabler.";
    return false;
}

std::string state_json(const AppContext& context)
{
    std::vector<std::wstring> manifest_files;
    std::string manifest_version;
    const bool installed_by_us = context.resolved &&
        read_install_manifest(context.game_dir, kMarkerName, manifest_files, manifest_version);
    std::string proxy_name;
    if (installed_by_us && !manifest_files.empty()) {
        const std::wstring& path = manifest_files.front();
        const size_t separator = path.find_last_of(L"\\/");
        proxy_name = narrow(separator == std::wstring::npos ? path : path.substr(separator + 1));
    }
    const std::string api_key = read_nexus_api_key(context.local_app_data);
    return "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"gameResolved\":" + (context.resolved ? "true" : "false") +
        ",\"installed\":" + (installed_by_us ? "true" : "false") +
        ",\"installedVersion\":" + json_escape(manifest_version) +
        ",\"proxyName\":" + json_escape(proxy_name) +
        ",\"hasNexusApiKey\":" + (!api_key.empty() ? "true" : "false") + "}";
}

std::string handle_state(const std::string& app_id)
{
    AppContext context;
    std::string error;
    resolve_app(app_id, context, error);
    return reply_ok("dlssEnablerState", state_json(context));
}

std::string handle_install(const std::string& app_id)
{
    AppContext context;
    std::string error;
    if (!resolve_app(app_id, context, error)) return reply_error("dlssenabler.unresolved", error);

    std::vector<std::wstring> existing_manifest;
    std::string existing_version;
    read_install_manifest(context.game_dir, kMarkerName, existing_manifest, existing_version);

    std::wstring proxy_path;
    if (!pick_proxy_name(context.game_dir, existing_manifest, proxy_path, error)) {
        return reply_error("dlssenabler.proxyConflict", error);
    }

    const std::string api_key = read_nexus_api_key(context.local_app_data);
    if (api_key.empty()) return reply_error("dlssenabler.noApiKey", "Add a Nexus API key in Hubcap Advanced Settings to install DLSS Enabler.");

    std::vector<NexusFile> files;
    if (!nexus_list_files(api_key, kNexusModId, files, error)) return reply_error("dlssenabler.nexusList", error);
    NexusFile selected;
    if (!nexus_select_file(files, "", selected)) return reply_error("dlssenabler.noFile", "Nexus did not return a downloadable DLSS Enabler file.");

    std::string download_url;
    if (!nexus_download_link(api_key, kNexusModId, selected.file_id, download_url, error)) return reply_error("dlssenabler.nexusLink", error);

    const HttpResult response = http_get_url(download_url, 64 * 1024 * 1024, 120000, L"User-Agent: HubcapUI-DLSS-Plugins\r\n");
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        return reply_error("dlssenabler.download", response.error.empty() ? http_error(response.status) : response.error);
    }

    std::string dll_bytes;
    if (looks_like_pe_dll(response.body)) {
        dll_bytes = response.body;
    } else if (!find_enabler_dll_in_zip(response.body, dll_bytes, error)) {
        return reply_error("dlssenabler.extract", error);
    }
    if (!looks_like_pe_dll(dll_bytes)) return reply_error("dlssenabler.extract", "The DLSS Enabler file was not a valid DLL.");

    if (!write_file_atomic(proxy_path, dll_bytes.data(), dll_bytes.size())) {
        return reply_error("dlssenabler.write", "Could not write the DLSS Enabler DLL into the game folder.");
    }
    if (!write_install_manifest(context.game_dir, kMarkerName, {proxy_path}, selected.name.empty() ? selected.file_name : selected.name)) {
        log("Could not write the DLSS Enabler install manifest; 'remove' may not find the file later.");
    }

    const size_t separator = proxy_path.find_last_of(L"\\/");
    const std::string proxy_name = narrow(separator == std::wstring::npos ? proxy_path : proxy_path.substr(separator + 1));
    return reply_ok("dlssEnablerInstalled", "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"proxyName\":" + json_escape(proxy_name) +
        ",\"statusText\":\"DLSS Enabler was installed as " + proxy_name + ". Launch the game and press ` to open its sidebar.\"}");
}

std::string handle_remove(const std::string& app_id)
{
    AppContext context;
    std::string error;
    if (!resolve_app(app_id, context, error)) return reply_error("dlssenabler.unresolved", error);
    if (!remove_installed_files(context.game_dir, kMarkerName)) {
        return reply_error("dlssenabler.removeFailed", "DLSS Enabler was not installed by this plugin, or the file could not be removed.");
    }
    return reply_ok("dlssEnablerRemoved", "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"statusText\":\"DLSS Enabler was removed.\"}");
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
    const ID = 'hubcap.dlssenabler';
    if (!host || host.version !== 1 || globalThis.__hubcapDlssEnablerInstalled) return;
    const required = ['send', 'onResult', 'libraryAppId', 'setGameSetting', 'showConfirm', 'setModalState'];
    if (!required.every(key => typeof host[key] === 'function')) return;
    globalThis.__hubcapDlssEnablerInstalled = true;
    let currentApp = '', pending = null, lastState = null;

    const command = (action) => host.send(ID, action, { appId: currentApp });

    host.onResult(ID, (response, envelope) => {
        if (response && response.kind === 'dlssEnablerState') {
            lastState = response.state;
            return;
        }
        if (!pending || envelope?.replyTo !== pending.id) return;
        const request = pending;
        pending = null;
        clearTimeout(request.timeout);
        host.setModalState(request.modal, {
            phase: response?.ok ? 'success' : 'error',
            title: response?.ok ? 'DLSS Enabler' : 'DLSS Enabler request failed',
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
            host.setModalState(modal, { phase: 'error', title: 'DLSS Enabler request timed out', message: 'Close this dialog and try again.' });
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
        host.setGameSetting(ID, app, {
            label: installed ? 'Remove DLSS Enabler' : 'Enable DLSS Enabler',
            checked: installed,
            onActivate: () => {
                if (pending) return;
                const remove = installed;
                host.showConfirm({
                    kind: remove ? 'dlssenabler-remove' : 'dlssenabler-install',
                    title: remove ? 'Remove DLSS Enabler?' : 'Enable DLSS Enabler?',
                    message: remove
                        ? 'Remove the DLSS Enabler file this plugin installed for this game?'
                        : 'Download and install DLSS Enabler (artur07305) for this game from Nexus Mods? Requires a Nexus API key in Hubcap Advanced Settings. Do not use in multiplayer games: anti-cheat may flag frame generation injection as cheating.',
                    confirmLabel: remove ? 'Remove' : 'Install',
                    cancelLabel: 'Cancel'
                }, modal => {
                    host.setModalState(modal, { phase: 'busy', title: 'DLSS Enabler', message: remove ? 'Removing DLSS Enabler...' : 'Downloading DLSS Enabler from Nexus Mods...' });
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
    plugin->name = "DLSS Enabler";
    plugin->version = kVersion;
    plugin->description = "Installs artur07305's DLSS Enabler into a selected Steam game via Nexus Mods.";
    plugin->ui_script = k_ui;
    plugin->handle = handle;
    plugin->free_string = free_string;
    plugin->shutdown = shutdown;
    if (host->log) host->log(plugin->id, "DLSS Enabler plugin initialized.");
    return true;
}
