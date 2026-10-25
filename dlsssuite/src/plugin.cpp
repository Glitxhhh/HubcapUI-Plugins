// HubcapUI ABI v1 plugin: DLSS Suite — merges what were three separate plugins
// (DLSSTweaks, dlssg-to-fsr3, DLSS Enabler) into one, since all three install from
// Nexus Mods and share the same Nexus API key. Merging eliminates the cross-plugin
// "which one owns the shared Nexus API key UI" coordination this repo fought with
// for a while (see dlssenabler's plugin.cpp history, now folded in here) — there's
// only one plugin now, so there's nothing to coordinate.
//
// Each of the three is still an independent per-game install/remove toggle; merging
// them doesn't change that, it just means one DLL, one settings-panel entry for the
// shared key, and one place dlssg-to-fsr3 looks to detect DLSSTweaks (still a
// same-game-folder file check, not IPC — merging didn't need to change that part).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <cstring>
#include <string>
#include <vector>

#include "hubcap_plugin.h"
#include "yyjson.h"
#include "dlss_common.h"

namespace {
using namespace dlsscommon;

const HubcapPluginHostV1* g_host = nullptr;
constexpr const char* kPluginId = "hubcap.dlsssuite";
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

// Shared by all three features — identical shape, only the "install this game"
// error message differs by feature, so resolve_app takes it as a parameter instead
// of being copy-pasted three times.
struct AppContext {
    std::string app_id;
    std::wstring steam_root;
    std::wstring local_app_data;
    std::wstring game_dir;
    std::string game_name;
    bool resolved = false;
};

bool resolve_app(const std::string& app_id, AppContext& context, std::string& error, const char* feature_label)
{
    context.app_id = app_id;
    context.steam_root = g_host && g_host->steam_root ? g_host->steam_root() : L"";
    context.local_app_data = g_host && g_host->local_app_data ? g_host->local_app_data() : L"";
    if (!numeric_app_id(app_id)) { error = "No game is selected."; return false; }
    if (context.steam_root.empty()) { error = "Could not determine the Steam install path."; return false; }
    context.game_name = installed_game_name(context.steam_root, app_id);
    if (!resolve_steam_game_dir(context.steam_root, app_id, context.game_dir)) {
        error = std::string("Install this game through Steam before installing ") + feature_label + ".";
        return false;
    }
    context.resolved = true;
    return true;
}

// ---------------------------------------------------------------------------
// DLSSTweaks (Nexus mod 550)
// ---------------------------------------------------------------------------
constexpr const char* kTwNexusModId = "550";
constexpr const wchar_t* kTwMarkerName = L"hubcap-dlsstweaks-install.json";
constexpr const wchar_t* kTwConfigToolName = L"DLSSTweaksConfig.exe";

std::string tw_state_json(const AppContext& context)
{
    std::vector<std::wstring> manifest_files;
    std::string manifest_version;
    const bool installed_by_us = context.resolved &&
        read_install_manifest(context.game_dir, kTwMarkerName, manifest_files, manifest_version);
    std::wstring config_tool;
    const bool config_tool_present = context.resolved &&
        find_file_in_tree(context.game_dir, kTwConfigToolName, config_tool);
    const std::string api_key = read_nexus_api_key(context.local_app_data);
    return "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"gameResolved\":" + (context.resolved ? "true" : "false") +
        ",\"installed\":" + (installed_by_us ? "true" : "false") +
        ",\"installedVersion\":" + json_escape(manifest_version) +
        ",\"configToolPresent\":" + (config_tool_present ? "true" : "false") +
        ",\"hasNexusApiKey\":" + (!api_key.empty() ? "true" : "false") + "}";
}

std::string tw_handle_state(const std::string& app_id)
{
    AppContext context;
    std::string error;
    resolve_app(app_id, context, error, "DLSSTweaks");
    return reply_ok("dlssTweaksState", tw_state_json(context));
}

std::string tw_handle_install(const std::string& app_id)
{
    AppContext context;
    std::string error;
    if (!resolve_app(app_id, context, error, "DLSSTweaks")) return reply_error("dlsstweaks.unresolved", error);

    std::vector<std::wstring> existing_manifest;
    std::string existing_version;
    const bool already_ours = read_install_manifest(context.game_dir, kTwMarkerName, existing_manifest, existing_version);
    std::wstring existing_dxgi = join(context.game_dir, L"dxgi.dll");
    if (!already_ours && GetFileAttributesW(existing_dxgi.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return reply_error("dlsstweaks.conflict",
            "This game already has a dxgi.dll that DLSSTweaks did not install (likely ReShade or another proxy DLL). "
            "Remove or rename it before installing DLSSTweaks.");
    }

    const std::string api_key = read_nexus_api_key(context.local_app_data);
    if (api_key.empty()) return reply_error("dlsstweaks.noApiKey", "Add a Nexus API key in DLSS Suite's Hubcap Settings entry to install DLSSTweaks.");

    std::vector<NexusFile> files;
    if (!nexus_list_files(api_key, kTwNexusModId, files, error)) return reply_error("dlsstweaks.nexusList", error);
    NexusFile selected;
    if (!nexus_select_file(files, "", selected)) return reply_error("dlsstweaks.noFile", "Nexus did not return a downloadable DLSSTweaks file.");

    std::string download_url;
    if (!nexus_download_link(api_key, kTwNexusModId, selected.file_id, download_url, error)) return reply_error("dlsstweaks.nexusLink", error);

    const HttpResult response = http_get_url(download_url, 32 * 1024 * 1024, 120000, L"User-Agent: HubcapUI-DLSS-Plugins\r\n");
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        return reply_error("dlsstweaks.download", response.error.empty() ? http_error(response.status) : response.error);
    }

    std::vector<std::wstring> written;
    if (!extract_zip_to_directory(response.body, context.game_dir, written, error)) {
        return reply_error("dlsstweaks.extract", error);
    }
    if (written.empty()) return reply_error("dlsstweaks.extract", "The DLSSTweaks download did not contain any files.");

    if (!write_install_manifest(context.game_dir, kTwMarkerName, written, selected.name.empty() ? selected.file_name : selected.name)) {
        log("Could not write the DLSSTweaks install manifest; 'remove' may not find every file later.");
    }

    std::wstring config_tool;
    if (find_file_in_tree(context.game_dir, kTwConfigToolName, config_tool)) {
        ShellExecuteW(nullptr, L"open", config_tool.c_str(), nullptr, context.game_dir.c_str(), SW_SHOWNORMAL);
    }

    return reply_ok("dlssTweaksInstalled", "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"statusText\":\"DLSSTweaks was installed. Finish setup in the DLSSTweaksConfig window.\"}");
}

std::string tw_handle_remove(const std::string& app_id)
{
    AppContext context;
    std::string error;
    if (!resolve_app(app_id, context, error, "DLSSTweaks")) return reply_error("dlsstweaks.unresolved", error);
    if (!remove_installed_files(context.game_dir, kTwMarkerName)) {
        return reply_error("dlsstweaks.removeFailed", "DLSSTweaks was not installed by this plugin, or some files could not be removed.");
    }
    return reply_ok("dlssTweaksRemoved", "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"statusText\":\"DLSSTweaks was removed.\"}");
}

// ---------------------------------------------------------------------------
// dlssg-to-fsr3 (Nexus mod 738) — picks the "DLSSTweaks Edition" Nexus file when
// DLSSTweaks is detected installed for the same game, "Universal" otherwise.
// ---------------------------------------------------------------------------
constexpr const char* kFsrNexusModId = "738";
constexpr const wchar_t* kFsrMarkerName = L"hubcap-dlssgtofsr3-install.json";

bool fsr_dlsstweaks_detected(const std::wstring& game_dir)
{
    std::vector<std::wstring> manifest_files;
    std::string manifest_version;
    if (read_install_manifest(game_dir, kTwMarkerName, manifest_files, manifest_version)) return true;
    std::wstring found;
    return find_file_in_tree(game_dir, L"DLSSTweaksConfig.exe", found) || find_file_in_tree(game_dir, L"dlsstweaks.ini", found);
}

std::string fsr_state_json(const AppContext& context)
{
    std::vector<std::wstring> manifest_files;
    std::string manifest_version;
    const bool installed_by_us = context.resolved &&
        read_install_manifest(context.game_dir, kFsrMarkerName, manifest_files, manifest_version);
    const bool has_dlsstweaks = context.resolved && fsr_dlsstweaks_detected(context.game_dir);
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

std::string fsr_handle_state(const std::string& app_id)
{
    AppContext context;
    std::string error;
    resolve_app(app_id, context, error, "dlssg-to-fsr3");
    return reply_ok("dlssgToFsr3State", fsr_state_json(context));
}

std::string fsr_handle_install(const std::string& app_id)
{
    AppContext context;
    std::string error;
    if (!resolve_app(app_id, context, error, "dlssg-to-fsr3")) return reply_error("dlssgtofsr3.unresolved", error);

    std::vector<std::wstring> existing_manifest;
    std::string existing_version;
    read_install_manifest(context.game_dir, kFsrMarkerName, existing_manifest, existing_version);

    const std::string api_key = read_nexus_api_key(context.local_app_data);
    if (api_key.empty()) return reply_error("dlssgtofsr3.noApiKey", "Add a Nexus API key in DLSS Suite's Hubcap Settings entry to install dlssg-to-fsr3.");

    const bool want_dlsstweaks_edition = fsr_dlsstweaks_detected(context.game_dir);
    const std::string variant_filter = want_dlsstweaks_edition ? "DLSSTweaks Edition" : "Universal";

    std::vector<NexusFile> files;
    if (!nexus_list_files(api_key, kFsrNexusModId, files, error)) return reply_error("dlssgtofsr3.nexusList", error);
    NexusFile selected;
    if (!nexus_select_file(files, variant_filter, selected)) {
        return reply_error("dlssgtofsr3.noFile", "Nexus did not return a downloadable \"" + variant_filter + "\" build of dlssg-to-fsr3.");
    }

    std::string download_url;
    if (!nexus_download_link(api_key, kFsrNexusModId, selected.file_id, download_url, error)) return reply_error("dlssgtofsr3.nexusLink", error);

    const HttpResult response = http_get_url(download_url, 64 * 1024 * 1024, 120000, L"User-Agent: HubcapUI-DLSS-Plugins\r\n");
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        return reply_error("dlssgtofsr3.download", response.error.empty() ? http_error(response.status) : response.error);
    }

    std::wstring preferred_proxy_name;
    for (const auto& file : existing_manifest) {
        const size_t separator = file.find_last_of(L"\\/");
        const std::wstring base = lower_wide(separator == std::wstring::npos ? file : file.substr(separator + 1));
        if (base == L"dbghelp.dll" || base == L"version.dll" || base == L"winhttp.dll") { preferred_proxy_name = base; break; }
    }

    if (!existing_version.empty()) remove_installed_files(context.game_dir, kFsrMarkerName);

    static const std::pair<const char*, const wchar_t*> kProxyFolders[] = {
        {"dll_dbghelp", L"dbghelp.dll"}, {"dll_version", L"version.dll"}, {"dll_winhttp", L"winhttp.dll"}};
    const std::vector<std::string> top_level_folders = zip_top_level_folders(response.body);
    std::string chosen_folder;
    for (const auto& candidate : kProxyFolders) {
        if (std::find(top_level_folders.begin(), top_level_folders.end(), candidate.first) == top_level_folders.end()) continue;
        const bool is_preferred = !preferred_proxy_name.empty() && preferred_proxy_name == lower_wide(candidate.second);
        const bool is_free = GetFileAttributesW(join(context.game_dir, candidate.second).c_str()) == INVALID_FILE_ATTRIBUTES;
        if (is_preferred || (chosen_folder.empty() && is_free)) chosen_folder = candidate.first;
        if (is_preferred) break;
    }

    std::vector<std::wstring> written;
    if (!top_level_folders.empty() && chosen_folder.empty()) {
        return reply_error("dlssgtofsr3.proxyConflict",
            "dbghelp.dll, version.dll and winhttp.dll are all already used in this game's folder. "
            "Remove one of those before installing dlssg-to-fsr3.");
    }
    const bool ok = chosen_folder.empty()
        ? extract_zip_to_directory(response.body, context.game_dir, written, error)
        : extract_zip_folder_to_directory(response.body, chosen_folder, context.game_dir, written, error);
    if (!ok) return reply_error("dlssgtofsr3.extract", error);
    if (written.empty()) return reply_error("dlssgtofsr3.extract", "The dlssg-to-fsr3 download did not contain any files.");

    if (!write_install_manifest(context.game_dir, kFsrMarkerName, written, selected.name.empty() ? selected.file_name : selected.name)) {
        log("Could not write the dlssg-to-fsr3 install manifest; 'remove' may not find every file later.");
    }

    return reply_ok("dlssgToFsr3Installed", "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"variant\":" + json_escape(variant_filter) +
        ",\"statusText\":\"dlssg-to-fsr3 (" + variant_filter + ") was installed. Check the extracted READ ME.txt for any remaining manual step.\"}");
}

std::string fsr_handle_remove(const std::string& app_id)
{
    AppContext context;
    std::string error;
    if (!resolve_app(app_id, context, error, "dlssg-to-fsr3")) return reply_error("dlssgtofsr3.unresolved", error);
    if (!remove_installed_files(context.game_dir, kFsrMarkerName)) {
        return reply_error("dlssgtofsr3.removeFailed", "dlssg-to-fsr3 was not installed by this plugin, or some files could not be removed.");
    }
    return reply_ok("dlssgToFsr3Removed", "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"statusText\":\"dlssg-to-fsr3 was removed.\"}");
}

// ---------------------------------------------------------------------------
// DLSS Enabler (Nexus mod 757) — single proxy DLL renamed to one of a handful of
// well-known names next to the game's executable.
// ---------------------------------------------------------------------------
constexpr const char* kEnNexusModId = "757";
constexpr const wchar_t* kEnMarkerName = L"hubcap-dlssenabler-install.json";
const wchar_t* const kEnProxyNames[] = {L"dxgi.dll", L"version.dll", L"winmm.dll", L"dbghelp.dll", L"psapi.dll", L"winhttp.dll"};

bool en_looks_like_pe_dll(const std::string& bytes)
{
    return bytes.size() >= 2 && bytes[0] == 'M' && bytes[1] == 'Z';
}

bool en_find_enabler_dll_in_zip(const std::string& zip_bytes, std::string& dll_bytes, std::wstring& suggested_name, std::string& error)
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
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&archive, best_index, &stat);
        const std::string entry = stat.m_filename;
        const size_t separator = entry.find_last_of("/\\");
        suggested_name = widen(separator == std::string::npos ? entry : entry.substr(separator + 1));
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

bool en_is_known_proxy_name(const std::wstring& name)
{
    for (const wchar_t* candidate : kEnProxyNames) if (_wcsicmp(candidate, name.c_str()) == 0) return true;
    return false;
}

bool en_pick_proxy_name(const std::wstring& game_dir, const std::vector<std::wstring>& existing_manifest,
    const std::wstring& suggested_name, std::wstring& chosen, std::string& error)
{
    if (!existing_manifest.empty()) { chosen = existing_manifest.front(); return true; }
    if (en_is_known_proxy_name(suggested_name)) {
        const std::wstring candidate = join(game_dir, suggested_name);
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) { chosen = candidate; return true; }
    }
    for (const wchar_t* name : kEnProxyNames) {
        const std::wstring candidate = join(game_dir, name);
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) { chosen = candidate; return true; }
    }
    error = "Every known proxy DLL name (dxgi.dll, version.dll, winmm.dll, dbghelp.dll, psapi.dll, winhttp.dll) "
        "already exists in this game's folder. Remove one of those (or whatever already provides DLSS) before installing DLSS Enabler.";
    return false;
}

std::string en_state_json(const AppContext& context)
{
    std::vector<std::wstring> manifest_files;
    std::string manifest_version;
    const bool installed_by_us = context.resolved &&
        read_install_manifest(context.game_dir, kEnMarkerName, manifest_files, manifest_version);
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

std::string en_handle_state(const std::string& app_id)
{
    AppContext context;
    std::string error;
    resolve_app(app_id, context, error, "DLSS Enabler");
    return reply_ok("dlssEnablerState", en_state_json(context));
}

std::string en_handle_install(const std::string& app_id)
{
    AppContext context;
    std::string error;
    if (!resolve_app(app_id, context, error, "DLSS Enabler")) return reply_error("dlssenabler.unresolved", error);

    std::vector<std::wstring> existing_manifest;
    std::string existing_version;
    read_install_manifest(context.game_dir, kEnMarkerName, existing_manifest, existing_version);

    const std::string api_key = read_nexus_api_key(context.local_app_data);
    if (api_key.empty()) return reply_error("dlssenabler.noApiKey", "Add a Nexus API key in DLSS Suite's Hubcap Settings entry to install DLSS Enabler.");

    std::vector<NexusFile> files;
    if (!nexus_list_files(api_key, kEnNexusModId, files, error)) return reply_error("dlssenabler.nexusList", error);
    std::vector<NexusFile> installable_files;
    for (const auto& file : files) {
        const std::string lower = lower_narrow(file.file_name);
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0) continue;
        installable_files.push_back(file);
    }
    NexusFile selected;
    if (!nexus_select_file(installable_files, "", selected)) {
        return reply_error("dlssenabler.noFile", "Nexus did not return a downloadable (non-installer) DLSS Enabler file.");
    }

    std::string download_url;
    if (!nexus_download_link(api_key, kEnNexusModId, selected.file_id, download_url, error)) return reply_error("dlssenabler.nexusLink", error);

    const HttpResult response = http_get_url(download_url, 64 * 1024 * 1024, 120000, L"User-Agent: HubcapUI-DLSS-Plugins\r\n");
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        return reply_error("dlssenabler.download", response.error.empty() ? http_error(response.status) : response.error);
    }

    std::string dll_bytes;
    std::wstring suggested_name;
    if (en_looks_like_pe_dll(response.body)) {
        dll_bytes = response.body;
    } else if (!en_find_enabler_dll_in_zip(response.body, dll_bytes, suggested_name, error)) {
        return reply_error("dlssenabler.extract", error);
    }
    if (!en_looks_like_pe_dll(dll_bytes)) return reply_error("dlssenabler.extract", "The DLSS Enabler file was not a valid DLL.");

    std::wstring proxy_path;
    if (!en_pick_proxy_name(context.game_dir, existing_manifest, suggested_name, proxy_path, error)) {
        return reply_error("dlssenabler.proxyConflict", error);
    }

    if (!write_file_atomic(proxy_path, dll_bytes.data(), dll_bytes.size())) {
        return reply_error("dlssenabler.write", "Could not write the DLSS Enabler DLL into the game folder.");
    }
    if (!write_install_manifest(context.game_dir, kEnMarkerName, {proxy_path}, selected.name.empty() ? selected.file_name : selected.name)) {
        log("Could not write the DLSS Enabler install manifest; 'remove' may not find the file later.");
    }

    const size_t separator = proxy_path.find_last_of(L"\\/");
    const std::string proxy_name = narrow(separator == std::wstring::npos ? proxy_path : proxy_path.substr(separator + 1));
    return reply_ok("dlssEnablerInstalled", "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"proxyName\":" + json_escape(proxy_name) +
        ",\"statusText\":\"DLSS Enabler was installed as " + proxy_name + ". Launch the game and press ` to open its sidebar.\"}");
}

std::string en_handle_remove(const std::string& app_id)
{
    AppContext context;
    std::string error;
    if (!resolve_app(app_id, context, error, "DLSS Enabler")) return reply_error("dlssenabler.unresolved", error);
    if (!remove_installed_files(context.game_dir, kEnMarkerName)) {
        return reply_error("dlssenabler.removeFailed", "DLSS Enabler was not installed by this plugin, or the file could not be removed.");
    }
    return reply_ok("dlssEnablerRemoved", "{\"appId\":" + json_escape(context.app_id) +
        ",\"gameName\":" + json_escape(context.game_name) +
        ",\"statusText\":\"DLSS Enabler was removed.\"}");
}

// ---------------------------------------------------------------------------
// Shared Nexus API key — one instance now, not three. No more "which plugin owns
// this" question since there's only one plugin.
// ---------------------------------------------------------------------------
std::string handle_set_nexus_key(const std::string& api_key)
{
    const std::string trimmed = trim(api_key);
    const std::string validation_error = validate_nexus_api_key(trimmed);
    if (!validation_error.empty()) return reply_error("nexusKey.invalid", validation_error);
    const std::wstring local_app_data = g_host && g_host->local_app_data ? g_host->local_app_data() : L"";
    std::string error;
    if (!write_nexus_api_key(local_app_data, trimmed, error)) return reply_error("nexusKey.saveFailed", error);
    return reply_ok("nexusKeySaved", "{\"statusText\":\"Nexus API key saved.\"}");
}

std::string handle_get_nexus_key()
{
    const std::wstring local_app_data = g_host && g_host->local_app_data ? g_host->local_app_data() : L"";
    return reply_ok("nexusKeyState", "{\"apiKey\":" + json_escape(read_nexus_api_key(local_app_data)) + "}");
}

// ---------------------------------------------------------------------------
// Bulk install scan — lets the Manage submenu show "Enable X"/"Remove X" for a
// game the first time it's right-clicked, not just after the player has visited
// its library page (which is what the per-app `state` action depends on alone).
// Walks every Steam library's appmanifest_*.acf once and checks each feature's
// marker file directly, instead of resolving one app_id at a time.
// ---------------------------------------------------------------------------
struct InstalledScanResult {
    std::vector<std::string> all_app_ids, dlsstweaks, dlssgtofsr3, dlssenabler;
};

// Single pass over every library's appmanifest_*.acf files. Returns every app seen
// (`all_app_ids`), not just matches — the caller needs the full set so it can mark
// an app explicitly "not installed" instead of leaving it unknown, which is what
// actually lets the Manage submenu show "Enable X" immediately for a game that's
// never been visited, not just "Remove X" for one that already has the mod.
InstalledScanResult scan_installed(const std::wstring& steam_root)
{
    InstalledScanResult out;
    for (const auto& library : steam_library_roots(steam_root)) {
        const std::wstring pattern = join(library, L"steamapps\\appmanifest_*.acf");
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW(pattern.c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring name = data.cFileName;
            constexpr size_t kPrefixLen = 12; // "appmanifest_"
            constexpr size_t kSuffixLen = 4;  // ".acf"
            if (name.size() <= kPrefixLen + kSuffixLen) continue;
            const std::wstring app_id_w = name.substr(kPrefixLen, name.size() - kPrefixLen - kSuffixLen);
            const std::string app_id(app_id_w.begin(), app_id_w.end());
            if (!numeric_app_id(app_id)) continue;
            std::string manifest;
            if (!read_text_file(join(library, L"steamapps\\" + name), manifest)) continue;
            const std::string install_dir = vdf_value(manifest, "installdir");
            if (install_dir.empty()) continue;
            out.all_app_ids.push_back(app_id);
            const std::wstring game_dir = join(library, L"steamapps\\common\\" + widen(install_dir));
            if (GetFileAttributesW(join(game_dir, kTwMarkerName).c_str()) != INVALID_FILE_ATTRIBUTES) out.dlsstweaks.push_back(app_id);
            if (GetFileAttributesW(join(game_dir, kFsrMarkerName).c_str()) != INVALID_FILE_ATTRIBUTES) out.dlssgtofsr3.push_back(app_id);
            if (GetFileAttributesW(join(game_dir, kEnMarkerName).c_str()) != INVALID_FILE_ATTRIBUTES) out.dlssenabler.push_back(app_id);
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    return out;
}

std::string handle_list_installed()
{
    const std::wstring steam_root = g_host && g_host->steam_root ? g_host->steam_root() : L"";
    if (steam_root.empty()) return reply_error("listInstalled.unavailable", "Could not determine the Steam install path.");
    const InstalledScanResult scan = scan_installed(steam_root);
    const auto to_json_array = [](const std::vector<std::string>& ids) {
        std::string out = "[";
        for (size_t i = 0; i < ids.size(); ++i) { if (i) out += ","; out += json_escape(ids[i]); }
        return out + "]";
    };
    return reply_ok("installedList", "{\"allAppIds\":" + to_json_array(scan.all_app_ids) +
        ",\"dlsstweaks\":" + to_json_array(scan.dlsstweaks) +
        ",\"dlssgtofsr3\":" + to_json_array(scan.dlssgtofsr3) +
        ",\"dlssenabler\":" + to_json_array(scan.dlssenabler) + "}");
}

// ---------------------------------------------------------------------------
// Dispatch — "state"/"install"/"remove" are shared action names across all three
// features, disambiguated by data.pluginData.feature ("dlsstweaks" / "dlssgtofsr3"
// / "dlssenabler"). setNexusKey/getNexusKey need no feature — one shared handler.
// ---------------------------------------------------------------------------
const char* HUBCAP_PLUGIN_CALL handle(const char* request_json)
{
    yyjson_doc* document = request_json ? yyjson_read(request_json, std::strlen(request_json), 0) : nullptr;
    if (!document) return copy_result(reply_error("request.invalid", "Invalid plugin request."));
    yyjson_val* root = yyjson_doc_get_root(document);
    yyjson_val* data = yyjson_obj_get(root, "data");
    const char* action = yyjson_get_str(yyjson_obj_get(data, "pluginAction"));
    const char* app_id_value = yyjson_get_str(yyjson_obj_get(root, "appId"));
    yyjson_val* plugin_data = yyjson_obj_get(data, "pluginData");
    const char* feature_value = yyjson_get_str(yyjson_obj_get(plugin_data, "feature"));
    const std::string app_id = app_id_value ? app_id_value : "";
    const std::string feature = feature_value ? feature_value : "";
    std::string result;
    if (!action) {
        result = reply_error("request.invalid", "Missing plugin action.");
    } else if (std::strcmp(action, "setNexusKey") == 0) {
        const char* key_value = yyjson_get_str(yyjson_obj_get(plugin_data, "nexusApiKey"));
        result = handle_set_nexus_key(key_value ? key_value : "");
    } else if (std::strcmp(action, "getNexusKey") == 0) {
        result = handle_get_nexus_key();
    } else if (std::strcmp(action, "listInstalled") == 0) {
        result = handle_list_installed();
    } else if (feature == "dlsstweaks" && std::strcmp(action, "state") == 0) {
        result = tw_handle_state(app_id);
    } else if (feature == "dlsstweaks" && std::strcmp(action, "install") == 0) {
        result = tw_handle_install(app_id);
    } else if (feature == "dlsstweaks" && std::strcmp(action, "remove") == 0) {
        result = tw_handle_remove(app_id);
    } else if (feature == "dlssgtofsr3" && std::strcmp(action, "state") == 0) {
        result = fsr_handle_state(app_id);
    } else if (feature == "dlssgtofsr3" && std::strcmp(action, "install") == 0) {
        result = fsr_handle_install(app_id);
    } else if (feature == "dlssgtofsr3" && std::strcmp(action, "remove") == 0) {
        result = fsr_handle_remove(app_id);
    } else if (feature == "dlssenabler" && std::strcmp(action, "state") == 0) {
        result = en_handle_state(app_id);
    } else if (feature == "dlssenabler" && std::strcmp(action, "install") == 0) {
        result = en_handle_install(app_id);
    } else if (feature == "dlssenabler" && std::strcmp(action, "remove") == 0) {
        result = en_handle_remove(app_id);
    } else {
        result = reply_error("request.unknown", "Unknown plugin action or missing/unrecognized feature.");
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
    const ID = 'hubcap.dlsssuite';
    if (!host || host.version !== 1 || globalThis.__hubcapDlssSuiteInstalled) return;
    const required = ['send', 'onResult', 'showConfirm', 'setModalState', 'setManageMenuEntry', 'libraryAppId'];
    if (!required.every(key => typeof host[key] === 'function')) return;
    globalThis.__hubcapDlssSuiteInstalled = true;

    // Cache of the last known installed state per app, per feature, so the Manage
    // submenu can show "Enable X"/"Remove X" (like Steam's own Add/Remove
    // Favorites) once a game's state has been seen, instead of a static label
    // forever. Populated by the background poll below and by every live state
    // check triggered from the menu itself.
    const stateCache = new Map(); // `${feature}:${appId}` -> boolean installed
    let installedGames = new Set(); // every Steam-installed appId, from listInstalled's allAppIds

    // ---- Shared Nexus API key: one dialog, one Hubcap Settings entry, no more
    // cross-plugin "who registers it" coordination — there's only one plugin now.
    let nexusKeyFetchPending = null, nexusKeySavePending = null;
    const showNexusKeyPrompt = currentValue => {
        let input;
        host.showConfirm({
            kind: 'nexus-api-key',
            title: 'Nexus API Key',
            message: 'Used by DLSSTweaks, dlssg-to-fsr3, and DLSS Enabler (all part of DLSS Suite) to download from Nexus Mods. Find yours at nexusmods.com, Account Settings → API Access.',
            renderMessage: element => {
                element.insertAdjacentHTML('beforeend', '<div style="margin-top:8px"><input type="password" id="hubcap-nexus-key-input" style="width:100%;box-sizing:border-box;padding:6px;border-radius:4px;border:1px solid #67707b;background:#0e1822;color:#c6d4df" placeholder="Paste your Nexus API key" autocomplete="off" /></div>');
                input = element.querySelector('#hubcap-nexus-key-input');
                if (input && currentValue) input.value = currentValue;
            },
            confirmLabel: 'Save',
            cancelLabel: 'Cancel'
        }, modal => {
            const value = (input?.value || '').trim();
            if (!value) {
                host.setModalState(modal, { phase: 'error', title: 'Nexus API Key', message: 'Enter a key, or cancel.' });
                return false;
            }
            host.setModalState(modal, { phase: 'busy', title: 'Nexus API Key', message: 'Validating and saving...' });
            const request = { modal, timeout: 0, id: '' };
            nexusKeySavePending = request;
            request.timeout = setTimeout(() => {
                if (nexusKeySavePending !== request) return;
                nexusKeySavePending = null;
                host.setModalState(modal, { phase: 'error', title: 'Nexus API Key', message: 'Timed out.' });
            }, 20000);
            request.id = host.send(ID, 'setNexusKey', { nexusApiKey: value });
            return false;
        });
    };
    const openNexusKeyDialog = () => {
        if (nexusKeyFetchPending || nexusKeySavePending) return;
        const request = { id: '', timeout: 0 };
        nexusKeyFetchPending = request;
        request.timeout = setTimeout(() => {
            if (nexusKeyFetchPending !== request) return;
            nexusKeyFetchPending = null;
            showNexusKeyPrompt('');
        }, 8000);
        request.id = host.send(ID, 'getNexusKey', {});
    };
    if (typeof host.setPluginSettingsEntry === 'function') {
        host.setPluginSettingsEntry(ID + '.nexuskey', { label: 'Set Nexus API Key', onActivate: openNexusKeyDialog });
    }

    // ---- Generic per-feature Manage-submenu machinery, parameterized instead of
    // copy-pasted three times (dlsstweaks / dlssgtofsr3 / dlssenabler). Each entry
    // lives in Steam's native Manage submenu (not the retired Game Settings
    // dropdown) and is global, not per-app — on activation we fetch live install
    // state for whichever game was right-clicked, then show install/remove.
    const makeFeature = (feature, settingId, labelBase, confirmMessage) => {
        let pending = null, pollPending = null;
        const cacheKey = appId => `${feature}:${appId}`;
        const command = (appId, action) => host.send(ID, action, { appId, feature });
        const openForApp = appId => {
            if (pending || !appId) return;
            const request = { id: '', timeout: 0, phase: 'state', appId };
            pending = request;
            request.timeout = setTimeout(() => { if (pending === request) pending = null; }, 8000);
            request.id = command(appId, 'state');
        };
        return {
            onResult(response, envelope) {
                if (pollPending && envelope?.replyTo === pollPending.id) {
                    const request = pollPending;
                    pollPending = null;
                    clearTimeout(request.timeout);
                    if (response?.ok && response.state?.gameResolved) stateCache.set(cacheKey(request.appId), !!response.state.installed);
                    return true;
                }
                if (!pending || envelope?.replyTo !== pending.id) return false;
                const request = pending;
                pending = null;
                clearTimeout(request.timeout);
                if (request.phase === 'state') {
                    if (!response?.ok || !response.state?.gameResolved) return true;
                    const installed = !!response.state.installed, variant = response.state.variant || '', appId = request.appId;
                    stateCache.set(cacheKey(appId), installed);
                    host.showConfirm({
                        kind: installed ? `${feature}-remove` : `${feature}-install`,
                        title: installed ? `Remove ${labelBase}?` : `Enable ${labelBase}?`,
                        message: installed
                            ? `Remove the ${labelBase} files this plugin installed for this game?`
                            : confirmMessage(variant),
                        confirmLabel: installed ? 'Remove' : 'Install',
                        cancelLabel: 'Cancel'
                    }, modal => {
                        host.setModalState(modal, { phase: 'busy', title: labelBase, message: installed ? `Removing ${labelBase}...` : `Downloading ${labelBase} from Nexus Mods...` });
                        const actionRequest = { id: '', timeout: 0, phase: 'action', modal, appId, nextInstalled: !installed };
                        pending = actionRequest;
                        actionRequest.timeout = setTimeout(() => {
                            if (pending !== actionRequest) return;
                            pending = null;
                            host.setModalState(modal, { phase: 'error', title: `${labelBase} request timed out`, message: 'Close this dialog and try again.' });
                        }, 60000);
                        actionRequest.id = command(appId, installed ? 'remove' : 'install');
                        return false;
                    });
                    return true;
                }
                if (response?.ok) stateCache.set(cacheKey(request.appId), request.nextInstalled);
                host.setModalState(request.modal, {
                    phase: response?.ok ? 'success' : 'error',
                    title: response?.ok ? labelBase : `${labelBase} request failed`,
                    message: response?.state?.statusText || response?.error?.message || 'No message returned.'
                });
                return true;
            },
            install() {
                host.setManageMenuEntry(settingId, {
                    label: `Enable ${labelBase}`,
                    resolveLabel: appId => stateCache.get(cacheKey(appId)) ? `Remove ${labelBase}` : `Enable ${labelBase}`,
                    resolveVisible: appId => installedGames.has(String(appId)),
                    onActivate: ({ appId }) => openForApp(appId)
                });
            },
            // Keeps stateCache warm for the game page currently being viewed, so its
            // Manage entry already reads correctly the first time it's opened — the
            // same "known ahead of time" feel Steam's own Add/Remove Favorites has,
            // without polling every game in the library.
            pollCurrent(appId) {
                if (pollPending || !appId) return;
                const request = { id: '', timeout: 0, appId };
                pollPending = request;
                request.timeout = setTimeout(() => { if (pollPending === request) pollPending = null; }, 8000);
                request.id = command(appId, 'state');
            }
        };
    };

    const features = [
        makeFeature('dlsstweaks', ID + '.dlsstweaks', 'DLSSTweaks',
            () => 'Download and install DLSSTweaks (emoose) for this game from Nexus Mods? Requires a Nexus API key (see this plugin’s entry in Hubcap Settings).'),
        makeFeature('dlssgtofsr3', ID + '.dlssgtofsr3', 'dlssg-to-fsr3',
            variant => `Download and install dlssg-to-fsr3 (Nukem) for this game from Nexus Mods, using the ${variant || 'Universal'} build? Requires a Nexus API key (see this plugin’s entry in Hubcap Settings).`),
        makeFeature('dlssenabler', ID + '.dlssenabler', 'DLSS Enabler',
            () => 'Download and install DLSS Enabler (artur07305) for this game from Nexus Mods? Requires a Nexus API key (see this plugin’s entry in Hubcap Settings). Do not use in multiplayer games: anti-cheat may flag frame generation injection as cheating.'),
    ];

    host.onResult(ID, (response, envelope) => {
        if (response?.ok && response.kind === 'installedList') {
            const allIds = response.state?.allAppIds || [];
            installedGames = new Set(allIds.map(String));
            for (const feature of ['dlsstweaks', 'dlssgtofsr3', 'dlssenabler']) {
                const installedSet = new Set(response.state?.[feature] || []);
                for (const appId of allIds) stateCache.set(`${feature}:${appId}`, installedSet.has(appId));
            }
            return;
        }
        if (nexusKeyFetchPending && envelope?.replyTo === nexusKeyFetchPending.id) {
            const request = nexusKeyFetchPending;
            nexusKeyFetchPending = null;
            clearTimeout(request.timeout);
            showNexusKeyPrompt(response?.ok ? (response.state?.apiKey || '') : '');
            return;
        }
        if (nexusKeySavePending && envelope?.replyTo === nexusKeySavePending.id) {
            const request = nexusKeySavePending;
            nexusKeySavePending = null;
            clearTimeout(request.timeout);
            host.setModalState(request.modal, {
                phase: response?.ok ? 'success' : 'error',
                title: 'Nexus API Key',
                message: response?.state?.statusText || response?.error?.message || 'No message returned.'
            });
            return;
        }
        for (const feature of features) if (feature.onResult(response, envelope)) return;
    });

    for (const feature of features) feature.install();
    host.send(ID, 'listInstalled', {});

    let lastPolledApp = '';
    setInterval(() => {
        const app = String(host.libraryAppId() || '');
        if (!app || app === lastPolledApp) return;
        lastPolledApp = app;
        for (const feature of features) feature.pollCurrent(app);
    }, 1000);
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
    plugin->name = "DLSS Suite";
    plugin->version = kVersion;
    plugin->description = "DLSSTweaks, dlssg-to-fsr3, and DLSS Enabler in one plugin, sharing one Nexus API key entry in Hubcap Settings.";
    plugin->ui_script = k_ui;
    plugin->handle = handle;
    plugin->free_string = free_string;
    plugin->shutdown = shutdown;
    if (host->log) host->log(plugin->id, "DLSS Suite plugin initialized.");
    return true;
}
