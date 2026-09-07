#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwctype>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "miniz.h"
#include "yyjson.h"
#include "hubcap_service.h"

std::string hubcap_plugins_catalog_json();
std::string hubcap_plugins_set_enabled(const std::string& id, bool enabled);
std::string hubcap_plugins_handle(const std::string& id, const std::string& request_json);

extern "C" void hubcap_runtime_log(const char* message);

namespace {
#ifndef HUBCAP_UI_VERSION
#define HUBCAP_UI_VERSION "0.0.0-dev"
#endif
struct Config {
    std::string api_key;
    std::wstring lua_dir;
    std::wstring path;
    std::string error;
};

struct LauncherSettings {
    bool collection_sync_enabled = false;
    bool show_reno_button = false;
    bool show_optiscaler_button = false;
    std::string collection_name;
    std::string nexus_api_key;
    std::string optiscaler_gpu = "Auto";
    std::string optiscaler_upscaler = "Auto";
};

struct HttpResult {
    DWORD status = 0;
    std::string body;
    std::string error;
};

struct ResolvedApp {
    std::string app_id;
    std::string visible_app_id;
    std::string game_name;
    std::string parent_name;
    bool is_dlc = false;
};

struct RenoMod {
    std::string name;
    std::string snapshot_url;
    std::string snapshot_url_32;
    std::string nexus_url;
    std::string discord_url;
    std::string engine;
    std::string notes;
    std::map<std::string, int> settings;
    bool generic = false;
    bool construction = false;
    bool blocked = false;
};

struct RenoInstallTarget {
    std::wstring game_dir;
    std::wstring executable;
    std::wstring reshade_dir;
};

bool reno_installation_state(const ResolvedApp& app, RenoInstallTarget* target = nullptr,
    std::vector<std::wstring>* addons = nullptr);

SRWLOCK g_reno_catalog_lock = SRWLOCK_INIT;
std::vector<RenoMod> g_reno_catalog;
ULONGLONG g_reno_catalog_at = 0;
ULONGLONG g_reno_catalog_attempt_at = 0;
ULONGLONG g_reno_catalog_updated_ms = 0;
bool g_reno_catalog_failed = false;

INIT_ONCE g_lua_watcher_once = INIT_ONCE_STATIC_INIT;
HANDLE g_lua_watcher_reconfigure = nullptr;
SRWLOCK g_lua_snapshot_lock = SRWLOCK_INIT;
std::atomic<std::uint64_t> g_lua_generation{0};
std::wstring g_lua_snapshot_dir;
std::vector<std::string> g_lua_snapshot_ids;

struct UpdateState {
    std::string phase = "idle";
    std::string latest_version;
    std::string release_name;
    std::string release_notes;
    std::string download_path;
    std::string asset_digest;
    std::string staged_dll;
    std::string staged_dll_hash;
    std::string error;
    std::string retry_action;
    bool boot_prompt_pending = false;
    bool manual_prompt_pending = false;
    bool deferred = false;
};

SRWLOCK g_update_lock = SRWLOCK_INIT;
UpdateState g_update_state;
std::atomic<bool> g_update_check_running{false};
std::atomic<bool> g_update_download_running{false};
std::atomic<bool> g_update_boot_started{false};
std::atomic<bool> g_update_boot_dialog_running{false};

void start_boot_update_dialog();
LauncherSettings read_launcher_settings();
std::string detected_gpu_vendor();

std::wstring widen(const std::string& value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring module_root()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return {};
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
    return path;
}

std::wstring join(const std::wstring& left, const std::wstring& right)
{
    if (left.empty()) return right;
    return left + (left.back() == L'\\' ? L"" : L"\\") + right;
}

std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string unquote(std::string value)
{
    value = trim(value);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
        std::string decoded;
        decoded.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size() && (value[i + 1] == '\\' || value[i + 1] == '"')) ++i;
            decoded.push_back(value[i]);
        }
        return decoded;
    }
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

Config read_config()
{
    Config config;
    const std::wstring root = module_root();
    config.path = join(root, L"config\\hubcaptools\\config.yaml");
    std::ifstream file(config.path.c_str(), std::ios::binary);
    if (!file) {
        config.error = "Config file not found. Make sure HubcapTools is installed.";
        return config;
    }

    std::string line;
    std::string lua_dir;
    while (std::getline(file, line)) {
        const size_t separator = line.find(':');
        if (separator == std::string::npos) continue;
        const std::string key = trim(line.substr(0, separator));
        const std::string value = unquote(line.substr(separator + 1));
        if (key == "HubcapApiKey") config.api_key = value;
        if (key == "HubcapLuaDir") lua_dir = value;
    }

    if (config.api_key.empty()) {
        config.error = "API Key missing.";
        return config;
    }
    if (lua_dir.empty()) {
        config.lua_dir = join(root, L"config\\hubcap-lua");
    } else {
        std::replace(lua_dir.begin(), lua_dir.end(), '/', '\\');
        const std::wstring raw = widen(lua_dir);
        DWORD needed = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
        if (needed > 1) {
            std::wstring expanded(needed, L'\0');
            ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), needed);
            expanded.resize(wcslen(expanded.c_str()));
            config.lua_dir = expanded;
        } else {
            config.lua_dir = raw;
        }
    }
    return config;
}

bool numeric_app_id(const std::string& app_id)
{
    return !app_id.empty() && app_id.size() <= 12 &&
        std::all_of(app_id.begin(), app_id.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool numeric_manifest_id(const std::string& manifest_id)
{
    return manifest_id.size() >= 12 && manifest_id.size() <= 20 &&
        std::all_of(manifest_id.begin(), manifest_id.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::wstring normalize_directory(std::wstring path)
{
    if (path.empty()) return path;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed > 1) {
        std::wstring full(needed, L'\0');
        const DWORD written = GetFullPathNameW(path.c_str(), needed, full.data(), nullptr);
        if (written > 0 && written < needed) {
            full.resize(written);
            path = std::move(full);
        }
    }
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

std::vector<std::string> scan_lua_app_ids(const std::wstring& directory)
{
    std::vector<std::string> ids;
    if (directory.empty()) return ids;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(join(directory, L"*.lua").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            std::wstring name = data.cFileName;
            if (name.size() <= 4 || _wcsicmp(name.c_str() + name.size() - 4, L".lua") != 0) continue;
            name.resize(name.size() - 4);
            const std::string id = narrow(name);
            if (numeric_app_id(id)) ids.push_back(id);
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    std::sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) {
        return _strtoui64(a.c_str(), nullptr, 10) < _strtoui64(b.c_str(), nullptr, 10);
    });
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void publish_lua_snapshot(const std::wstring& directory, bool force_generation = false)
{
    const std::wstring normalized = normalize_directory(directory);
    std::vector<std::string> ids = scan_lua_app_ids(normalized);
    bool changed = false;
    AcquireSRWLockExclusive(&g_lua_snapshot_lock);
    if (_wcsicmp(g_lua_snapshot_dir.c_str(), normalized.c_str()) != 0 || g_lua_snapshot_ids != ids) {
        g_lua_snapshot_dir = normalized;
        g_lua_snapshot_ids = std::move(ids);
        changed = true;
    }
    ReleaseSRWLockExclusive(&g_lua_snapshot_lock);
    if (changed || force_generation || g_lua_generation.load() == 0) g_lua_generation.fetch_add(1);
}

bool relevant_lua_notification(const unsigned char* buffer, DWORD bytes)
{
    if (bytes == 0) return true;
    DWORD offset = 0;
    while (offset < bytes) {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
        std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
        if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".lua") == 0) {
            name.resize(name.size() - 4);
            if (numeric_app_id(narrow(name))) return true;
        }
        if (info->NextEntryOffset == 0) break;
        offset += info->NextEntryOffset;
    }
    return false;
}

DWORD WINAPI lua_watcher_thread(void*)
{
    std::vector<unsigned char> buffer(64 * 1024);
    for (;;) {
        const Config config = read_config();
        const std::wstring directory = normalize_directory(config.lua_dir);
        publish_lua_snapshot(directory);

        HANDLE change = CreateFileW(
            directory.c_str(),
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (change == INVALID_HANDLE_VALUE) {
            WaitForSingleObject(g_lua_watcher_reconfigure, 1000);
            continue;
        }

        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready) {
            CloseHandle(change);
            WaitForSingleObject(g_lua_watcher_reconfigure, 1000);
            continue;
        }

        bool reconfigure = false;
        while (!reconfigure) {
            ResetEvent(ready);
            OVERLAPPED overlapped{};
            overlapped.hEvent = ready;
            const BOOL armed = ReadDirectoryChangesW(
                change,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                nullptr,
                &overlapped,
                nullptr);
            if (!armed) break;

            HANDLE waits[] = {g_lua_watcher_reconfigure, ready};
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0) {
                CancelIoEx(change, &overlapped);
                reconfigure = true;
                continue;
            }
            if (wait != WAIT_OBJECT_0 + 1) {
                CancelIoEx(change, &overlapped);
                break;
            }

            DWORD bytes = 0;
            GetOverlappedResult(change, &overlapped, &bytes, FALSE);
            if (WaitForSingleObject(g_lua_watcher_reconfigure, 125) == WAIT_OBJECT_0) {
                reconfigure = true;
                continue;
            }
            if (relevant_lua_notification(buffer.data(), bytes)) publish_lua_snapshot(directory, true);
        }
        CloseHandle(ready);
        CloseHandle(change);
    }
}

BOOL CALLBACK start_lua_watcher_once(PINIT_ONCE, PVOID, PVOID*)
{
    g_lua_watcher_reconfigure = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_lua_watcher_reconfigure) return FALSE;
    HANDLE thread = CreateThread(nullptr, 0, lua_watcher_thread, nullptr, 0, nullptr);
    if (!thread) {
        CloseHandle(g_lua_watcher_reconfigure);
        g_lua_watcher_reconfigure = nullptr;
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}

std::string json_escape(const std::string& value)
{
    std::string result = "\"";
    for (unsigned char c : value) {
        switch (c) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20) {
                char escaped[7];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                result += escaped;
            } else {
                result.push_back(static_cast<char>(c));
            }
        }
    }
    result += '"';
    return result;
}

std::string reply_error(const std::string& id, const std::string& code, const std::string& message)
{
    return "{\"v\":1,\"replyTo\":" + json_escape(id) + ",\"ok\":false,\"error\":{\"code\":" +
        json_escape(code) + ",\"message\":" + json_escape(message) + "}}";
}

std::string reply_ok(const std::string& id, const std::string& kind, const std::string& state)
{
    return "{\"v\":1,\"replyTo\":" + json_escape(id) + ",\"ok\":true,\"kind\":" + json_escape(kind) +
        ",\"state\":" + state + "}";
}

std::string http_error(DWORD status)
{
    switch (status) {
    case 401: return "Invalid Hubcap API key or unauthorized access.";
    case 403: return "Hubcap rejected this request. Check that your API key has access.";
    case 404: return "Hubcap does not have this file for this app yet.";
    case 429: return "Hubcap rate limit or daily limit reached. Try again later.";
    case 500: return "Hubcap server error. Try again later.";
    case 502: return "Hubcap gateway error. Try again later.";
    case 503: return "Hubcap service unavailable. Try again later.";
    case 504: return "Hubcap request timed out. Try again later.";
    default: return "Hubcap returned HTTP " + std::to_string(status) + ".";
    }
}

HttpResult http_get_host(const std::wstring& host, const std::wstring& path, const std::string& api_key,
    size_t maximum_bytes = 4 * 1024 * 1024, int response_timeout_ms = 15000,
    const std::wstring& extra_headers = L"")
{
    HttpResult result;
    HINTERNET session = WinHttpOpen(L"HubcapUI/0.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) {
        result.error = "Could not initialize Hubcap HTTP.";
        return result;
    }
    WinHttpSetTimeouts(session, 10000, 10000, response_timeout_ms, response_timeout_ms);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (!request) {
        result.error = "Could not create Hubcap request.";
    } else {
        if (!api_key.empty()) {
            const std::wstring authorization = L"Authorization: Bearer " + widen(api_key);
            WinHttpAddRequestHeaders(request, authorization.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
        if (_wcsicmp(host.c_str(), L"api.github.com") == 0) {
            WinHttpAddRequestHeaders(request, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28",
                static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
        if (!extra_headers.empty()) {
            WinHttpAddRequestHeaders(request, extra_headers.c_str(), static_cast<DWORD>(-1),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) {
            result.error = "Hubcap request failed or timed out.";
        } else {
            DWORD size = sizeof(result.status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &result.status, &size, nullptr);
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                if (result.body.size() + available > maximum_bytes) {
                    result.error = "Hubcap response was too large.";
                    break;
                }
                const size_t old_size = result.body.size();
                result.body.resize(old_size + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, result.body.data() + old_size, available, &read)) {
                    result.error = "Could not read Hubcap response.";
                    break;
                }
                result.body.resize(old_size + read);
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

HttpResult http_get_url(const std::string& url, size_t maximum_bytes, int response_timeout_ms,
    const std::wstring& extra_headers = L"")
{
    HttpResult result;
    const std::wstring wide_url = widen(url);
    if (wide_url.empty() || wide_url.size() > 8192) {
        result.error = "Download URL was invalid.";
        return result;
    }
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS || !components.lpszHostName) {
        result.error = "Only HTTPS download URLs are supported.";
        return result;
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.lpszExtraInfo && components.dwExtraInfoLength) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    return http_get_host(host, path.empty() ? L"/" : path, "", maximum_bytes, response_timeout_ms, extra_headers);
}

bool official_reshade_addon_release(std::string& resolved_url, std::string& version,
    std::string& signer_thumbprint, std::string& error)
{
    const HttpResult page = http_get_url("https://reshade.me/", 1024 * 1024, 20000);
    if (!page.error.empty() || page.status < 200 || page.status >= 300) {
        error = page.error.empty() ? "The official ReShade page returned HTTP " + std::to_string(page.status) + "." : page.error;
        return false;
    }
    static constexpr char suffix[] = "_Addon.exe";
    const size_t suffix_at = page.body.find(suffix);
    const size_t name_at = suffix_at == std::string::npos ? std::string::npos :
        page.body.rfind("ReShade_Setup_", suffix_at);
    if (name_at == std::string::npos || suffix_at - name_at > 96) {
        error = "The official ReShade page did not advertise a full-add-on installer.";
        return false;
    }
    const std::string filename = page.body.substr(name_at, suffix_at + sizeof(suffix) - 1 - name_at);
    if (filename.empty() || std::any_of(filename.begin(), filename.end(), [](unsigned char character) {
            return !std::isalnum(character) && character != '.' && character != '_' && character != '-';
        })) {
        error = "The official ReShade page returned an invalid installer name.";
        return false;
    }
    static constexpr char prefix[] = "ReShade_Setup_";
    version = filename.substr(sizeof(prefix) - 1, filename.size() - (sizeof(prefix) - 1) - (sizeof(suffix) - 1));
    if (version.empty() || std::any_of(version.begin(), version.end(), [](unsigned char character) {
            return !std::isdigit(character) && character != '.';
        })) {
        error = "The official ReShade page returned an invalid installer version.";
        return false;
    }
    static constexpr char thumbprint_label[] = "X.509 Digital Signature Thumbprint:";
    const size_t thumbprint_at = page.body.find(thumbprint_label);
    if (thumbprint_at != std::string::npos) {
        std::string token;
        for (size_t index = thumbprint_at + sizeof(thumbprint_label) - 1;
             index < page.body.size() && index < thumbprint_at + 256; ++index) {
            const unsigned char character = static_cast<unsigned char>(page.body[index]);
            if (std::isxdigit(character)) {
                token.push_back(static_cast<char>(std::toupper(character)));
                if (token.size() == 40) { signer_thumbprint = token; break; }
            } else if (!token.empty()) token.clear();
        }
    }
    if (signer_thumbprint.size() != 40) {
        error = "The official ReShade page did not provide a valid signer thumbprint.";
        return false;
    }
    resolved_url = "https://reshade.me/downloads/" + filename;
    return true;
}

HttpResult http_get(const std::wstring& path, const std::string& api_key, size_t maximum_bytes = 4 * 1024 * 1024,
    int response_timeout_ms = 15000)
{
    return http_get_host(L"hubcapmanifest.com", path, api_key, maximum_bytes, response_timeout_ms);
}

bool lua_exists(const Config& config, const std::string& app_id)
{
    return GetFileAttributesW(join(config.lua_dir, widen(app_id + ".lua")).c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string json_app_id(yyjson_val* value)
{
    if (!value) return {};
    if (yyjson_is_str(value)) return yyjson_get_str(value);
    if (yyjson_is_uint(value)) return std::to_string(yyjson_get_uint(value));
    if (yyjson_is_sint(value) && yyjson_get_sint(value) > 0) return std::to_string(yyjson_get_sint(value));
    return {};
}

std::string installed_game_name(const std::string& app_id);

ResolvedApp resolve_app(const std::string& visible_app_id)
{
    ResolvedApp resolved{visible_app_id, visible_app_id, installed_game_name(visible_app_id), "", false};
    const HttpResult response = http_get_host(
        L"store.steampowered.com",
        L"/api/appdetails?appids=" + widen(visible_app_id) + L"&filters=basic",
        "",
        1024 * 1024,
        10000);
    if (!response.error.empty() || response.status < 200 || response.status >= 300) return resolved;

    yyjson_doc* document = yyjson_read(response.body.data(), response.body.size(), 0);
    if (!document) return resolved;
    yyjson_val* root = yyjson_doc_get_root(document);
    yyjson_val* app = yyjson_obj_get(root, visible_app_id.c_str());
    yyjson_val* data = yyjson_obj_get(app, "data");
    const char* app_name = yyjson_get_str(yyjson_obj_get(data, "name"));
    if (app_name) resolved.game_name = app_name;
    const char* type = yyjson_get_str(yyjson_obj_get(data, "type"));
    yyjson_val* full_game = yyjson_obj_get(data, "fullgame");
    const std::string parent_id = json_app_id(yyjson_obj_get(full_game, "appid"));
    const char* parent_name = yyjson_get_str(yyjson_obj_get(full_game, "name"));
    if (type && _stricmp(type, "dlc") == 0 && numeric_app_id(parent_id)) {
        resolved.app_id = parent_id;
        resolved.parent_name = parent_name ? parent_name : "";
        if (!resolved.parent_name.empty()) resolved.game_name = resolved.parent_name;
        resolved.is_dlc = true;
    }
    yyjson_doc_free(document);
    return resolved;
}

std::string markdown_cell_text(std::string value)
{
    value = trim(value);
    if (!value.empty() && value.front() == '[') {
        const size_t close = value.find(']');
        if (close != std::string::npos && close > 1) value = value.substr(1, close - 1);
    }
    const std::vector<std::pair<std::string, std::string>> entities = {
        {"&amp;", "&"}, {"&middot;", " "}, {"&#39;", "'"}, {"&quot;", "\""}
    };
    for (const auto& entity : entities) {
        size_t position = 0;
        while ((position = value.find(entity.first, position)) != std::string::npos) {
            value.replace(position, entity.first.size(), entity.second);
            position += entity.second.size();
        }
    }
    return trim(value);
}

std::string ascii_lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

void replace_all(std::string& value, const std::string& needle, const std::string& replacement)
{
    size_t position = 0;
    while (!needle.empty() && (position = value.find(needle, position)) != std::string::npos) {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

std::string markdown_notes(std::string value)
{
    replace_all(value, "<br>", " ");
    replace_all(value, "<br/>", " ");
    replace_all(value, "<br />", " ");
    replace_all(value, "`", "");
    size_t tag = 0;
    while ((tag = value.find('<', tag)) != std::string::npos) {
        const size_t end = value.find('>', tag + 1);
        if (end == std::string::npos) break;
        value.erase(tag, end - tag + 1);
    }
    value = markdown_cell_text(value);
    std::string compact;
    bool space = false;
    for (unsigned char character : value) {
        if (std::isspace(character)) {
            space = !compact.empty();
        } else {
            if (space) compact.push_back(' ');
            compact.push_back(static_cast<char>(character));
            space = false;
        }
    }
    return trim(compact);
}

bool note_is_conditional(const std::string& lower, size_t position)
{
    const size_t begin = position > 64 ? position - 64 : 0;
    const size_t end = std::min(lower.size(), position + 140);
    const std::string clause = lower.substr(begin, end - begin);
    return clause.rfind("if ", 0) == 0 || clause.find(" if ") != std::string::npos ||
        clause.find(" optional") != std::string::npos ||
        clause.find(" depending") != std::string::npos || clause.find(" or ") != std::string::npos;
}

std::map<std::string, int> parse_reno_settings(const std::string& engine, const std::string& notes)
{
    std::map<std::string, int> settings;
    const std::string lower = ascii_lower(notes);
    const std::vector<std::string> formats = {
        "R8G8B8A8_TYPELESS", "B8G8R8A8_TYPELESS", "R11G11B10_FLOAT", "R10G10B10A2_TYPELESS",
        "R10G10B10A2_UNORM", "R16G16B16A16_TYPELESS", "R8G8B8A8_UNORM", "R8G8B8A8_UNORM_SRGB"
    };
    for (const auto& format : formats) {
        const std::string format_lower = ascii_lower(format);
        const size_t first = lower.find(format_lower);
        if (first != std::string::npos && lower.find(format_lower, first + format_lower.size()) != std::string::npos)
            continue;
        size_t position = 0;
        while ((position = lower.find(format_lower, position)) != std::string::npos) {
            if (engine == "Unity Engine" && format == "B8G8R8A8_TYPELESS") {
                position += format_lower.size();
                continue;
            }
            const size_t end = std::min(lower.size(), position + format_lower.size() + 48);
            const std::string clause = lower.substr(position, end - position);
            int mode = 0;
            if (clause.find("any size") != std::string::npos) mode = 3;
            else if (clause.find("output ratio") != std::string::npos) mode = 2;
            else if (clause.find("output size") != std::string::npos) mode = 1;
            if (mode && !note_is_conditional(lower, position)) settings["Upgrade_" + format] = mode;
            position += format_lower.size();
        }
    }
    const size_t copy_destinations = lower.find("upgrade copy destinations on");
    if (copy_destinations != std::string::npos && !note_is_conditional(lower, copy_destinations))
        settings["Upgrade_CopyDestinations"] = 1;
    if (engine == "Unity Engine") {
        size_t proxy = lower.find("enable swapchain proxy");
        if (proxy == std::string::npos) proxy = lower.find("swapchain proxy on");
        if (proxy == std::string::npos) proxy = lower.find("swap chain proxy on");
        if (proxy != std::string::npos && !note_is_conditional(lower, proxy))
            settings["Use_Swapchain_Proxy"] = 1;
        if (lower.find("swapchain encoding: gamma") != std::string::npos ||
            lower.find("swapchain encoding gamma") != std::string::npos || lower.find("swap chain encoding gamma") != std::string::npos)
            settings["Swapchain_Encoding"] = 1;
        if (lower.find("blit copy") != std::string::npos && lower.find("scaling only") != std::string::npos)
            settings["Blit_Copy_Hack"] = 3;
        if (lower.find("swap chain format scrgb") != std::string::npos || lower.find("swapchain format scrgb") != std::string::npos)
            settings["Upgrade_UseSCRGB"] = 1;
        if (lower.find("force borderless disabled") != std::string::npos) settings["ForceBorderless"] = 0;
        if (lower.find("lut shaper vanilla") != std::string::npos) settings["colorGradeInternalLUTShaper"] = 0;

        auto parse_offset = [&](const std::string& label, const std::string& key) {
            size_t position = lower.find(label);
            if (position == std::string::npos || note_is_conditional(lower, position)) return;
            position += label.size();
            while (position < lower.size() && (lower[position] == ' ' || lower[position] == ':' || lower[position] == '=')) ++position;
            if (position < lower.size() && lower[position] == '+') ++position;
            size_t end = position;
            while (end < lower.size() && std::isdigit(static_cast<unsigned char>(lower[end]))) ++end;
            if (end > position) settings[key] = std::stoi(lower.substr(position, end - position));
        };
        parse_offset("scaling offset", "Scaling_Offset");
        parse_offset("tonemap offset", "Tonemap_Offset");
        size_t both = lower.find("both compatibility offsets");
        if (both != std::string::npos && !note_is_conditional(lower, both)) {
            size_t number = lower.find_first_of("0123456789", both);
            if (number != std::string::npos) {
                size_t end = number;
                while (end < lower.size() && std::isdigit(static_cast<unsigned char>(lower[end]))) ++end;
                const int value = std::stoi(lower.substr(number, end - number));
                settings["Scaling_Offset"] = value;
                settings["Tonemap_Offset"] = value;
            }
        }
    }
    if (!settings.empty()) settings["SettingsMode"] = 2;
    return settings;
}

std::string expected_generic_snapshot(const std::string& engine, bool is_64_bit)
{
    if (engine == "Unreal Engine" && is_64_bit)
        return "https://clshortfuse.github.io/renodx/renodx-unrealengine.addon64";
    if (engine == "Unity Engine")
        return is_64_bit ? "https://notvoosh.github.io/renodx-unity/renodx-unityengine.addon64" :
            "https://notvoosh.github.io/renodx-unity/renodx-unityengine.addon32";
    return {};
}

bool write_reno_settings(const std::wstring& reshade_directory, const std::map<std::string, int>& settings,
    std::string& error)
{
    if (settings.empty()) return true;
    const std::wstring ini_path = join(reshade_directory, L"ReShade.ini");
    for (const auto& setting : settings) {
        const std::wstring key = widen(setting.first);
        const std::wstring value = std::to_wstring(setting.second);
        if (!WritePrivateProfileStringW(L"renodx", key.c_str(), value.c_str(), ini_path.c_str())) {
            error = "Could not apply the RenoDX wiki settings to ReShade.ini.";
            return false;
        }
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini_path.c_str());
    return true;
}

std::string normalized_game_name(const std::string& value)
{
    const std::wstring wide = widen(value);
    std::string result;
    bool separator = true;
    for (wchar_t character : wide) {
        if (!iswalnum(character)) {
            separator = true;
            continue;
        }
        if (separator && !result.empty()) result.push_back(' ');
        wchar_t lowered = towlower(character);
        if (lowered >= L'a' && lowered <= L'z') result.push_back(static_cast<char>(lowered));
        else if (lowered >= L'0' && lowered <= L'9') result.push_back(static_cast<char>(lowered));
        separator = false;
    }
    return trim(result);
}

std::set<std::string> game_name_tokens(const std::string& value)
{
    std::set<std::string> tokens;
    std::istringstream stream(normalized_game_name(value));
    std::string token;
    while (stream >> token) {
        if (token == "ii") token = "2";
        else if (token == "iii") token = "3";
        else if (token == "iv") token = "4";
        else if (token == "v") token = "5";
        if (token != "the" && token != "a" && token != "edition" && token != "tm") tokens.insert(token);
    }
    return tokens;
}

double game_name_match_score(const std::string& steam_name, const std::string& catalog_name)
{
    const std::string steam = normalized_game_name(steam_name);
    const std::string catalog = normalized_game_name(catalog_name);
    if (steam.empty() || catalog.empty()) return 0;
    if (steam == catalog) return 1;
    const auto left = game_name_tokens(steam);
    const auto right = game_name_tokens(catalog);
    if (left.empty() || right.empty()) return 0;
    size_t intersection = 0;
    for (const auto& token : left) if (right.count(token)) ++intersection;
    const size_t union_size = left.size() + right.size() - intersection;
    const double jaccard = union_size ? static_cast<double>(intersection) / union_size : 0;
    const double coverage = static_cast<double>(intersection) / std::min(left.size(), right.size());
    if (intersection >= 2 && coverage == 1.0 && jaccard >= 0.66) return 0.94;
    return intersection >= 2 && jaccard >= 0.82 ? jaccard : 0;
}

std::vector<RenoMod> parse_reno_catalog(const std::string& markdown)
{
    std::vector<RenoMod> mods;
    std::istringstream stream(markdown);
    std::string line;
    std::string generic_engine;
    std::string generic_snapshot_64;
    std::string generic_snapshot_32;
    bool capture_next_generic_link_line = false;
    while (std::getline(stream, line)) {
        bool capture_generic_links = false;
        if (line.rfind("### Unreal Engine", 0) == 0) {
            generic_engine = "Unreal Engine";
            generic_snapshot_64.clear();
            generic_snapshot_32.clear();
            capture_generic_links = true;
            capture_next_generic_link_line = false;
        } else if (line.rfind("### Unity Engine", 0) == 0) {
            generic_engine = "Unity Engine";
            generic_snapshot_64.clear();
            generic_snapshot_32.clear();
            capture_next_generic_link_line = true;
        } else if (capture_next_generic_link_line && !trim(line).empty()) {
            capture_generic_links = true;
            capture_next_generic_link_line = false;
        } else if (!generic_engine.empty() && line.rfind("# ", 0) == 0) {
            generic_engine.clear();
            generic_snapshot_64.clear();
            generic_snapshot_32.clear();
            capture_next_generic_link_line = false;
        }
        if (capture_generic_links) {
            size_t cursor = 0;
            while ((cursor = line.find("https://", cursor)) != std::string::npos) {
                const size_t end = line.find_first_of(") \t\r\n", cursor);
                const std::string url = line.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
                if (url == expected_generic_snapshot(generic_engine, true)) generic_snapshot_64 = url;
                else if (url == expected_generic_snapshot(generic_engine, false)) generic_snapshot_32 = url;
                cursor += url.size();
            }
        }
        if (line.size() < 4 || line.front() != '|') continue;
        std::vector<std::string> cells;
        size_t begin = 1;
        while (begin <= line.size()) {
            const size_t end = line.find('|', begin);
            if (end == std::string::npos) break;
            cells.push_back(line.substr(begin, end - begin));
            begin = end + 1;
        }
        if (cells.size() < 3) continue;
        RenoMod mod;
        mod.name = markdown_cell_text(cells[0]);
        if (mod.name.empty() || mod.name == "Name" || mod.name.front() == ':') continue;
        if (!generic_engine.empty()) {
            mod.generic = true;
            mod.engine = generic_engine;
            mod.snapshot_url = generic_snapshot_64;
            mod.snapshot_url_32 = generic_snapshot_32;
            const std::string status = cells.size() > 1 ? ascii_lower(cells[1]) : "";
            mod.construction = status.find("construction") != std::string::npos || status.find("🚧") != std::string::npos;
            mod.blocked = status.find("no_entry") != std::string::npos || status.find("⛔") != std::string::npos;
            mod.notes = cells.size() > 2 ? markdown_notes(cells[2]) : "";
            mod.settings = parse_reno_settings(mod.engine, mod.notes);
            if (!mod.snapshot_url.empty() || !mod.snapshot_url_32.empty()) mods.push_back(std::move(mod));
            continue;
        }
        const std::string links = cells[2];
        size_t cursor = 0;
        while ((cursor = links.find("](", cursor)) != std::string::npos) {
            cursor += 2;
            const size_t end = links.find(')', cursor);
            if (end == std::string::npos) break;
            const std::string url = links.substr(cursor, end - cursor);
            if (mod.snapshot_url.empty() && url.rfind("https://", 0) == 0 &&
                (url.find(".addon64") != std::string::npos || url.find(".addon32") != std::string::npos)) {
                mod.snapshot_url = url;
            } else if (mod.nexus_url.empty() && url.rfind("https://www.nexusmods.com/", 0) == 0 &&
                url.find("/mods/") != std::string::npos) {
                mod.nexus_url = url;
            } else if (mod.discord_url.empty() &&
                (url.rfind("https://discord.com/", 0) == 0 || url.rfind("https://discord.gg/", 0) == 0)) {
                mod.discord_url = url;
            }
            cursor = end + 1;
        }
        if (cells.size() > 3) {
            const std::string status = ascii_lower(cells[3]);
            mod.construction = status.find("construction") != std::string::npos || status.find("🚧") != std::string::npos;
            mod.blocked = status.find("no_entry") != std::string::npos || status.find("⛔") != std::string::npos;
        }
        if (!mod.snapshot_url.empty() || !mod.nexus_url.empty() || !mod.discord_url.empty()) mods.push_back(std::move(mod));
    }
    return mods;
}

std::vector<RenoMod> reno_catalog(bool force = false)
{
    // Serialize refreshes so concurrent game checks share one request and result.
    AcquireSRWLockExclusive(&g_reno_catalog_lock);
    const ULONGLONG now = GetTickCount64();
    const bool fresh = !g_reno_catalog_failed && !g_reno_catalog.empty() && now - g_reno_catalog_at < 15ull * 60ull * 1000ull;
    const bool retry_later = g_reno_catalog_failed && now - g_reno_catalog_attempt_at < 60ull * 1000ull;
    if (force || (!fresh && !retry_later)) {
        g_reno_catalog_attempt_at = now;
        const HttpResult response = http_get_host(L"raw.githubusercontent.com", L"/wiki/clshortfuse/renodx/Mods.md", "",
            4 * 1024 * 1024, 15000);
        auto parsed = response.error.empty() && response.status >= 200 && response.status < 300 ?
            parse_reno_catalog(response.body) : std::vector<RenoMod>{};
        g_reno_catalog_failed = parsed.empty();
        if (!parsed.empty()) {
            g_reno_catalog = std::move(parsed);
            g_reno_catalog_at = GetTickCount64();
            FILETIME timestamp{};
            GetSystemTimeAsFileTime(&timestamp);
            ULARGE_INTEGER ticks{};
            ticks.LowPart = timestamp.dwLowDateTime;
            ticks.HighPart = timestamp.dwHighDateTime;
            g_reno_catalog_updated_ms = (ticks.QuadPart - 116444736000000000ull) / 10000ull;
        }
    }
    const auto result = g_reno_catalog;
    ReleaseSRWLockExclusive(&g_reno_catalog_lock);
    return result;
}

bool find_reno_mod(const std::string& game_name, RenoMod& match)
{
    double best_specific = 0;
    double best_generic = 0;
    RenoMod generic_match;
    for (const auto& mod : reno_catalog()) {
        const double score = game_name_match_score(game_name, mod.name);
        if (!mod.generic && score > best_specific) {
            best_specific = score;
            match = mod;
        } else if (mod.generic && score > best_generic) {
            best_generic = score;
            generic_match = mod;
        }
    }
    if (best_specific >= 0.82) return !match.blocked;
    if (best_generic >= 0.82) {
        match = generic_match;
        return !match.blocked;
    }
    return false;
}

std::string reno_state_fields(const ResolvedApp& app)
{
    RenoMod mod;
    const bool available = !app.game_name.empty() && find_reno_mod(app.game_name, mod);
    const bool installed = available && reno_installation_state(app);
    const bool discord_only = available && mod.snapshot_url.empty() && mod.nexus_url.empty() && !mod.discord_url.empty();
    const std::string source = !available ? "" : mod.generic ? ("Generic " + mod.engine) : !mod.snapshot_url.empty() ? "Snapshot" :
        !mod.nexus_url.empty() ? "Nexus Mods" : "RenoDX Discord";
    return ",\"renoAvailable\":" + std::string(available ? "true" : "false") +
        ",\"renoName\":" + json_escape(available ? mod.name : "") +
        ",\"renoSource\":" + json_escape(source) +
        ",\"renoGeneric\":" + std::string(available && mod.generic ? "true" : "false") +
        ",\"renoEngine\":" + json_escape(available ? mod.engine : "") +
        ",\"renoNotes\":" + json_escape(available ? mod.notes : "") +
        ",\"renoConstruction\":" + std::string(available && mod.construction ? "true" : "false") +
        ",\"renoInstalled\":" + std::string(installed ? "true" : "false") +
        ",\"renoNeedsNexus\":" + std::string(available && mod.snapshot_url.empty() && !mod.nexus_url.empty() ? "true" : "false") +
        ",\"renoDiscordOnly\":" + std::string(discord_only ? "true" : "false") +
        ",\"showRenoButton\":true,\"showOptiScalerButton\":true";
}

std::string state_json(const ResolvedApp& app, bool exists, bool available, const std::string& text, bool error)
{
    const std::string tone = error ? "error" : ((text == "Downloaded." || text == "Removed.") ? "success" : "idle");
    return "{\"appId\":" + json_escape(app.app_id) + ",\"visibleAppId\":" + json_escape(app.visible_app_id) +
        ",\"gameName\":" + json_escape(app.game_name) +
        ",\"isDlc\":" + (app.is_dlc ? "true" : "false") + ",\"parentName\":" + json_escape(app.parent_name) +
        ",\"exists\":" + (exists ? "true" : "false") +
        ",\"available\":" + (available ? "true" : "false") + ",\"statusText\":" + json_escape(text) +
        ",\"statusTone\":" + json_escape(tone) + ",\"statusError\":" + (error ? "true" : "false") +
        ",\"controlsDisabled\":false" + reno_state_fields(app) + "}";
}

std::string dlc_status(const ResolvedApp& app)
{
    if (!app.is_dlc) return {};
    return "DLC detected: using base game " + app.app_id + (app.parent_name.empty() ? "" : " - " + app.parent_name);
}

std::string handle_state(const std::string& id, const ResolvedApp& app)
{
    const Config config = read_config();
    if (!config.error.empty()) return reply_error(id, "config.invalid", config.error);
    const bool exists = lua_exists(config, app.app_id);
    if (exists) return reply_ok(id, "storeState", state_json(app, true, true, dlc_status(app), false));

    const HttpResult response = http_get(L"/api/v1/status/" + widen(app.app_id), config.api_key);
    if (!response.error.empty()) return reply_error(id, "http.failed", response.error);
    if (response.status < 200 || response.status >= 300) return reply_error(id, "http.failed", http_error(response.status));

    yyjson_doc* document = yyjson_read(response.body.data(), response.body.size(), 0);
    if (!document) return reply_error(id, "http.invalidJson", "Hubcap returned invalid status data.");
    yyjson_val* root = yyjson_doc_get_root(document);
    const char* status = yyjson_get_str(yyjson_obj_get(root, "status"));
    const bool manifest = yyjson_get_bool(yyjson_obj_get(root, "manifest_file_exists"));
    const bool updating = yyjson_get_bool(yyjson_obj_get(root, "update_in_progress"));
    const bool available = status && _stricmp(status, "available") == 0 && manifest && !updating;
    yyjson_doc_free(document);
    const std::string status_text = app.is_dlc ? dlc_status(app) : (available ? "" : "Lua unavailable.");
    return reply_ok(id, "storeState", state_json(app, false, available, status_text, app.is_dlc ? false : !available));
}

long long json_integer(yyjson_val* root, const char* primary, const char* fallback)
{
    yyjson_val* value = yyjson_obj_get(root, primary);
    if (!value && fallback) value = yyjson_obj_get(root, fallback);
    return value && yyjson_is_num(value) ? yyjson_get_sint(value) : 0;
}

std::string handle_usage(const std::string& id)
{
    const Config config = read_config();
    if (!config.error.empty()) {
        const bool missing = config.error.find("Config file not found") != std::string::npos;
        const std::string label = missing ?
            "Hubcap config missing. Make sure Hubcaptool is installed." : "Invalid API Key";
        return reply_ok(id, "usage", "{\"error\":" + json_escape(config.error) +
            ",\"errorLabel\":" + json_escape(label) + "}");
    }
    const HttpResult response = http_get(L"/api/v1/user/stats", config.api_key);
    if (!response.error.empty()) return reply_ok(id, "usage", "{\"error\":" + json_escape(response.error) +
        ",\"errorLabel\":\"Stats Error\"}");
    if (response.status < 200 || response.status >= 300) {
        const bool invalid_key = response.status == 401 || response.status == 403;
        return reply_ok(id, "usage", "{\"error\":" + json_escape(http_error(response.status)) +
            ",\"errorLabel\":" + json_escape(invalid_key ? "Invalid API Key" : "Stats Error") + "}");
    }
    yyjson_doc* document = yyjson_read(response.body.data(), response.body.size(), 0);
    if (!document) return reply_error(id, "http.invalidJson", "Hubcap returned invalid usage data.");
    yyjson_val* root = yyjson_doc_get_root(document);
    const char* username = yyjson_get_str(yyjson_obj_get(root, "username"));
    const char* expiry = yyjson_get_str(yyjson_obj_get(root, "api_key_expires_at"));
    const long long daily_usage = json_integer(root, "daily_usage", "api_key_usage_count");
    const long long daily_limit = json_integer(root, "daily_limit", "role_daily_limit");
    const std::string state = "{\"username\":" + json_escape(username ? username : "") +
        ",\"dailyUsage\":" + std::to_string(daily_usage) +
        ",\"dailyLimit\":" + std::to_string(daily_limit) +
        ",\"apiKeyExpiresAt\":" + json_escape(expiry ? expiry : "") + "}";
    yyjson_doc_free(document);
    return reply_ok(id, "usage", state);
}

std::string handle_remove(const std::string& id, const ResolvedApp& app)
{
    const Config config = read_config();
    if (!config.error.empty()) return reply_error(id, "config.invalid", config.error);
    const std::string& app_id = app.app_id;
    const std::wstring lua = join(config.lua_dir, widen(app_id + ".lua"));
    if (!DeleteFileW(lua.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        return reply_error(id, "fs.removeFailed", "Could not remove the Lua file.");
    }
    const std::wstring root = module_root();
    DeleteFileW(join(root, L"config\\hubcapplugin-manifest-" + widen(app_id) + L".txt").c_str());
    DeleteFileW(join(root, L"depotcache\\.hubcapmanifest-" + widen(app_id)).c_str());
    return reply_ok(id, "storeState", state_json(app, false, true, app.is_dlc ? dlc_status(app) : "Removed.", false));
}

bool safe_bundle_name(const std::string& name)
{
    if (name.empty() || name.size() > 240 || name.front() == '/' || name.front() == '\\' || name.find(':') != std::string::npos) return false;
    std::string normalized = name;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::stringstream parts(normalized);
    std::string part;
    while (std::getline(parts, part, '/')) {
        if (part.empty() || part == "." || part == "..") return false;
    }
    return true;
}

std::string basename_of(std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    const size_t slash = name.find_last_of('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

bool manifest_name(const std::string& name)
{
    const std::string suffix = ".manifest";
    if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    const std::string stem = name.substr(0, name.size() - suffix.size());
    const size_t underscore = stem.find('_');
    if (underscore == std::string::npos || stem.find('_', underscore + 1) != std::string::npos) return false;
    return numeric_app_id(stem.substr(0, underscore)) && numeric_manifest_id(stem.substr(underscore + 1));
}

bool write_file_atomic(const std::wstring& target, const void* data, size_t size)
{
    const std::wstring temporary = target + L".hubcap-new";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const unsigned char* cursor = static_cast<const unsigned char*>(data);
    size_t remaining = size;
    bool ok = true;
    while (remaining) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

bool read_handle_text(HANDLE file, std::string& text)
{
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) return false;
    text.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < text.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(text.size() - offset, 1024 * 1024));
        DWORD read = 0;
        if (!ReadFile(file, text.data() + offset, chunk, &read, nullptr) || read != chunk) return false;
        offset += read;
    }
    return true;
}

bool write_handle_text(HANDLE file, const std::string& text)
{
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) return false;
    const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text.data());
    size_t remaining = text.size();
    while (remaining) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) return false;
        cursor += written;
        remaining -= written;
    }
    return SetEndOfFile(file) != FALSE && FlushFileBuffers(file) != FALSE;
}

bool mutate_text_file_in_place(
    const std::wstring& target,
    const std::function<void(std::string&)>& mutate,
    std::string* before,
    std::string* after)
{
    HANDLE file = CreateFileW(target.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::string original;
    bool ok = read_handle_text(file, original);
    std::string updated = original;
    if (ok) mutate(updated);
    if (before) *before = original;
    if (after) *after = updated;
    if (ok && updated != original) {
        ok = write_handle_text(file, updated);
        if (!ok) write_handle_text(file, original);
    }
    CloseHandle(file);
    return ok;
}

bool mutate_text_file_atomic(
    const std::wstring& target,
    const std::function<void(std::string&)>& mutate,
    std::string* before = nullptr,
    std::string* after = nullptr)
{
    bool sharing_blocked_replace = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE source = CreateFileW(
            target.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (source == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_SHARING_VIOLATION || GetLastError() == ERROR_LOCK_VIOLATION) {
                Sleep(25);
                continue;
            }
            return false;
        }

        std::string original;
        bool ok = read_handle_text(source, original);
        std::string updated = original;
        if (ok) mutate(updated);
        if (before) *before = original;
        if (after) *after = updated;
        if (!ok || updated == original) {
            CloseHandle(source);
            return ok;
        }

        const std::wstring temporary = target + L".hubcap-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetCurrentThreadId()) + L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
        HANDLE output = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE) {
            CloseHandle(source);
            return false;
        }
        const unsigned char* cursor = reinterpret_cast<const unsigned char*>(updated.data());
        size_t remaining = updated.size();
        while (ok && remaining) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1024 * 1024));
            DWORD written = 0;
            ok = WriteFile(output, cursor, chunk, &written, nullptr) != FALSE && written == chunk;
            cursor += written;
            remaining -= written;
        }
        if (ok) ok = FlushFileBuffers(output) != FALSE;
        CloseHandle(output);
        DWORD replace_error = ERROR_SUCCESS;
        if (ok) {
            ok = ReplaceFileW(target.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
            if (!ok) replace_error = GetLastError();
        }
        CloseHandle(source);
        if (!ok) DeleteFileW(temporary.c_str());
        if (!ok && (replace_error == ERROR_SHARING_VIOLATION || replace_error == ERROR_LOCK_VIOLATION)) {
            sharing_blocked_replace = true;
            Sleep(25);
            continue;
        }
        return ok;
    }
    if (!sharing_blocked_replace) return false;
    hubcap_runtime_log("Config atomic replacement remained sharing-locked; trying verified in-place update.");
    return mutate_text_file_in_place(target, mutate, before, after);
}

std::wstring local_app_data()
{
    const DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (needed <= 1) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), needed);
    if (!written || written >= needed) return {};
    value.resize(written);
    return value;
}

std::wstring launcher_settings_path()
{
    const std::wstring base = local_app_data();
    return base.empty() ? std::wstring() : join(base, L"HubcapUI\\settings.json");
}

std::wstring legacy_launcher_settings_path()
{
    const std::wstring base = local_app_data();
    return base.empty() ? std::wstring() : join(base, L"HubcapLauncher\\settings.json");
}

LauncherSettings read_launcher_settings()
{
    LauncherSettings settings;
    const std::wstring current_path = launcher_settings_path();
    std::wstring path = current_path;
    if (path.empty()) return settings;
    std::ifstream file(path.c_str(), std::ios::binary);
    bool legacy = false;
    if (!file) {
        path = legacy_launcher_settings_path();
        file.clear();
        file.open(path.c_str(), std::ios::binary);
        legacy = static_cast<bool>(file);
    }
    if (!file) return settings;
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    if (legacy) {
        const std::wstring directory = current_path.substr(0, current_path.find_last_of(L"\\/"));
        if ((CreateDirectoryW(directory.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) &&
            write_file_atomic(current_path, text.data(), text.size())) {
            DeleteFileW(path.c_str());
        }
    }
    yyjson_doc* document = yyjson_read(text.data(), text.size(), 0);
    if (!document) return settings;
    yyjson_val* root = yyjson_doc_get_root(document);
    yyjson_val* enabled = yyjson_obj_get(root, "CollectionSyncEnabled");
    yyjson_val* show_reno_button = yyjson_obj_get(root, "ShowRenoDXButton");
    if (!show_reno_button) show_reno_button = yyjson_obj_get(root, "ShowReno" "HDRButton");
    const char* name = yyjson_get_str(yyjson_obj_get(root, "CollectionName"));
    const char* nexus_api_key = yyjson_get_str(yyjson_obj_get(root, "NexusApiKey"));
    const char* optiscaler_gpu = yyjson_get_str(yyjson_obj_get(root, "OptiScalerGpu"));
    const char* optiscaler_upscaler = yyjson_get_str(yyjson_obj_get(root, "OptiScalerUpscaler"));
    yyjson_val* show_optiscaler_button = yyjson_obj_get(root, "ShowOptiScalerButton");
    settings.collection_sync_enabled = yyjson_get_bool(enabled);
    settings.show_reno_button = yyjson_get_bool(show_reno_button);
    settings.show_optiscaler_button = yyjson_get_bool(show_optiscaler_button);
    settings.collection_name = settings.collection_sync_enabled && name ? trim(name) : "";
    settings.nexus_api_key = nexus_api_key ? trim(nexus_api_key) : "";
    const std::string gpu = optiscaler_gpu ? trim(optiscaler_gpu) : "Auto";
    for (const char* option : {"Auto", "NVIDIA", "AMD", "Intel"}) {
        if (_stricmp(gpu.c_str(), option) == 0) { settings.optiscaler_gpu = option; break; }
    }
    const std::string upscaler = optiscaler_upscaler ? trim(optiscaler_upscaler) : "Auto";
    for (const char* option : {"Auto", "FSR31", "XeSS", "DLSS"}) {
        if (_stricmp(upscaler.c_str(), option) == 0) { settings.optiscaler_upscaler = option; break; }
    }
    yyjson_doc_free(document);
    return settings;
}

bool write_launcher_settings(bool collection_sync_enabled, const std::string& collection_name,
    const std::string& nexus_api_key = "", bool show_reno_button = false,
    const std::string& optiscaler_gpu = "Auto", const std::string& optiscaler_upscaler = "Auto",
    bool show_optiscaler_button = false)
{
    const std::wstring path = launcher_settings_path();
    if (path.empty()) return false;
    const std::wstring directory = path.substr(0, path.find_last_of(L"\\/"));
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const std::string normalized_name = collection_sync_enabled ? trim(collection_name) : "";
    const std::string text = "{\n  \"CollectionSyncEnabled\": " +
        std::string(collection_sync_enabled ? "true" : "false") +
        ",\n  \"CollectionName\": " + json_escape(normalized_name) +
        ",\n  \"NexusApiKey\": " + json_escape(trim(nexus_api_key)) +
        ",\n  \"ShowRenoDXButton\": " + std::string(show_reno_button ? "true" : "false") +
        ",\n  \"OptiScalerGpu\": " + json_escape(optiscaler_gpu) +
        ",\n  \"OptiScalerUpscaler\": " + json_escape(optiscaler_upscaler) +
        ",\n  \"ShowOptiScalerButton\": " + std::string(show_optiscaler_button ? "true" : "false") + "\n}\n";
    return write_file_atomic(path, text.data(), text.size());
}

bool equal_case_insensitive(const std::string& left, const std::string& right)
{
    const std::wstring wide_left = widen(left);
    const std::wstring wide_right = widen(right);
    if ((left.size() && wide_left.empty()) || (right.size() && wide_right.empty())) return false;
    return CompareStringOrdinal(
        wide_left.c_str(), static_cast<int>(wide_left.size()),
        wide_right.c_str(), static_cast<int>(wide_right.size()), TRUE) == CSTR_EQUAL;
}

std::string app_ids_json(const std::vector<std::string>& ids)
{
    std::string result = "[";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) result.push_back(',');
        result += json_escape(ids[i]);
    }
    result.push_back(']');
    return result;
}

std::string group_lua_config_json(const LauncherSettings& settings)
{
    return "{\"enabled\":" + std::string(settings.collection_sync_enabled ? "true" : "false") +
        ",\"name\":" + json_escape(settings.collection_name) +
        ",\"appIds\":" + app_ids_json(hubcap_service_lua_app_ids()) + "}";
}

std::string yaml_field(const std::string& text, const std::string& wanted)
{
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const size_t separator = line.find(':');
        if (separator == std::string::npos || trim(line.substr(0, separator)) != wanted) continue;
        return unquote(line.substr(separator + 1));
    }
    return {};
}

std::string yaml_quote(const std::string& value)
{
    std::string result = "\"";
    for (char c : value) {
        if (c == '\\' || c == '"') result.push_back('\\');
        result.push_back(c);
    }
    result.push_back('"');
    return result;
}

void set_yaml_field(std::string& text, const std::string& wanted, const std::string& value)
{
    const std::string replacement = wanted + ": " + yaml_quote(value);
    size_t line_begin = 0;
    while (line_begin <= text.size()) {
        const size_t line_end = text.find('\n', line_begin);
        const size_t content_end = line_end == std::string::npos ? text.size() : line_end;
        std::string line = text.substr(line_begin, content_end - line_begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t separator = line.find(':');
        if (separator != std::string::npos && trim(line.substr(0, separator)) == wanted) {
            text.replace(line_begin, content_end - line_begin, replacement);
            return;
        }
        if (line_end == std::string::npos) break;
        line_begin = line_end + 1;
    }
    if (!text.empty() && text.back() != '\n') text.push_back('\n');
    text += replacement + "\n";
}

struct YamlMapEntry {
    bool map_found = false;
    bool map_unsupported = false;
    bool entry_found = false;
    size_t map_line_begin = 0;
    size_t map_line_end = 0;
    size_t map_next_line = 0;
    size_t entry_begin = 0;
    size_t entry_end = 0;
    size_t entry_next_line = 0;
    std::string child_indent;
    std::string entry_indent;
    std::string value;
};

std::string yaml_line_ending(const std::string& text)
{
    return text.find("\r\n") == std::string::npos ? "\n" : "\r\n";
}

bool yaml_top_level_field(const std::string& line, const std::string& wanted, size_t& separator, std::string& value)
{
    if (line.empty() || std::isspace(static_cast<unsigned char>(line.front())) || line.front() == '#') return false;
    bool single = false;
    bool double_quote = false;
    bool escaped = false;
    separator = std::string::npos;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (double_quote && c == '\\') {
            escaped = true;
            continue;
        }
        if (!double_quote && c == '\'') single = !single;
        else if (!single && c == '"') double_quote = !double_quote;
        else if (!single && !double_quote && c == ':') {
            separator = i;
            break;
        }
    }
    if (separator == std::string::npos || unquote(trim(line.substr(0, separator))) != wanted) return false;
    value = trim(line.substr(separator + 1));
    if (!value.empty() && value.front() == '#') {
        value.clear();
        return true;
    }
    const size_t comment = value.find(" #");
    if (comment != std::string::npos) value.resize(comment);
    value = trim(value);
    return true;
}

bool normalize_fake_app_ids_layout(std::string& text, std::string& error)
{
    size_t line_begin = 0;
    while (line_begin <= text.size()) {
        const size_t newline = text.find('\n', line_begin);
        const size_t line_end = newline == std::string::npos ? text.size() : newline;
        std::string line = text.substr(line_begin, line_end - line_begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t separator = 0;
        std::string value;
        if (yaml_top_level_field(line, "FakeAppIds", separator, value)) {
            if (value.empty()) return true;
            if (value == "{}" || value == "null" || value == "~") {
                const std::string comment = line.substr(separator + 1).find('#') == std::string::npos ? "" :
                    " " + line.substr(separator + 1).substr(line.substr(separator + 1).find('#'));
                text.replace(line_begin, line_end - line_begin, line.substr(0, separator + 1) + comment);
                return true;
            }
            error = "FakeAppIds uses an unsupported inline YAML layout.";
            return false;
        }
        if (newline == std::string::npos) break;
        line_begin = newline + 1;
    }
    return true;
}

YamlMapEntry find_yaml_map_entry(const std::string& text, const std::string& map_name, const std::string& key)
{
    YamlMapEntry result;
    size_t line_begin = 0;
    while (line_begin <= text.size()) {
        const size_t newline = text.find('\n', line_begin);
        const size_t line_end = newline == std::string::npos ? text.size() : newline;
        std::string line = text.substr(line_begin, line_end - line_begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string content = trim(line);
        if (!result.map_found) {
            size_t separator = 0;
            std::string value;
            if (yaml_top_level_field(line, map_name, separator, value)) {
                result.map_found = true;
                result.map_unsupported = !value.empty();
                result.map_line_begin = line_begin;
                result.map_line_end = line_end;
                result.map_next_line = newline == std::string::npos ? text.size() : newline + 1;
                if (result.map_unsupported) break;
            }
        } else {
            const size_t first = line.find_first_not_of(" \t");
            if (first != std::string::npos && line[first] != '#' && first == 0) break;
            if (first != std::string::npos && line[first] != '#') {
                if (result.child_indent.empty()) result.child_indent = line.substr(0, first);
                const size_t separator = line.find(':', first);
                if (separator != std::string::npos && unquote(trim(line.substr(first, separator - first))) == key) {
                    result.entry_found = true;
                    result.entry_begin = line_begin;
                    result.entry_end = line_end;
                    result.entry_next_line = newline == std::string::npos ? text.size() : newline + 1;
                    result.entry_indent = line.substr(0, first);
                    std::string value = trim(line.substr(separator + 1));
                    const size_t comment = value.find(" #");
                    if (comment != std::string::npos) value.resize(comment);
                    result.value = unquote(value);
                    break;
                }
            }
        }
        if (newline == std::string::npos) break;
        line_begin = newline + 1;
    }
    return result;
}

bool online_fix_enabled(const std::string& text, const std::string& app_id)
{
    const YamlMapEntry entry = find_yaml_map_entry(text, "FakeAppIds", app_id);
    return entry.entry_found && trim(entry.value) == "480";
}

std::vector<std::string> multiplayer_mode_app_ids(const std::string& text)
{
    std::vector<std::string> ids;
    bool in_map = false;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string content = trim(line);
        if (!in_map) {
            size_t separator = 0;
            std::string value;
            if (yaml_top_level_field(line, "FakeAppIds", separator, value) && value.empty()) in_map = true;
            continue;
        }
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        if (first == 0) break;
        const size_t separator = line.find(':', first);
        if (separator == std::string::npos) continue;
        const std::string app_id = unquote(trim(line.substr(first, separator - first)));
        std::string value = trim(line.substr(separator + 1));
        const size_t comment = value.find(" #");
        if (comment != std::string::npos) value.resize(comment);
        if (app_id != "0" && numeric_app_id(app_id) && unquote(value) == "480") ids.push_back(app_id);
    }
    std::sort(ids.begin(), ids.end(), [](const std::string& left, const std::string& right) {
        return std::stoull(left) < std::stoull(right);
    });
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

bool set_online_fix(
    std::string& text,
    const std::string& app_id,
    bool enabled,
    const std::string& restore_value,
    std::string& error)
{
    if (!normalize_fake_app_ids_layout(text, error)) return false;
    const std::string newline = yaml_line_ending(text);
    const YamlMapEntry entry = find_yaml_map_entry(text, "FakeAppIds", app_id);
    if (entry.map_unsupported) {
        error = "FakeAppIds uses an unsupported YAML layout.";
        return false;
    }
    const std::string indent = entry.entry_found ? entry.entry_indent :
        (entry.child_indent.empty() ? "  " : entry.child_indent);
    const std::string mapping = indent + app_id + ": " + (enabled ? "480" : restore_value);
    if (enabled) {
        if (entry.entry_found) {
            text.replace(entry.entry_begin, entry.entry_end - entry.entry_begin, mapping);
        } else if (entry.map_found) {
            const std::string prefix = entry.map_line_end == text.size() ? newline : "";
            text.insert(entry.map_next_line, prefix + mapping + newline);
        } else {
            if (!text.empty() && text.back() != '\n') text += newline;
            text += "FakeAppIds:" + newline + mapping + newline;
        }
    } else if (entry.entry_found) {
        if (restore_value.empty()) text.erase(entry.entry_begin, entry.entry_next_line - entry.entry_begin);
        else text.replace(entry.entry_begin, entry.entry_end - entry.entry_begin, mapping);
    }
    return true;
}

bool read_text_file(const std::wstring& path, std::string& text)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) return false;
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return static_cast<bool>(file) || file.eof();
}

std::string settings_state(
    const std::string& status = "",
    const std::string& tone = "idle",
    bool save_failed = false,
    bool restart_required = false,
    const std::string& lua_override = "",
    const std::string& api_key_override = "",
    long long settings_anchor_right = 0,
    long long settings_anchor_screen_left = 0,
    long long settings_anchor_screen_top = 0,
    long long settings_anchor_client_left = 0,
    long long settings_anchor_client_top = 0,
    bool settings_prefer_source = false,
    const std::string& settings_surface = "",
    int collection_sync_override = -1,
    const std::string& collection_name_override = "",
    const std::string& nexus_api_key_override = "",
    bool nexus_api_key_override_valid = false,
    int show_reno_button_override = -1,
    const std::string& optiscaler_gpu_override = "",
    const std::string& optiscaler_upscaler_override = "",
    int show_optiscaler_button_override = -1)
{
    LauncherSettings launcher_settings = read_launcher_settings();
    if (collection_sync_override >= 0) {
        launcher_settings.collection_sync_enabled = collection_sync_override != 0;
        launcher_settings.collection_name = launcher_settings.collection_sync_enabled ? trim(collection_name_override) : "";
    }
    if (nexus_api_key_override_valid) launcher_settings.nexus_api_key = trim(nexus_api_key_override);
    if (show_reno_button_override >= 0) launcher_settings.show_reno_button = show_reno_button_override != 0;
    if (!optiscaler_gpu_override.empty()) launcher_settings.optiscaler_gpu = optiscaler_gpu_override;
    if (!optiscaler_upscaler_override.empty()) launcher_settings.optiscaler_upscaler = optiscaler_upscaler_override;
    if (show_optiscaler_button_override >= 0) launcher_settings.show_optiscaler_button = show_optiscaler_button_override != 0;
    const std::string collection_names = launcher_settings.collection_name.empty() ?
        "[]" : "[" + json_escape(launcher_settings.collection_name) + "]";
    const std::string group_fields =
        ",\"collectionSyncEnabled\":" + std::string(launcher_settings.collection_sync_enabled ? "true" : "false") +
        ",\"collectionName\":" + json_escape(launcher_settings.collection_name) +
        ",\"collectionNames\":" + collection_names +
        ",\"nexusApiKey\":" + json_escape(launcher_settings.nexus_api_key) +
        ",\"showRenoButton\":" + std::string(launcher_settings.show_reno_button ? "true" : "false") +
        ",\"showOptiScalerButton\":" + std::string(launcher_settings.show_optiscaler_button ? "true" : "false") +
        ",\"optiscalerGpu\":" + json_escape(launcher_settings.optiscaler_gpu) +
        ",\"optiscalerUpscaler\":" + json_escape(launcher_settings.optiscaler_upscaler) +
        ",\"detectedGpuVendor\":" + json_escape(detected_gpu_vendor());
    const std::wstring path = join(module_root(), L"config\\hubcaptools\\config.yaml");
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) {
        return "{\"configMissing\":true,\"luaDir\":\"\",\"apiKey\":\"\",\"statusText\":" +
            json_escape(status.empty() ? "Config file not found. Make sure HubcapTools is installed." : status) +
            ",\"statusTone\":\"error\",\"saveFailed\":" + (save_failed ? "true" : "false") +
            ",\"restartRequired\":false" +
            ",\"settingsAnchorRight\":" + std::to_string(settings_anchor_right) +
            ",\"settingsAnchorScreenLeft\":" + std::to_string(settings_anchor_screen_left) +
            ",\"settingsAnchorScreenTop\":" + std::to_string(settings_anchor_screen_top) +
            ",\"settingsAnchorClientLeft\":" + std::to_string(settings_anchor_client_left) +
            ",\"settingsAnchorClientTop\":" + std::to_string(settings_anchor_client_top) +
            ",\"settingsPreferSource\":" + (settings_prefer_source ? "true" : "false") +
            ",\"settingsSurface\":" + json_escape(settings_surface) + group_fields + "}";
    }
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string lua_dir = lua_override.empty() ? yaml_field(text, "HubcapLuaDir") : lua_override;
    if (lua_dir.empty()) lua_dir = narrow(join(module_root(), L"config\\hubcap-lua"));
    const std::string api_key = api_key_override.empty() ? yaml_field(text, "HubcapApiKey") : api_key_override;
    return "{\"configMissing\":false,\"luaDir\":" + json_escape(lua_dir) + ",\"apiKey\":" +
        json_escape(api_key) + ",\"statusText\":" + json_escape(status) +
        ",\"statusTone\":" + json_escape(tone) + ",\"saveFailed\":" + (save_failed ? "true" : "false") +
        ",\"restartRequired\":" + (restart_required ? "true" : "false") +
        ",\"settingsAnchorRight\":" + std::to_string(settings_anchor_right) +
        ",\"settingsAnchorScreenLeft\":" + std::to_string(settings_anchor_screen_left) +
        ",\"settingsAnchorScreenTop\":" + std::to_string(settings_anchor_screen_top) +
        ",\"settingsAnchorClientLeft\":" + std::to_string(settings_anchor_client_left) +
        ",\"settingsAnchorClientTop\":" + std::to_string(settings_anchor_client_top) +
        ",\"settingsPreferSource\":" + (settings_prefer_source ? "true" : "false") +
        ",\"settingsSurface\":" + json_escape(settings_surface) + group_fields + "}";
}

std::string handle_settings(
    const std::string& id,
    long long settings_anchor_right,
    long long settings_anchor_screen_left,
    long long settings_anchor_screen_top,
    long long settings_anchor_client_left,
    long long settings_anchor_client_top,
    bool settings_prefer_source,
    const std::string& settings_surface)
{
    return reply_ok(id, "settings", settings_state(
        "", "idle", false, false, "", "", settings_anchor_right,
        settings_anchor_screen_left, settings_anchor_screen_top,
        settings_anchor_client_left, settings_anchor_client_top,
        settings_prefer_source, settings_surface));
}

std::string validate_api_key(const std::string& api_key)
{
    const HttpResult response = http_get(L"/api/v1/user/stats", api_key, 1024 * 1024, 15000);
    if (!response.error.empty()) return "Could not validate API Key: " + response.error;
    if (response.status == 401 || response.status == 403) {
        return "API Key is invalid (HTTP " + std::to_string(response.status) + ").";
    }
    if (response.status < 200 || response.status >= 300) return "Could not validate API Key: " + http_error(response.status);
    yyjson_doc* document = yyjson_read(response.body.data(), response.body.size(), 0);
    if (!document) return "Could not validate API Key: Hubcap returned invalid data.";
    yyjson_doc_free(document);
    return {};
}

std::string validate_nexus_api_key(const std::string& api_key)
{
    if (api_key.empty()) return {};
    const HttpResult response = http_get_host(L"api.nexusmods.com", L"/v1/users/validate.json", "",
        1024 * 1024, 15000, L"apikey: " + widen(api_key));
    if (!response.error.empty()) return "Could not validate Nexus API key: " + response.error;
    if (response.status == 401 || response.status == 403) return "Nexus API key is invalid or unauthorized.";
    if (response.status < 200 || response.status >= 300) {
        return "Could not validate Nexus API key (HTTP " + std::to_string(response.status) + ").";
    }
    return {};
}

std::string handle_validate_api_key(const std::string& id, const std::string& api_key)
{
    if (api_key.empty() || api_key.size() > 4096 || api_key.find_first_of("\r\n") != std::string::npos) {
        return reply_ok(id, "apiKeyStatus", "{\"valid\":false,\"statusText\":\"Enter a valid API Key.\"}");
    }
    const std::string error = validate_api_key(api_key);
    if (!error.empty()) {
        return reply_ok(id, "apiKeyStatus", "{\"valid\":false,\"statusText\":" + json_escape(error) + "}");
    }
    return reply_ok(id, "apiKeyStatus", "{\"valid\":true,\"statusText\":\"API Key is valid.\"}");
}

std::string handle_save_settings(
    const std::string& id,
    const std::string& lua_dir,
    const std::string& api_key,
    const std::string& nexus_api_key,
    bool show_reno_button,
    bool show_reno_button_valid,
    const std::string& optiscaler_gpu,
    const std::string& optiscaler_upscaler,
    bool show_optiscaler_button,
    bool show_optiscaler_button_valid,
    bool collection_sync_enabled,
    const std::string& collection_name,
    bool collection_sync_valid)
{
    const std::string normalized_collection_name = collection_sync_enabled ? trim(collection_name) : "";
    const auto draft_error = [&](const std::string& message) {
        return reply_ok(id, "settings", settings_state(
            message, "error", true, false, lua_dir, api_key, 0, 0, 0, 0, 0, false, "",
            collection_sync_enabled ? 1 : 0, normalized_collection_name, nexus_api_key, true,
            show_reno_button ? 1 : 0, optiscaler_gpu, optiscaler_upscaler,
            show_optiscaler_button ? 1 : 0));
    };
    if (!collection_sync_valid) return draft_error("Group Lua setting was invalid.");
    if (!show_reno_button_valid) return draft_error("RenoDX setting was invalid.");
    if (!show_optiscaler_button_valid) return draft_error("OptiScaler setting was invalid.");
    if (optiscaler_gpu != "Auto" && optiscaler_gpu != "NVIDIA" &&
        optiscaler_gpu != "AMD" && optiscaler_gpu != "Intel") {
        return draft_error("OptiScaler GPU setting was invalid.");
    }
    if (optiscaler_upscaler != "Auto" && optiscaler_upscaler != "FSR31" &&
        optiscaler_upscaler != "XeSS" && optiscaler_upscaler != "DLSS") {
        return draft_error("OptiScaler upscaler setting was invalid.");
    }
    if (collection_sync_enabled && normalized_collection_name.empty()) {
        return draft_error("Choose a collection for Group Lua.");
    }
    if (lua_dir.empty() || lua_dir.size() > 1024 || lua_dir.find_first_of("\r\n") != std::string::npos) {
        return draft_error("Enter a valid Lua Folder.");
    }
    if (api_key.empty() || api_key.size() > 4096 || api_key.find_first_of("\r\n") != std::string::npos) {
        return draft_error("Enter a valid API Key.");
    }
    if (nexus_api_key.size() > 4096 || nexus_api_key.find_first_of("\r\n") != std::string::npos) {
        return draft_error("Enter a valid Nexus API Key.");
    }
    const std::wstring path = join(module_root(), L"config\\hubcaptools\\config.yaml");
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) return draft_error("Config file not found. Make sure HubcapTools is installed.");
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    const std::string old_lua_dir = yaml_field(text, "HubcapLuaDir");
    const std::string old_api_key = yaml_field(text, "HubcapApiKey");
    const bool lua_changed = _wcsicmp(
        normalize_directory(widen(old_lua_dir)).c_str(),
        normalize_directory(widen(lua_dir)).c_str()) != 0;
    if (api_key != old_api_key) {
        const std::string validation_error = validate_api_key(api_key);
        if (!validation_error.empty()) {
            return draft_error(validation_error);
        }
    }
    if (lua_changed || api_key != old_api_key) {
        if (!mutate_text_file_atomic(path, [&](std::string& latest) {
                set_yaml_field(latest, "HubcapLuaDir", lua_dir);
                set_yaml_field(latest, "HubcapApiKey", api_key);
            })) {
            return draft_error("Could not save Hubcap settings.");
        }
    }
    const LauncherSettings old_launcher_settings = read_launcher_settings();
    if (nexus_api_key != old_launcher_settings.nexus_api_key) {
        const std::string nexus_validation_error = validate_nexus_api_key(nexus_api_key);
        if (!nexus_validation_error.empty()) return draft_error(nexus_validation_error);
    }
    if (!write_launcher_settings(collection_sync_enabled, normalized_collection_name, nexus_api_key,
            show_reno_button, optiscaler_gpu, optiscaler_upscaler, show_optiscaler_button)) {
        return draft_error("Could not save Hubcap settings.");
    }
    if (lua_changed) hubcap_service_notify_lua_config_changed();
    return reply_ok(id, "settings", settings_state(
        lua_changed ? "Saved. Restart Steam to apply the Lua Folder." : "Saved.",
        "success",
        false,
        lua_changed));
}

std::string handle_group_lua_config(const std::string& id)
{
    return reply_ok(id, "groupLuaConfig", group_lua_config_json(read_launcher_settings()));
}

std::string handle_disable_group_lua_if_collection(const std::string& id, const std::string& collection_name)
{
    LauncherSettings settings = read_launcher_settings();
    if (settings.collection_sync_enabled && !trim(collection_name).empty() &&
        equal_case_insensitive(settings.collection_name, trim(collection_name))) {
        if (!write_launcher_settings(false, "", settings.nexus_api_key, settings.show_reno_button,
                settings.optiscaler_gpu, settings.optiscaler_upscaler, settings.show_optiscaler_button)) {
            return reply_error(id, "settings.saveFailed", "Could not save Group Lua settings.");
        }
        settings = {};
    }
    return reply_ok(id, "groupLuaConfig", group_lua_config_json(settings));
}

std::string handle_restart_steam(const std::string& id)
{
    const std::wstring root = module_root();
    const std::wstring steam = join(root, L"steam.exe");
    if (GetFileAttributesW(steam.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return reply_error(id, "settings.restartFailed", "Could not find steam.exe.");
    }

    std::wstring escaped = steam;
    size_t cursor = 0;
    while ((cursor = escaped.find(L'\'', cursor)) != std::wstring::npos) {
        escaped.insert(cursor, 1, L'\'');
        cursor += 2;
    }
    std::wstring command = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -Command \"$p=Get-Process steam -ErrorAction SilentlyContinue; if($p){$p|Wait-Process}; Start-Process -FilePath '" + escaped + L"'\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process)) {
        return reply_error(id, "settings.restartFailed", "Could not start the Steam restart helper.");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    const HINSTANCE shutdown = ShellExecuteW(nullptr, L"open", steam.c_str(), L"-shutdown", root.c_str(), SW_HIDE);
    if (reinterpret_cast<INT_PTR>(shutdown) <= 32) {
        return reply_error(id, "settings.restartFailed", "Could not request Steam shutdown.");
    }
    return reply_ok(id, "steamRestarting", "{\"statusText\":\"Restarting Steam...\"}");
}

std::string handle_select_lua_folder(const std::string& id, const std::string& requested_initial)
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog,
        reinterpret_cast<void**>(&dialog));
    if (FAILED(result) || !dialog) {
        if (SUCCEEDED(initialized)) CoUninitialize();
        return reply_error(id, "settings.folderPickerFailed", "Could not open the folder picker.");
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Select Hubcap Lua Folder");
    std::wstring initial = widen(requested_initial);
    std::replace(initial.begin(), initial.end(), L'/', L'\\');
    if (!initial.empty()) {
        const DWORD needed = ExpandEnvironmentStringsW(initial.c_str(), nullptr, 0);
        if (needed > 1) {
            std::wstring expanded(needed, L'\0');
            ExpandEnvironmentStringsW(initial.c_str(), expanded.data(), needed);
            expanded.resize(wcslen(expanded.c_str()));
            initial = std::move(expanded);
        }
        const DWORD attributes = GetFileAttributesW(initial.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            IShellItem* initial_item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_IShellItem,
                reinterpret_cast<void**>(&initial_item))) && initial_item) {
                dialog->SetFolder(initial_item);
                initial_item->Release();
            }
        }
    }
    HWND owner = GetForegroundWindow();
    result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        dialog->Release();
        if (SUCCEEDED(initialized)) CoUninitialize();
        return reply_ok(id, "folderSelected", "{\"cancelled\":true}");
    }
    IShellItem* item = nullptr;
    PWSTR selected = nullptr;
    if (SUCCEEDED(result)) result = dialog->GetResult(&item);
    if (SUCCEEDED(result) && item) result = item->GetDisplayName(SIGDN_FILESYSPATH, &selected);
    std::string path = selected ? narrow(selected) : "";
    if (selected) CoTaskMemFree(selected);
    if (item) item->Release();
    dialog->Release();
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (FAILED(result) || path.empty()) return reply_error(id, "settings.folderPickerFailed", "Could not select that folder.");
    return reply_ok(id, "folderSelected", "{\"cancelled\":false,\"luaDir\":" + json_escape(path) + "}");
}

std::string handle_copy_api_key(const std::string& id)
{
    const Config config = read_config();
    if (config.api_key.empty()) return reply_error(id, "settings.apiKeyMissing", "API Key is empty.");
    const std::wstring key = widen(config.api_key);
    if (!OpenClipboard(nullptr)) return reply_error(id, "settings.clipboardFailed", "Could not open the clipboard.");
    EmptyClipboard();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (key.size() + 1) * sizeof(wchar_t));
    if (!memory) {
        CloseClipboard();
        return reply_error(id, "settings.clipboardFailed", "Could not copy the API Key.");
    }
    void* target = GlobalLock(memory);
    std::memcpy(target, key.c_str(), (key.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return reply_error(id, "settings.clipboardFailed", "Could not copy the API Key.");
    }
    CloseClipboard();
    return reply_ok(id, "settingsNotice", "{\"statusText\":\"API Key copied.\",\"statusTone\":\"success\"}");
}

bool window_uses_executable(HWND window, const std::wstring& executable)
{
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return false;
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (!process_id) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return false;
    wchar_t path[32768] = {};
    DWORD path_size = ARRAYSIZE(path);
    const bool matched = QueryFullProcessImageNameW(process, 0, path, &path_size) &&
        CompareStringOrdinal(path, -1, executable.c_str(), -1, TRUE) == CSTR_EQUAL;
    CloseHandle(process);
    return matched;
}

struct BrowserWindowSearch {
    const std::wstring* executable;
    const std::set<HWND>* existing;
    std::set<HWND>* collected;
    HWND found = nullptr;
};

BOOL CALLBACK find_browser_window(HWND window, LPARAM value)
{
    auto* search = reinterpret_cast<BrowserWindowSearch*>(value);
    if (!window_uses_executable(window, *search->executable)) return TRUE;
    if (search->collected) search->collected->insert(window);
    if (search->existing && search->existing->find(window) == search->existing->end()) {
        search->found = window;
        return FALSE;
    }
    return TRUE;
}

void focus_browser_window(HWND window, const RECT& work_area)
{
    if (!window || !IsWindow(window)) return;
    const DWORD current_thread = GetCurrentThreadId();
    const DWORD foreground_thread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD browser_thread = GetWindowThreadProcessId(window, nullptr);
    const bool attached_foreground = foreground_thread && foreground_thread != current_thread &&
        AttachThreadInput(current_thread, foreground_thread, TRUE);
    const bool attached_browser = browser_thread && browser_thread != current_thread && browser_thread != foreground_thread &&
        AttachThreadInput(current_thread, browser_thread, TRUE);
    const int work_width = std::max(1L, work_area.right - work_area.left);
    const int work_height = std::max(1L, work_area.bottom - work_area.top);
    const int width = std::min(work_width, std::max(800, work_width * 4 / 5));
    const int height = std::min(work_height, std::max(600, work_height * 4 / 5));
    const int left = work_area.left + (work_width - width) / 2;
    const int top = work_area.top + (work_height - height) / 2;
    WINDOWPLACEMENT placement = {sizeof(placement)};
    if (GetWindowPlacement(window, &placement)) {
        placement.flags = 0;
        placement.showCmd = SW_SHOWNORMAL;
        placement.rcNormalPosition = {left, top, left + width, top + height};
        SetWindowPlacement(window, &placement);
    }
    SetWindowPos(window, HWND_TOP, left, top, width, height, SWP_SHOWWINDOW);
    BringWindowToTop(window);
    const bool focused = SetForegroundWindow(window) != FALSE;
    if (attached_browser) AttachThreadInput(current_thread, browser_thread, FALSE);
    if (attached_foreground) AttachThreadInput(current_thread, foreground_thread, FALSE);
    if (!focused) {
        FLASHWINFO flash = {sizeof(flash), window, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
        FlashWindowEx(&flash);
    }
}

void focus_new_browser_window(std::wstring executable, std::set<HWND> existing, RECT work_area)
{
    for (int attempt = 0; attempt < 40; ++attempt) {
        BrowserWindowSearch search = {&executable, &existing, nullptr};
        EnumWindows(find_browser_window, reinterpret_cast<LPARAM>(&search));
        if (search.found) {
            focus_browser_window(search.found, work_area);
            return;
        }
        Sleep(100);
    }
}

std::string handle_open_api_keys(const std::string& id)
{
    constexpr wchar_t url[] = L"https://hubcapmanifest.com/api-keys/stats";
    AllowSetForegroundWindow(ASFW_ANY);

    wchar_t browser_path[32768] = {};
    DWORD browser_path_size = ARRAYSIZE(browser_path);
    const HRESULT association = AssocQueryStringW(
        ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L"https", L"open", browser_path, &browser_path_size);
    bool launched = false;
    if (SUCCEEDED(association) && browser_path[0]) {
        std::wstring browser_name = browser_path;
        const size_t separator = browser_name.find_last_of(L"\\/");
        if (separator != std::wstring::npos) browser_name.erase(0, separator + 1);
        std::transform(browser_name.begin(), browser_name.end(), browser_name.begin(), towlower);
        const bool firefox = browser_name.find(L"firefox") != std::wstring::npos;
        const bool chromium = browser_name.find(L"chrome") != std::wstring::npos ||
            browser_name.find(L"msedge") != std::wstring::npos || browser_name.find(L"brave") != std::wstring::npos ||
            browser_name.find(L"opera") != std::wstring::npos || browser_name.find(L"vivaldi") != std::wstring::npos ||
            browser_name.find(L"chromium") != std::wstring::npos || browser_name.find(L"arc") != std::wstring::npos;
        const bool new_window_supported = firefox || chromium;
        if (new_window_supported) {
            std::set<HWND> existing_windows;
            std::wstring browser_executable = browser_path;
            BrowserWindowSearch before = {&browser_executable, nullptr, &existing_windows};
            EnumWindows(find_browser_window, reinterpret_cast<LPARAM>(&before));
            RECT work_area = {};
            MONITORINFO monitor_info = {sizeof(monitor_info)};
            const HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
            if (monitor && GetMonitorInfoW(monitor, &monitor_info)) work_area = monitor_info.rcWork;
            else SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
            const int work_width = std::max(1L, work_area.right - work_area.left);
            const int work_height = std::max(1L, work_area.bottom - work_area.top);
            const int width = std::min(work_width, std::max(800, work_width * 4 / 5));
            const int height = std::min(work_height, std::max(600, work_height * 4 / 5));
            const int left = work_area.left + (work_width - width) / 2;
            const int top = work_area.top + (work_height - height) / 2;
            const std::wstring arguments = firefox
                ? std::wstring(L"-new-window \"") + url + L"\""
                : L"--new-window --window-position=" + std::to_wstring(left) + L"," + std::to_wstring(top) +
                    L" --window-size=" + std::to_wstring(width) + L"," + std::to_wstring(height) + L" \"" + url + L"\"";
            SHELLEXECUTEINFOW info = {};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_NOCLOSEPROCESS;
            info.hwnd = GetForegroundWindow();
            info.lpVerb = L"open";
            info.lpFile = browser_path;
            info.lpParameters = arguments.c_str();
            info.nShow = SW_SHOWNORMAL;
            launched = ShellExecuteExW(&info) != FALSE;
            if (info.hProcess) {
                AllowSetForegroundWindow(GetProcessId(info.hProcess));
                WaitForInputIdle(info.hProcess, 2000);
                CloseHandle(info.hProcess);
            }
            if (launched) std::thread(focus_new_browser_window, std::move(browser_executable), std::move(existing_windows), work_area).detach();
        }
    }
    if (!launched) {
        const HINSTANCE result = ShellExecuteW(GetForegroundWindow(), L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        launched = reinterpret_cast<INT_PTR>(result) > 32;
    }
    if (!launched) {
        return reply_error(id, "settings.apiKeyPageFailed", "Could not open the API key page.");
    }
    return reply_ok(id, "externalOpened", "{}");
}

struct BundleFile {
    std::string name;
    std::vector<unsigned char> bytes;
    bool lua = false;
};

std::string sha256_bytes(const void* data, size_t size)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, hash_size = 0, bytes = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &bytes, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &bytes, 0) < 0) return {};
    std::vector<unsigned char> object(object_size), digest(hash_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0 ||
        (size && BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) < 0) ||
        BCryptFinishHash(hash, digest.data(), hash_size, 0) < 0) return {};
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const auto byte : digest) { result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]); }
    return result;
}

std::string sha256_file(const std::wstring& path)
{
    std::string bytes;
    return read_text_file(path, bytes) ? sha256_bytes(bytes.data(), bytes.size()) : std::string();
}

std::vector<int> version_parts(std::string value)
{
    if (value.size() > 64) return {};
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) value.erase(value.begin());
    const size_t suffix = value.find_first_of("-+");
    if (suffix != std::string::npos) value.resize(suffix);
    std::vector<int> parts;
    std::istringstream stream(value);
    std::string part;
    while (std::getline(stream, part, '.')) {
        if (part.empty() || part.size() > 9 || parts.size() >= 4 ||
            !std::all_of(part.begin(), part.end(), [](unsigned char value) { return std::isdigit(value) != 0; })) return {};
        unsigned int parsed = 0;
        for (const char digit : part) {
            parsed = parsed * 10u + static_cast<unsigned int>(digit - '0');
            if (parsed > 999999999u) return {};
        }
        parts.push_back(static_cast<int>(parsed));
    }
    return parts;
}

bool version_newer(const std::string& latest, const std::string& current)
{
    auto left = version_parts(latest), right = version_parts(current);
    if (left.empty() || right.empty()) return false;
    left.resize(std::max(left.size(), right.size())); right.resize(left.size());
    return left > right;
}

bool stable_release_tag(const std::string& value)
{
    if (value.size() < 6 || value.size() > 32 || value.front() != 'v' ||
        std::count(value.begin(), value.end(), '.') != 2) return false;
    if (!std::all_of(value.begin() + 1, value.end(), [](unsigned char character) {
        return std::isdigit(character) != 0 || character == '.';
    })) return false;
    return version_parts(value).size() == 3;
}

bool sha256_digest(const std::string& value)
{
    return value.size() == 71 && value.rfind("sha256:", 0) == 0 &&
        std::all_of(value.begin() + 7, value.end(), [](unsigned char character) { return std::isxdigit(character) != 0; });
}

bool valid_amd64_dll(const std::string& bytes)
{
    if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    IMAGE_DOS_HEADER dos{}; std::memcpy(&dos, bytes.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        static_cast<size_t>(dos.e_lfanew) > bytes.size() - sizeof(IMAGE_NT_HEADERS64)) return false;
    IMAGE_NT_HEADERS64 headers{}; std::memcpy(&headers, bytes.data() + dos.e_lfanew, sizeof(headers));
    return headers.Signature == IMAGE_NT_SIGNATURE && headers.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        (headers.FileHeader.Characteristics & IMAGE_FILE_DLL) != 0 && headers.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
        headers.FileHeader.NumberOfSections > 0 && headers.FileHeader.NumberOfSections <= 96;
}

std::string update_state_json()
{
    AcquireSRWLockShared(&g_update_lock);
    const UpdateState state = g_update_state;
    ReleaseSRWLockShared(&g_update_lock);
    return "{\"phase\":" + json_escape(state.phase) + ",\"currentVersion\":" + json_escape(HUBCAP_UI_VERSION) +
        ",\"latestVersion\":" + json_escape(state.latest_version) + ",\"releaseName\":" + json_escape(state.release_name) +
        ",\"releaseNotes\":" + json_escape(state.release_notes) + ",\"bootPromptPending\":" +
        (state.boot_prompt_pending ? "true" : "false") + ",\"manualPromptPending\":" +
        (state.manual_prompt_pending ? "true" : "false") + ",\"deferred\":" + (state.deferred ? "true" : "false") +
        ",\"error\":" + json_escape(state.error) + ",\"retryAction\":" + json_escape(state.retry_action) + "}";
}

void set_update_error(const std::string& message, const std::string& retry_action)
{
    AcquireSRWLockExclusive(&g_update_lock);
    g_update_state.phase = "error"; g_update_state.error = message; g_update_state.retry_action = retry_action;
    ReleaseSRWLockExclusive(&g_update_lock);
}

void consume_update_result()
{
    AcquireSRWLockShared(&g_update_lock);
    const bool restarting = g_update_state.phase == "restarting";
    ReleaseSRWLockShared(&g_update_lock);
    const std::wstring path = join(local_app_data(), L"HubcapUI\\update-result.json");
    std::string json;
    if (!read_text_file(path, json)) return;
    DeleteFileW(path.c_str());
    if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xEF &&
        static_cast<unsigned char>(json[1]) == 0xBB && static_cast<unsigned char>(json[2]) == 0xBF) json.erase(0, 3);
    yyjson_doc* document = yyjson_read(json.data(), json.size(), 0);
    yyjson_val* root = document ? yyjson_doc_get_root(document) : nullptr;
    yyjson_val* ok_value = root ? yyjson_obj_get(root, "ok") : nullptr;
    const char* message = root ? yyjson_get_str(yyjson_obj_get(root, "message")) : nullptr;
    const bool ok = ok_value && yyjson_is_true(ok_value);
    if (!ok && restarting) {
        AcquireSRWLockExclusive(&g_update_lock);
        if (g_update_state.phase == "restarting") {
            g_update_state.phase = "error";
            g_update_state.error = message ? message : "The update could not be applied.";
            g_update_state.retry_action = "restart";
        }
        ReleaseSRWLockExclusive(&g_update_lock);
    } else if (!ok && message) {
        hubcap_runtime_log((std::string("Previous update result: ") + message).c_str());
    }
    if (document) yyjson_doc_free(document);
}

bool update_test_mode()
{
    std::wstring command = GetCommandLineW();
    std::transform(command.begin(), command.end(), command.begin(), towlower);
    return command.find(L"-hubcap-updatetest") != std::wstring::npos;
}

bool update_process_is_steam()
{
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, L"steam.exe") == 0;
}

void check_for_update(bool boot)
{
    if (update_test_mode()) {
        AcquireSRWLockExclusive(&g_update_lock);
        g_update_state.phase = "available";
        g_update_state.latest_version = std::string("v") + HUBCAP_UI_VERSION + "-test";
        g_update_state.release_name = "HubcapUI Update Test";
        g_update_state.release_notes = "Test mode changelog\n\n- Boot update prompt\n- In-Steam Update button\n- Verified staging\n- Restart and DLL replacement";
        g_update_state.boot_prompt_pending = false;
        g_update_state.error.clear(); g_update_state.retry_action.clear(); g_update_check_running.store(false);
        ReleaseSRWLockExclusive(&g_update_lock);
        if (boot) start_boot_update_dialog();
        return;
    }
    const HttpResult response = http_get_host(L"api.github.com", L"/repos/oogiefied/HubcapUI/releases/latest", "", 1024 * 1024, 8000);
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        set_update_error(response.error.empty() ? "Could not check for updates." : response.error, "check");
        g_update_check_running.store(false); return;
    }
    yyjson_doc* document = yyjson_read(response.body.data(), response.body.size(), 0);
    yyjson_val* root = document ? yyjson_doc_get_root(document) : nullptr;
    const char* tag = root ? yyjson_get_str(yyjson_obj_get(root, "tag_name")) : nullptr;
    const char* name = root ? yyjson_get_str(yyjson_obj_get(root, "name")) : nullptr;
    const char* notes = root ? yyjson_get_str(yyjson_obj_get(root, "body")) : nullptr;
    std::string download, digest;
    yyjson_val* assets = root ? yyjson_obj_get(root, "assets") : nullptr;
    size_t index, maximum; yyjson_val* asset;
    yyjson_arr_foreach(assets, index, maximum, asset) {
        const char* asset_name = yyjson_get_str(yyjson_obj_get(asset, "name"));
        const char* asset_url = yyjson_get_str(yyjson_obj_get(asset, "browser_download_url"));
        const char* asset_digest = yyjson_get_str(yyjson_obj_get(asset, "digest"));
        if (asset_name && asset_url && std::string(asset_name) == "mswsock.dll") {
            download = asset_url; digest = asset_digest ? asset_digest : ""; break;
        }
    }
    const bool available = tag && stable_release_tag(tag) && version_newer(tag, HUBCAP_UI_VERSION) && !download.empty() && sha256_digest(digest);
    AcquireSRWLockExclusive(&g_update_lock);
    if (available) {
        const bool new_version = g_update_state.latest_version != tag;
        g_update_state.phase = "available"; g_update_state.latest_version = tag;
        g_update_state.release_name = name ? name : tag; g_update_state.release_notes = notes ? notes : "";
        g_update_state.download_path = download; g_update_state.asset_digest = digest.substr(7); g_update_state.error.clear(); g_update_state.retry_action.clear();
        if (boot && new_version) g_update_state.deferred = false;
    } else {
        g_update_state = {}; g_update_state.phase = "upToDate";
    }
    g_update_check_running.store(false);
    ReleaseSRWLockExclusive(&g_update_lock);
    if (document) yyjson_doc_free(document);
    if (available && boot) start_boot_update_dialog();
}

void start_update_check(bool boot)
{
    AcquireSRWLockExclusive(&g_update_lock);
    const bool blocked = g_update_check_running.load() || g_update_download_running.load() ||
        g_update_state.phase == "downloading" || g_update_state.phase == "staged" || g_update_state.phase == "restarting" ||
        (g_update_state.phase == "error" && g_update_state.retry_action == "restart");
    if (blocked) { ReleaseSRWLockExclusive(&g_update_lock); return; }
    g_update_check_running.store(true);
    g_update_state.phase = "checking"; g_update_state.error.clear(); g_update_state.retry_action.clear();
    ReleaseSRWLockExclusive(&g_update_lock);
    std::thread(check_for_update, boot).detach();
}

void download_update()
{
    UpdateState state;
    AcquireSRWLockShared(&g_update_lock); state = g_update_state; ReleaseSRWLockShared(&g_update_lock);
    if (update_test_mode()) {
        const std::wstring root = module_root();
        const std::wstring source_dll = join(root, L"mswsock.dll");
        const std::wstring base = join(local_app_data(), L"HubcapUI\\updates\\test");
        CreateDirectoryW(join(local_app_data(), L"HubcapUI").c_str(), nullptr); CreateDirectoryW(join(local_app_data(), L"HubcapUI\\updates").c_str(), nullptr); CreateDirectoryW(base.c_str(), nullptr);
        const std::wstring dll = join(base, L"mswsock.dll");
        std::string dll_bytes;
        if (!read_text_file(source_dll, dll_bytes) || !write_file_atomic(dll, dll_bytes.data(), dll_bytes.size())) {
            set_update_error("Could not stage the test DLL.", "download"); g_update_download_running.store(false); return;
        }
        AcquireSRWLockExclusive(&g_update_lock); g_update_state.phase = "staged"; g_update_state.staged_dll = narrow(dll); g_update_state.staged_dll_hash = sha256_bytes(dll_bytes.data(), dll_bytes.size()); g_update_state.boot_prompt_pending = false; g_update_state.error.clear(); g_update_state.retry_action.clear(); g_update_download_running.store(false); ReleaseSRWLockExclusive(&g_update_lock);
        return;
    }
    const std::string prefix = "https://github.com";
    if (state.download_path.rfind(prefix, 0) != 0) { set_update_error("Update URL was not trusted.", "download"); g_update_download_running.store(false); return; }
    const HttpResult response = http_get_host(L"github.com", widen(state.download_path.substr(prefix.size())), "", 8 * 1024 * 1024, 120000);
    std::string wanted = state.asset_digest; std::transform(wanted.begin(), wanted.end(), wanted.begin(), ::toupper);
    if (!response.error.empty() || response.status < 200 || response.status >= 300 || sha256_bytes(response.body.data(), response.body.size()) != wanted) {
        set_update_error("Downloaded update failed SHA-256 verification.", "download"); g_update_download_running.store(false); return;
    }
    if (response.body.size() < 1024 || response.body.size() > 8ull * 1024ull * 1024ull || !valid_amd64_dll(response.body)) {
        set_update_error("Downloaded update was not a valid x64 DLL.", "download"); g_update_download_running.store(false); return;
    }
    const std::vector<unsigned char> dll_bytes(response.body.begin(), response.body.end());
    const std::string dll_hash = sha256_bytes(dll_bytes.data(), dll_bytes.size());
    if (dll_hash.empty()) { set_update_error("Could not verify the update DLL.", "download"); g_update_download_running.store(false); return; }
    const std::wstring base = join(local_app_data(), L"HubcapUI\\updates\\" + widen(state.latest_version));
    CreateDirectoryW(join(local_app_data(), L"HubcapUI").c_str(), nullptr); CreateDirectoryW(join(local_app_data(), L"HubcapUI\\updates").c_str(), nullptr); CreateDirectoryW(base.c_str(), nullptr);
    const std::wstring dll = join(base, L"mswsock.dll");
    if (!write_file_atomic(dll, dll_bytes.data(), dll_bytes.size())) { set_update_error("Could not stage the update.", "download"); g_update_download_running.store(false); return; }
    HMODULE candidate = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    const FARPROC version_export = candidate ? GetProcAddress(candidate, "__hubcap_bootstrap_version") : nullptr;
    const bool expected_exports = candidate && version_export && GetProcAddress(candidate, "AcceptEx");
    const char* embedded_version = expected_exports ? reinterpret_cast<const char* (__cdecl*)()>(version_export)() : nullptr;
    const bool version_matches = embedded_version && state.latest_version == std::string("v") + embedded_version;
    if (candidate) FreeLibrary(candidate);
    if (!expected_exports || !version_matches) { DeleteFileW(dll.c_str()); set_update_error("Downloaded DLL did not match the HubcapUI release version.", "download"); g_update_download_running.store(false); return; }
    AcquireSRWLockExclusive(&g_update_lock); g_update_state.phase = "staged"; g_update_state.staged_dll = narrow(dll); g_update_state.staged_dll_hash = dll_hash; g_update_state.boot_prompt_pending = false; g_update_state.error.clear(); g_update_state.retry_action.clear(); g_update_download_running.store(false); ReleaseSRWLockExclusive(&g_update_lock);
}

void start_update_download()
{
    AcquireSRWLockExclusive(&g_update_lock);
    const bool eligible = update_test_mode() ||
        ((g_update_state.phase == "available" || (g_update_state.phase == "error" && g_update_state.retry_action == "download")) &&
            !g_update_state.download_path.empty() && !g_update_state.asset_digest.empty());
    if (!eligible || g_update_download_running.load() || g_update_check_running.load()) {
        if (!eligible) { g_update_state.phase = "error"; g_update_state.error = "No verified update is available to download."; g_update_state.retry_action = "check"; }
        ReleaseSRWLockExclusive(&g_update_lock); return;
    }
    g_update_download_running.store(true);
    g_update_state.phase = "downloading"; g_update_state.boot_prompt_pending = false; g_update_state.error.clear(); g_update_state.retry_action.clear();
    ReleaseSRWLockExclusive(&g_update_lock);
    std::thread(download_update).detach();
}

std::string powershell_quote(std::string value)
{
    size_t cursor = 0;
    while ((cursor = value.find('\'', cursor)) != std::string::npos) { value.insert(cursor, 1, '\''); cursor += 2; }
    return "'" + value + "'";
}

std::string handle_update_restart(const std::string& id)
{
    UpdateState state; AcquireSRWLockShared(&g_update_lock); state = g_update_state; ReleaseSRWLockShared(&g_update_lock);
    const bool restart_retry = state.phase == "error" && state.retry_action == "restart";
    if (state.phase != "staged" && !restart_retry) { set_update_error("The update is not ready to install.", "download"); return reply_ok(id, "updateState", update_state_json()); }
    const std::wstring root = module_root(), steam = join(root, L"steam.exe"), target = join(root, L"mswsock.dll");
    const std::string current_hash = sha256_file(target);
    if (current_hash.empty() || state.staged_dll_hash.empty()) { set_update_error("Could not verify the installed or staged DLL.", "restart"); return reply_ok(id, "updateState", update_state_json()); }
    const std::wstring updates = join(local_app_data(), L"HubcapUI\\updates"); CreateDirectoryW(join(local_app_data(), L"HubcapUI").c_str(), nullptr); CreateDirectoryW(updates.c_str(), nullptr);
    const std::wstring script_path = join(updates, L"apply-update.ps1");
    const std::string target_q = powershell_quote(narrow(target)), stage_q = powershell_quote(state.staged_dll), steam_q = powershell_quote(narrow(steam));
    const std::string temp_q = powershell_quote(narrow(target + L".hubcap-update-new")), rollback_q = powershell_quote(narrow(target + L".hubcap-update-rollback"));
    const std::string result_q = powershell_quote(narrow(join(local_app_data(), L"HubcapUI\\update-result.json")));
    const std::string version_q = powershell_quote(state.latest_version);
    const std::string script = "$ErrorActionPreference='Stop'\nImport-Module \"$PSHOME\\Modules\\Microsoft.PowerShell.Utility\\Microsoft.PowerShell.Utility.psd1\" -Force\n$target=" + target_q + "\n$stage=" + stage_q + "\n$temp=" + temp_q + "\n$rollback=" + rollback_q + "\n$steam=" + steam_q + "\n$result=" + result_q +
        "\n$expected='" + current_hash + "'\n$wanted='" + state.staged_dll_hash + "'\n$deadline=(Get-Date).AddSeconds(120)\n$swapped=$false\n$safe=$false\n$installed=$false\n$message='Update failed.'\n" +
        "try { while(Get-Process -Id " + std::to_string(GetCurrentProcessId()) + " -ErrorAction SilentlyContinue){if((Get-Date) -gt $deadline){throw 'Steam did not exit in time.'};Start-Sleep -Milliseconds 500};" +
        "if((Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash -ne $expected){throw 'Installed DLL changed before update.'};if((Get-FileHash -Algorithm SHA256 -LiteralPath $stage).Hash -ne $wanted){throw 'Staged DLL verification failed.'};" +
        "foreach($owned in @($temp,$rollback)){if(Test-Path -LiteralPath $owned){Remove-Item -LiteralPath $owned -Force};if(Test-Path -LiteralPath $owned){throw 'Could not clear an old update file.'}};" +
        "Copy-Item -LiteralPath $stage -Destination $temp;if((Get-FileHash -Algorithm SHA256 -LiteralPath $temp).Hash -ne $wanted){throw 'Temporary DLL verification failed.'};" +
        "[IO.File]::Replace($temp,$target,$rollback,$true);$swapped=$true;if((Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash -ne $wanted){throw 'Installed DLL verification failed.'};" +
        "$safe=$true;$installed=$true;$message='Update installed successfully.';Remove-Item -LiteralPath $rollback,$stage -Force -ErrorAction SilentlyContinue}" +
        "catch { $message=$_.Exception.Message;if($swapped -and (Test-Path -LiteralPath $rollback)){try{[IO.File]::Replace($rollback,$target,$null,$true);if((Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash -ne $expected){throw 'Restored DLL verification failed.'};$safe=$true;$message+=' Original DLL restored.'}catch{$safe=$false;$message+=' Rollback failed: '+$_.Exception.Message}}elseif((-not $swapped) -and (Test-Path -LiteralPath $target)){$safe=((Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash -eq $expected)}}" +
        "finally { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue;@{ok=$installed;safeToLaunch=$safe;version=" + version_q + ";message=$message}|ConvertTo-Json|Set-Content -LiteralPath $result -Encoding UTF8 -ErrorAction SilentlyContinue;if($safe){Start-Process -FilePath $steam -ErrorAction SilentlyContinue};Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue }\n";
    DeleteFileW(join(local_app_data(), L"HubcapUI\\update-result.json").c_str());
    const std::string encoded_script = std::string("\xEF\xBB\xBF") + script;
    if (!write_file_atomic(script_path, encoded_script.data(), encoded_script.size())) { set_update_error("Could not create the update apply helper.", "restart"); return reply_ok(id, "updateState", update_state_json()); }
    std::wstring command = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script_path + L"\"";
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, root.c_str(), &startup, &process)) { DeleteFileW(script_path.c_str()); set_update_error("Could not start the update helper.", "restart"); return reply_ok(id, "updateState", update_state_json()); }
    CloseHandle(process.hThread);
    AcquireSRWLockExclusive(&g_update_lock); g_update_state.phase = "restarting"; ReleaseSRWLockExclusive(&g_update_lock);
    const HINSTANCE shutdown = ShellExecuteW(nullptr, L"open", steam.c_str(), L"-shutdown", root.c_str(), SW_HIDE);
    if (reinterpret_cast<INT_PTR>(shutdown) <= 32) {
        TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); CloseHandle(process.hProcess);
        if (!update_test_mode()) DeleteFileW(script_path.c_str());
        set_update_error("Could not request Steam shutdown.", "restart");
        return reply_ok(id, "updateState", update_state_json());
    }
    CloseHandle(process.hProcess);
    return reply_ok(id, "updateState", update_state_json());
}

struct NativeDialogButton { int id; const wchar_t* label; };
struct NativeDialogContext {
    int selected = 0;
    HBRUSH background = nullptr;
    HBRUSH panel = nullptr;
};

LRESULT CALLBACK update_window_proc(HWND window, UINT message, WPARAM word, LPARAM value)
{
    auto* context = reinterpret_cast<NativeDialogContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        context = static_cast<NativeDialogContext*>(reinterpret_cast<CREATESTRUCTW*>(value)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    } else if (message == WM_COMMAND && HIWORD(word) == BN_CLICKED && context) {
        context->selected = LOWORD(word); DestroyWindow(window); return 0;
    } else if (message == WM_ERASEBKGND && context) {
        RECT area{}; GetClientRect(window, &area); FillRect(reinterpret_cast<HDC>(word), &area, context->background); return 1;
    } else if (message == WM_CTLCOLORSTATIC && context) {
        HDC dc = reinterpret_cast<HDC>(word);
        const int control_id = GetDlgCtrlID(reinterpret_cast<HWND>(value));
        if (control_id == 2002) {
            SetBkColor(dc, RGB(37, 39, 44)); SetTextColor(dc, RGB(184, 198, 209));
            return reinterpret_cast<LRESULT>(context->panel);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, control_id == 2001 ? RGB(116, 209, 255) : RGB(220, 222, 223));
        return reinterpret_cast<LRESULT>(context->background);
    } else if (message == WM_CTLCOLOREDIT && context) {
        HDC dc = reinterpret_cast<HDC>(word);
        SetBkColor(dc, RGB(37, 39, 44)); SetTextColor(dc, RGB(184, 198, 209));
        return reinterpret_cast<LRESULT>(context->panel);
    } else if (message == WM_DRAWITEM) {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(value);
        if (!item || item->CtlType != ODT_BUTTON) return FALSE;
        if (context && context->background) FillRect(item->hDC, &item->rcItem, context->background);
        const bool primary = item->CtlID == 1001 || item->CtlID == 1003;
        COLORREF fill = primary ? RGB(26, 159, 255) : RGB(75, 83, 99);
        if (item->itemState & ODS_SELECTED) fill = primary ? RGB(20, 135, 212) : RGB(90, 100, 119);
        HBRUSH brush = CreateSolidBrush(fill); HPEN pen = CreatePen(PS_SOLID, 1, fill);
        HGDIOBJ old_brush = SelectObject(item->hDC, brush), old_pen = SelectObject(item->hDC, pen);
        RoundRect(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom, 6, 6);
        SelectObject(item->hDC, old_brush); SelectObject(item->hDC, old_pen); DeleteObject(brush); DeleteObject(pen);
        wchar_t label[128] = {}; GetWindowTextW(item->hwndItem, label, ARRAYSIZE(label));
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
        HGDIOBJ old_font = font ? SelectObject(item->hDC, font) : nullptr;
        SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, RGB(255, 255, 255));
        DrawTextW(item->hDC, label, -1, &item->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (old_font) SelectObject(item->hDC, old_font);
        if (item->itemState & ODS_FOCUS) { RECT focus = item->rcItem; InflateRect(&focus, -3, -3); DrawFocusRect(item->hDC, &focus); }
        return TRUE;
    } else if (message == WM_CLOSE) {
        DestroyWindow(window); return 0;
    } else if (message == WM_DESTROY) {
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, word, value);
}

int show_update_dialog(const std::wstring& instruction, const std::wstring& content,
    const NativeDialogButton* buttons, UINT button_count)
{
    static const wchar_t* class_name = L"HubcapUIUpdateWindow";
    static INIT_ONCE registration = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&registration, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = update_window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = class_name;
        return RegisterClassW(&window_class) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }, nullptr, nullptr);

    NativeDialogContext context;
    context.background = CreateSolidBrush(RGB(55, 61, 72));
    context.panel = CreateSolidBrush(RGB(37, 39, 44));
    constexpr int width = 540, height = 440;
    const int left = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
    const int top = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, class_name, L"HubcapUI Update",
        WS_CAPTION | WS_SYSMENU, left, top, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), &context);
    if (!window) {
        hubcap_runtime_log(("Native update window creation failed with Windows error " + std::to_string(GetLastError()) + ".").c_str());
        DeleteObject(context.background); DeleteObject(context.panel);
        return 0;
    }
    hubcap_runtime_log("Native update window opened.");

    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        if (auto set_attribute = reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
            BOOL dark = TRUE; if (FAILED(set_attribute(window, 20, &dark, sizeof(dark)))) set_attribute(window, 19, &dark, sizeof(dark));
        }
    }

    HFONT heading_font = CreateFontW(-19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
    HFONT text_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Arial");
    HWND heading = CreateWindowExW(0, L"STATIC", instruction.c_str(), WS_CHILD | WS_VISIBLE,
        16, 16, 490, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2001)), GetModuleHandleW(nullptr), nullptr);
    std::wstring windows_content; windows_content.reserve(content.size() + 32);
    for (size_t index = 0; index < content.size(); ++index) {
        if (content[index] == L'\n' && (index == 0 || content[index - 1] != L'\r')) windows_content.push_back(L'\r');
        windows_content.push_back(content[index]);
    }
    HWND changelog = CreateWindowExW(0, L"EDIT", windows_content.c_str(),
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        16, 54, 490, 290, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2002)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(heading_font), TRUE);
    SendMessageW(changelog, WM_SETFONT, reinterpret_cast<WPARAM>(text_font), TRUE);
    int button_left = 506 - static_cast<int>(button_count) * 130;
    for (UINT index = 0; index < button_count; ++index) {
        HWND button = CreateWindowExW(0, L"BUTTON", buttons[index].label,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            button_left + static_cast<int>(index) * 130, 360, 120, 32, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(buttons[index].id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(text_font), TRUE);
    }
    ShowWindow(window, SW_SHOWNORMAL); UpdateWindow(window); SetForegroundWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    DeleteObject(heading_font); DeleteObject(text_font); DeleteObject(context.background); DeleteObject(context.panel);
    return context.selected;
}

void boot_update_dialog_worker()
{
    constexpr int update_now = 1001, later = 1002, restart_now = 1003, close_dialog = 1004;
    UpdateState state;
    AcquireSRWLockShared(&g_update_lock); state = g_update_state; ReleaseSRWLockShared(&g_update_lock);
    const std::wstring instruction = L"HubcapUI " + widen(state.latest_version) + L" is available";
    std::wstring notes = widen(state.release_notes.empty() ? "No changelog was provided." : state.release_notes);
    if (notes.size() > 12000) notes.resize(12000);
    const NativeDialogButton available_buttons[] = {{update_now, L"Update Now"}, {later, L"Later"}};
    const int choice = show_update_dialog(instruction, notes, available_buttons, ARRAYSIZE(available_buttons));
    if (choice != update_now) {
        AcquireSRWLockExclusive(&g_update_lock);
        g_update_state.boot_prompt_pending = false;
        g_update_state.manual_prompt_pending = false;
        g_update_state.deferred = true;
        ReleaseSRWLockExclusive(&g_update_lock);
        g_update_boot_dialog_running.store(false);
        return;
    }

    start_update_download();
    const ULONGLONG deadline = GetTickCount64() + 125000;
    do {
        AcquireSRWLockShared(&g_update_lock); state = g_update_state; ReleaseSRWLockShared(&g_update_lock);
        if (state.phase == "staged" || state.phase == "error") break;
        Sleep(100);
    } while (GetTickCount64() < deadline);

    if (state.phase == "staged") {
        const NativeDialogButton ready_buttons[] = {{restart_now, L"Restart & Update"}, {later, L"Later"}};
        const int ready_choice = show_update_dialog(L"HubcapUI update is ready",
            L"The DLL was downloaded and verified. Restart Steam now to replace the loaded DLL.",
            ready_buttons, ARRAYSIZE(ready_buttons));
        if (ready_choice == restart_now) handle_update_restart("boot-update-dialog");
    } else {
        const NativeDialogButton error_buttons[] = {{close_dialog, L"Close"}};
        const std::wstring error = widen(state.error.empty() ? "The update could not be downloaded." : state.error);
        show_update_dialog(L"HubcapUI update failed", error, error_buttons, ARRAYSIZE(error_buttons));
    }
    g_update_boot_dialog_running.store(false);
}

void start_boot_update_dialog()
{
    if (!update_process_is_steam() || g_update_boot_dialog_running.exchange(true)) return;
    hubcap_runtime_log("Opening native boot update window.");
    std::thread(boot_update_dialog_worker).detach();
}

std::string handle_download(const std::string& id, const ResolvedApp& app)
{
    const Config config = read_config();
    if (!config.error.empty()) return reply_error(id, "config.invalid", config.error);
    const std::string& app_id = app.app_id;
    const HttpResult response = http_get(L"/api/v1/manifest/" + widen(app_id), config.api_key, 64 * 1024 * 1024, 120000);
    if (!response.error.empty()) return reply_error(id, "download.failed", response.error);
    if (response.status < 200 || response.status >= 300) return reply_error(id, "download.failed", http_error(response.status));

    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, response.body.data(), response.body.size(), 0)) {
        return reply_error(id, "zip.invalid", "Downloaded ZIP was invalid.");
    }

    std::vector<BundleFile> files;
    std::set<std::string> basenames;
    bool found_app_lua = false;
    bool found_manifest = false;
    unsigned long long total = 0;
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    std::string failure;
    if (count > 64) failure = "Downloaded ZIP contained too many files.";

    for (mz_uint index = 0; failure.empty() && index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) {
            failure = "Downloaded ZIP metadata was invalid.";
            break;
        }
        if (mz_zip_reader_is_file_a_directory(&archive, index)) continue;
        const std::string entry = stat.m_filename;
        if (!safe_bundle_name(entry) || stat.m_is_encrypted) {
            failure = "Downloaded ZIP contained an unsafe entry.";
            break;
        }
        if (stat.m_uncomp_size > 64ull * 1024 * 1024 || total + stat.m_uncomp_size > 256ull * 1024 * 1024) {
            failure = "Downloaded ZIP was too large.";
            break;
        }
        const std::string base = basename_of(entry);
        const bool is_lua = base.size() > 4 && base.compare(base.size() - 4, 4, ".lua") == 0 && numeric_app_id(base.substr(0, base.size() - 4));
        const bool is_manifest = manifest_name(base);
        if (!is_lua && !is_manifest) continue;
        if (!basenames.insert(base).second) {
            failure = "Downloaded ZIP contained duplicate file names.";
            break;
        }
        size_t extracted_size = 0;
        void* extracted = mz_zip_reader_extract_to_heap(&archive, index, &extracted_size, 0);
        if (!extracted || extracted_size != stat.m_uncomp_size) {
            if (extracted) mz_free(extracted);
            failure = "Could not extract the downloaded ZIP.";
            break;
        }
        BundleFile file;
        file.name = base;
        file.lua = is_lua;
        file.bytes.assign(static_cast<unsigned char*>(extracted), static_cast<unsigned char*>(extracted) + extracted_size);
        mz_free(extracted);
        total += extracted_size;
        if (base == app_id + ".lua") found_app_lua = true;
        if (is_manifest) found_manifest = true;
        files.push_back(std::move(file));
    }
    mz_zip_reader_end(&archive);

    if (failure.empty() && !found_app_lua) failure = "Downloaded ZIP did not contain the requested .lua file.";
    if (failure.empty() && !found_manifest) failure = "Downloaded ZIP did not contain a .manifest file.";
    if (!failure.empty()) return reply_error(id, "zip.invalid", failure);

    const std::wstring depot = join(module_root(), L"depotcache");
    CreateDirectoryW(config.lua_dir.c_str(), nullptr);
    CreateDirectoryW(depot.c_str(), nullptr);
    for (const auto& file : files) {
        const std::wstring target = join(file.lua ? config.lua_dir : depot, widen(file.name));
        if (!write_file_atomic(target, file.bytes.data(), file.bytes.size())) {
            return reply_error(id, "fs.installFailed", "Could not install the downloaded Hubcap files.");
        }
    }
    const std::wstring root = module_root();
    DeleteFileW(join(root, L"config\\hubcapplugin-manifest-" + widen(app_id) + L".txt").c_str());
    DeleteFileW(join(root, L"depotcache\\.hubcapmanifest-" + widen(app_id)).c_str());
    return reply_ok(id, "storeState", state_json(app, true, true, app.is_dlc ? dlc_status(app) : "Downloaded.", false));
}

std::string vdf_unescape(const std::string& value)
{
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && (value[i + 1] == '\\' || value[i + 1] == '"')) ++i;
        result.push_back(value[i]);
    }
    return result;
}

std::string vdf_value(const std::string& text, const std::string& wanted)
{
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const size_t first = line.find('"');
        const size_t key_end = first == std::string::npos ? std::string::npos : line.find('"', first + 1);
        const size_t value_begin = key_end == std::string::npos ? std::string::npos : line.find('"', key_end + 1);
        const size_t value_end = value_begin == std::string::npos ? std::string::npos : line.find('"', value_begin + 1);
        if (value_end == std::string::npos || line.substr(first + 1, key_end - first - 1) != wanted) continue;
        return vdf_unescape(line.substr(value_begin + 1, value_end - value_begin - 1));
    }
    return {};
}

std::vector<std::wstring> steam_library_roots()
{
    std::vector<std::wstring> roots{module_root()};
    std::string text;
    if (!read_text_file(join(module_root(), L"steamapps\\libraryfolders.vdf"), text)) return roots;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("\"path\"") == std::string::npos) continue;
        const std::string path = vdf_value(line, "path");
        const std::wstring wide_path = widen(path);
        if (!wide_path.empty() && std::none_of(roots.begin(), roots.end(), [&](const auto& item) {
                return _wcsicmp(item.c_str(), wide_path.c_str()) == 0;
            })) roots.push_back(wide_path);
    }
    return roots;
}

std::string installed_game_name(const std::string& app_id)
{
    for (const auto& library : steam_library_roots()) {
        std::string manifest;
        if (!read_text_file(join(library, L"steamapps\\appmanifest_" + widen(app_id) + L".acf"), manifest)) continue;
        const std::string name = vdf_value(manifest, "name");
        if (!name.empty()) return name;
    }
    return {};
}

bool resolve_steam_game_dir(const std::string& app_id, std::wstring& game_dir)
{
    for (const auto& library : steam_library_roots()) {
        std::string manifest;
        if (!read_text_file(join(library, L"steamapps\\appmanifest_" + widen(app_id) + L".acf"), manifest)) continue;
        const std::string install_dir = vdf_value(manifest, "installdir");
        if (install_dir.empty()) continue;
        const std::wstring candidate = join(library, L"steamapps\\common\\" + widen(install_dir));
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            game_dir = candidate;
            return true;
        }
    }
    return false;
}

void collect_files(const std::wstring& directory, int depth, const std::wstring& extension,
    std::vector<std::pair<std::wstring, unsigned long long>>& files)
{
    if (depth < 0 || files.size() > 4096) return;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(join(directory, L"*").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring path = join(directory, name);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) collect_files(path, depth - 1, extension, files);
            continue;
        }
        if (name.size() >= extension.size() && _wcsicmp(name.c_str() + name.size() - extension.size(), extension.c_str()) == 0) {
            const unsigned long long size = (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            files.push_back({path, size});
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
}

std::wstring lower_wide(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}

bool select_game_executable(const std::wstring& game_dir, const std::string& game_name, std::wstring& executable)
{
    std::vector<std::pair<std::wstring, unsigned long long>> candidates;
    collect_files(game_dir, 7, L".exe", candidates);
    long long best_score = std::numeric_limits<long long>::min();
    const std::wstring normalized_title = lower_wide(widen(normalized_game_name(game_name)));
    for (const auto& candidate : candidates) {
        const std::wstring path = lower_wide(candidate.first);
        if (path.find(L"\\_commonredist\\") != std::wstring::npos || path.find(L"\\redist\\") != std::wstring::npos ||
            path.find(L"crashreport") != std::wstring::npos || path.find(L"unins") != std::wstring::npos ||
            path.find(L"setup") != std::wstring::npos || path.find(L"unitycrashhandler") != std::wstring::npos ||
            path.find(L"easyanticheat") != std::wstring::npos || path.find(L"battleye") != std::wstring::npos ||
            path.find(L"benchmark") != std::wstring::npos || path.find(L"dedicatedserver") != std::wstring::npos ||
            path.find(L"crashpad") != std::wstring::npos || path.find(L"cefsubprocess") != std::wstring::npos) continue;
        long long score = static_cast<long long>(std::min<unsigned long long>(candidate.second / (1024 * 1024), 500));
        if (path.find(L"shipping") != std::wstring::npos) score += 3000;
        if (path.find(L"\\binaries\\win64\\") != std::wstring::npos) score += 1000;
        else if (path.find(L"\\win64\\") != std::wstring::npos || path.find(L"\\x64\\") != std::wstring::npos) score += 450;
        if (path.find(L"launcher") != std::wstring::npos || path.find(L"bootstrap") != std::wstring::npos ||
            path.find(L"startprotectedgame") != std::wstring::npos) score -= 1200;
        if (path.find(L"server") != std::wstring::npos || path.find(L"editor") != std::wstring::npos ||
            path.find(L"helper") != std::wstring::npos || path.find(L"tool") != std::wstring::npos) score -= 700;
        std::wstring compact_title;
        for (wchar_t c : normalized_title) if (iswalnum(c)) compact_title.push_back(c);
        std::wstring filename = path.substr(path.find_last_of(L"\\/") + 1);
        std::wstring compact_filename;
        for (wchar_t c : filename) if (iswalnum(c)) compact_filename.push_back(c);
        if (compact_title.size() >= 5 && compact_filename.find(compact_title) != std::wstring::npos) score += 500;
        if (score > best_score) {
            best_score = score;
            executable = candidate.first;
        }
    }
    return !executable.empty();
}

bool executable_is_64_bit(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return true;
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    bool result = ReadFile(file, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE;
    if (result) {
        LARGE_INTEGER offset{};
        offset.QuadPart = dos.e_lfanew + sizeof(DWORD);
        IMAGE_FILE_HEADER header{};
        result = SetFilePointerEx(file, offset, nullptr, FILE_BEGIN) && ReadFile(file, &header, sizeof(header), &read, nullptr) &&
            read == sizeof(header) && header.Machine == IMAGE_FILE_MACHINE_AMD64;
    }
    CloseHandle(file);
    return result;
}

std::wstring command_argument(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(c);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

bool file_version(const std::wstring& path, WORD& major, WORD& minor, WORD& patch)
{
    HMODULE version = LoadLibraryW(L"version.dll");
    if (!version) return false;
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    const auto size_fn = reinterpret_cast<SizeFn>(GetProcAddress(version, "GetFileVersionInfoSizeW"));
    const auto info_fn = reinterpret_cast<InfoFn>(GetProcAddress(version, "GetFileVersionInfoW"));
    const auto query_fn = reinterpret_cast<QueryFn>(GetProcAddress(version, "VerQueryValueW"));
    DWORD ignored = 0;
    const DWORD size = size_fn ? size_fn(path.c_str(), &ignored) : 0;
    std::vector<unsigned char> data(size);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixed_size = 0;
    const bool ok = size && info_fn && query_fn && info_fn(path.c_str(), 0, size, data.data()) &&
        query_fn(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &fixed_size) && fixed &&
        fixed_size >= sizeof(VS_FIXEDFILEINFO) && fixed->dwSignature == 0xFEEF04BD;
    if (ok) {
        major = HIWORD(fixed->dwFileVersionMS);
        minor = LOWORD(fixed->dwFileVersionMS);
        patch = HIWORD(fixed->dwFileVersionLS);
    }
    FreeLibrary(version);
    return ok;
}

bool file_version_at_least(const std::wstring& path, WORD wanted_major, WORD wanted_minor, WORD wanted_patch)
{
    WORD major = 0, minor = 0, patch = 0;
    return file_version(path, major, minor, patch) &&
        (major > wanted_major || (major == wanted_major &&
            (minor > wanted_minor || (minor == wanted_minor && patch >= wanted_patch))));
}

bool file_version_matches(const std::wstring& path, const std::string& wanted)
{
    unsigned int wanted_major = 0, wanted_minor = 0, wanted_patch = 0;
    char trailing = 0;
    if (std::sscanf(wanted.c_str(), "%u.%u.%u%c", &wanted_major, &wanted_minor, &wanted_patch, &trailing) != 3 ||
        wanted_major > 0xffff || wanted_minor > 0xffff || wanted_patch > 0xffff) return false;
    WORD major = 0, minor = 0, patch = 0;
    return file_version(path, major, minor, patch) && major == wanted_major && minor == wanted_minor && patch == wanted_patch;
}

bool file_signer_thumbprint(const std::wstring& path, std::string& thumbprint)
{
    HMODULE crypt32 = LoadLibraryW(L"crypt32.dll");
    if (!crypt32) return false;
    using QueryObjectFn = BOOL(WINAPI*)(DWORD, const void*, DWORD, DWORD, DWORD, DWORD*, DWORD*, DWORD*, HCERTSTORE*, HCRYPTMSG*, const void**);
    using MessageParamFn = BOOL(WINAPI*)(HCRYPTMSG, DWORD, DWORD, void*, DWORD*);
    using FindCertificateFn = PCCERT_CONTEXT(WINAPI*)(HCERTSTORE, DWORD, DWORD, DWORD, const void*, PCCERT_CONTEXT);
    using CertificatePropertyFn = BOOL(WINAPI*)(PCCERT_CONTEXT, DWORD, void*, DWORD*);
    using FreeCertificateFn = BOOL(WINAPI*)(PCCERT_CONTEXT);
    using CloseStoreFn = BOOL(WINAPI*)(HCERTSTORE, DWORD);
    using CloseMessageFn = BOOL(WINAPI*)(HCRYPTMSG);
    const auto query_object = reinterpret_cast<QueryObjectFn>(GetProcAddress(crypt32, "CryptQueryObject"));
    const auto message_param = reinterpret_cast<MessageParamFn>(GetProcAddress(crypt32, "CryptMsgGetParam"));
    const auto find_certificate = reinterpret_cast<FindCertificateFn>(GetProcAddress(crypt32, "CertFindCertificateInStore"));
    const auto certificate_property = reinterpret_cast<CertificatePropertyFn>(GetProcAddress(crypt32, "CertGetCertificateContextProperty"));
    const auto free_certificate = reinterpret_cast<FreeCertificateFn>(GetProcAddress(crypt32, "CertFreeCertificateContext"));
    const auto close_store = reinterpret_cast<CloseStoreFn>(GetProcAddress(crypt32, "CertCloseStore"));
    const auto close_message = reinterpret_cast<CloseMessageFn>(GetProcAddress(crypt32, "CryptMsgClose"));
    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    DWORD encoding = 0, content = 0, format = 0;
    bool ok = query_object && message_param && find_certificate && certificate_property && free_certificate && close_store && close_message &&
        query_object(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
            CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &content, &format, &store, &message, nullptr);
    std::vector<unsigned char> signer_data;
    PCCERT_CONTEXT certificate = nullptr;
    if (ok) {
        DWORD size = 0;
        ok = message_param(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &size) && size >= sizeof(CMSG_SIGNER_INFO);
        signer_data.resize(size);
        ok = ok && message_param(message, CMSG_SIGNER_INFO_PARAM, 0, signer_data.data(), &size);
    }
    if (ok) {
        const auto* signer = reinterpret_cast<const CMSG_SIGNER_INFO*>(signer_data.data());
        CERT_INFO info{};
        info.Issuer = signer->Issuer;
        info.SerialNumber = signer->SerialNumber;
        certificate = find_certificate(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &info, nullptr);
        ok = certificate != nullptr;
    }
    std::vector<unsigned char> hash;
    if (ok) {
        DWORD size = 0;
        ok = certificate_property(certificate, CERT_SHA1_HASH_PROP_ID, nullptr, &size) && size == 20;
        hash.resize(size);
        ok = ok && certificate_property(certificate, CERT_SHA1_HASH_PROP_ID, hash.data(), &size);
    }
    if (ok) {
        static constexpr char hex[] = "0123456789ABCDEF";
        thumbprint.clear();
        thumbprint.reserve(hash.size() * 2);
        for (unsigned char byte : hash) {
            thumbprint.push_back(hex[byte >> 4]);
            thumbprint.push_back(hex[byte & 0x0f]);
        }
    }
    if (certificate) free_certificate(certificate);
    if (message) close_message(message);
    if (store) close_store(store, 0);
    FreeLibrary(crypt32);
    return ok;
}

bool valid_reshade_setup(const std::wstring& path, const std::string& version, const std::string& signer_thumbprint,
    bool require_exact_version)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    unsigned char signature[2]{};
    DWORD read = 0;
    const bool executable = ReadFile(file, signature, sizeof(signature), &read, nullptr) && read == sizeof(signature) &&
        signature[0] == 'M' && signature[1] == 'Z';
    CloseHandle(file);
    std::string actual_thumbprint;
    const bool version_ok = require_exact_version ? file_version_matches(path, version) : file_version_at_least(path, 6, 8, 0);
    return executable && version_ok && file_signer_thumbprint(path, actual_thumbprint) &&
        _stricmp(actual_thumbprint.c_str(), signer_thumbprint.c_str()) == 0;
}

std::wstring reshade_api_for_target(const std::wstring& executable, const std::wstring& reshade_directory = {})
{
    const std::wstring directory = executable.substr(0, executable.find_last_of(L"\\/"));
    if (!reshade_directory.empty() &&
        GetFileAttributesW(join(reshade_directory, L"ReShade.ini").c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesW(join(reshade_directory, L"dxgi.dll").c_str()) != INVALID_FILE_ATTRIBUTES) return L"dxgi";
        if (GetFileAttributesW(join(reshade_directory, L"d3d12.dll").c_str()) != INVALID_FILE_ATTRIBUTES) return L"d3d12";
        if (GetFileAttributesW(join(reshade_directory, L"d3d11.dll").c_str()) != INVALID_FILE_ATTRIBUTES) return L"d3d11";
        if (GetFileAttributesW(join(reshade_directory, L"d3d10.dll").c_str()) != INVALID_FILE_ATTRIBUTES) return L"d3d10";
        if (GetFileAttributesW(join(reshade_directory, L"d3d9.dll").c_str()) != INVALID_FILE_ATTRIBUTES) return L"d3d9";
        if (GetFileAttributesW(join(reshade_directory, L"ddraw.dll").c_str()) != INVALID_FILE_ATTRIBUTES) return L"ddraw";
        if (GetFileAttributesW(join(reshade_directory, L"opengl32.dll").c_str()) != INVALID_FILE_ATTRIBUTES) return L"opengl";
    }
    if (GetFileAttributesW(join(directory, L"UnityPlayer.dll").c_str()) != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(join(directory, L"GameAssembly.dll").c_str()) != INVALID_FILE_ATTRIBUTES) {
        return L"dxgi";
    }
    std::wstring normalized = executable;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), towlower);
    if (normalized.find(L"\\binaries\\win64\\") != std::wstring::npos ||
        normalized.find(L"\\binaries\\win32\\") != std::wstring::npos) {
        return L"dxgi";
    }
    if (GetFileAttributesW(join(directory, L"vulkan-1.dll").c_str()) != INVALID_FILE_ATTRIBUTES) return L"vulkan";
    return {};
}

bool run_reshade_setup(const std::wstring& setup, const std::wstring& executable, std::string& error)
{
    const std::wstring api = reshade_api_for_target(executable);
    std::wstring command = command_argument(setup) + L" --headless --state update";
    if (!api.empty()) command += L" --api " + api;
    command += L" " + command_argument(executable);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(setup.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            setup.substr(0, setup.find_last_of(L"\\/")).c_str(), &startup, &process)) {
        error = "Could not start the ReShade installer.";
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 180000);
    DWORD exit_code = 1;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        error = "ReShade installation timed out.";
    } else if (!GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != 0) {
        error = "ReShade could not detect or install the game's rendering API.";
    }
    CloseHandle(process.hProcess);
    return error.empty();
}

bool run_reshade_uninstall(const std::wstring& setup, const std::wstring& executable,
    const std::wstring& reshade_directory, std::string& error)
{
    const std::wstring api = reshade_api_for_target(executable, reshade_directory);
    std::wstring command = command_argument(setup) + L" --headless --state uninstall";
    if (!api.empty()) command += L" --api " + api;
    command += L" " + command_argument(executable);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const std::wstring directory = setup.substr(0, setup.find_last_of(L"\\/"));
    if (!CreateProcessW(setup.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            directory.c_str(), &startup, &process)) {
        error = "Could not start the verified ReShade uninstaller.";
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 180000);
    DWORD exit_code = 1;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        error = "ReShade uninstall timed out.";
    } else if (!GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != 0) {
        error = "The official ReShade uninstaller could not remove this installation.";
    }
    CloseHandle(process.hProcess);
    return error.empty();
}

bool open_reshade_setup(const std::wstring& setup, const std::wstring& executable)
{
    const std::wstring arguments = command_argument(executable);
    const std::wstring directory = setup.substr(0, setup.find_last_of(L"\\/"));
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", setup.c_str(), arguments.c_str(), directory.c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool find_reshade_directory(const std::wstring& game_dir, std::wstring& directory)
{
    std::vector<std::pair<std::wstring, unsigned long long>> configs;
    collect_files(game_dir, 8, L"ReShade.ini", configs);
    if (configs.empty()) return false;
    FILETIME newest{};
    for (const auto& config : configs) {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (!GetFileAttributesExW(config.first.c_str(), GetFileExInfoStandard, &attributes)) continue;
        if (directory.empty() || CompareFileTime(&attributes.ftLastWriteTime, &newest) > 0) {
            newest = attributes.ftLastWriteTime;
            directory = config.first.substr(0, config.first.find_last_of(L"\\/"));
        }
    }
    return !directory.empty();
}

bool reno_installation_state(const ResolvedApp& app, RenoInstallTarget* target,
    std::vector<std::wstring>* addons)
{
    std::wstring game_dir;
    if (!numeric_app_id(app.app_id) || !resolve_steam_game_dir(app.app_id, game_dir)) return false;
    std::vector<std::pair<std::wstring, unsigned long long>> candidates;
    collect_files(game_dir, 8, L".addon32", candidates);
    collect_files(game_dir, 8, L".addon64", candidates);
    std::vector<std::wstring> matches;
    std::vector<std::wstring> primary_matches;
    for (const auto& candidate : candidates) {
        const size_t separator = candidate.first.find_last_of(L"\\/");
        const std::wstring name = lower_wide(candidate.first.substr(separator == std::wstring::npos ? 0 : separator + 1));
        if (name.find(L"renodx") == std::wstring::npos) continue;
        matches.push_back(candidate.first);
        if (name.find(L"dlssfix") == std::wstring::npos) primary_matches.push_back(candidate.first);
    }
    if (primary_matches.empty()) return false;
    if (addons) *addons = matches;
    if (target) {
        target->game_dir = game_dir;
        target->reshade_dir = primary_matches.front().substr(0, primary_matches.front().find_last_of(L"\\/"));
        select_game_executable(game_dir, app.game_name, target->executable);
    }
    return true;
}

bool extract_reno_addon(const HttpResult& response, bool want_64_bit, std::string& filename,
    std::vector<unsigned char>& bytes, std::string& error)
{
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        error = response.error.empty() ? "RenoDX download returned HTTP " + std::to_string(response.status) + "." : response.error;
        return false;
    }
    if (response.body.size() >= 2 && response.body[0] == 'M' && response.body[1] == 'Z') {
        filename = want_64_bit ? "renodx.addon64" : "renodx.addon32";
        bytes.assign(response.body.begin(), response.body.end());
        return true;
    }
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, response.body.data(), response.body.size(), 0)) {
        error = "RenoDX download was neither an add-on nor a valid ZIP archive.";
        return false;
    }
    const std::string suffix = want_64_bit ? ".addon64" : ".addon32";
    for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&archive); ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat) || mz_zip_reader_is_file_a_directory(&archive, index)) continue;
        const std::string entry = stat.m_filename;
        if (!safe_bundle_name(entry) || entry.size() < suffix.size() ||
            _stricmp(entry.c_str() + entry.size() - suffix.size(), suffix.c_str()) != 0 || stat.m_uncomp_size > 64ull * 1024 * 1024) continue;
        size_t size = 0;
        void* extracted = mz_zip_reader_extract_to_heap(&archive, index, &size, 0);
        if (extracted && size >= 2 && static_cast<unsigned char*>(extracted)[0] == 'M' && static_cast<unsigned char*>(extracted)[1] == 'Z') {
            filename = basename_of(entry);
            bytes.assign(static_cast<unsigned char*>(extracted), static_cast<unsigned char*>(extracted) + size);
            mz_free(extracted);
            mz_zip_reader_end(&archive);
            return true;
        }
        if (extracted) mz_free(extracted);
    }
    mz_zip_reader_end(&archive);
    error = "The RenoDX archive did not contain an add-on for this game's architecture.";
    return false;
}

std::wstring downloads_directory()
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &path)) || !path) return {};
    const std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

std::wstring default_browser_executable()
{
    DWORD size = 0;
    if (AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L"https", nullptr, nullptr, &size) != S_FALSE || !size) return {};
    std::wstring executable(size, L'\0');
    if (FAILED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L"https", nullptr, executable.data(), &size))) return {};
    if (!executable.empty() && executable.back() == L'\0') executable.pop_back();
    return executable;
}

std::wstring configured_download_directory(const std::wstring& preferences_path)
{
    std::string preferences;
    if (!read_text_file(preferences_path, preferences)) return {};
    yyjson_doc* document = yyjson_read(preferences.data(), preferences.size(), 0);
    yyjson_val* root = document ? yyjson_doc_get_root(document) : nullptr;
    yyjson_val* download = root ? yyjson_obj_get(root, "download") : nullptr;
    yyjson_val* savefile = root ? yyjson_obj_get(root, "savefile") : nullptr;
    const char* save_directory = savefile ? yyjson_get_str(yyjson_obj_get(savefile, "default_directory")) : nullptr;
    const char* download_directory = download ? yyjson_get_str(yyjson_obj_get(download, "default_directory")) : nullptr;
    const char* configured = save_directory && *save_directory ? save_directory : download_directory;
    const std::wstring result = configured ? widen(configured) : std::wstring();
    if (document) yyjson_doc_free(document);
    const DWORD attributes = result.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(result.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? result : std::wstring();
}

std::wstring chromium_download_directory(const std::wstring& user_data)
{
    std::string local_state;
    if (!read_text_file(join(user_data, L"Local State"), local_state)) return {};
    yyjson_doc* document = yyjson_read(local_state.data(), local_state.size(), 0);
    yyjson_val* root = document ? yyjson_doc_get_root(document) : nullptr;
    yyjson_val* profile = root ? yyjson_obj_get(root, "profile") : nullptr;
    const char* last_used = profile ? yyjson_get_str(yyjson_obj_get(profile, "last_used")) : nullptr;
    const std::wstring profile_name = widen(last_used && *last_used ? last_used : "Default");
    if (document) yyjson_doc_free(document);
    return profile_name.empty() ? std::wstring() : configured_download_directory(join(join(user_data, profile_name), L"Preferences"));
}

std::wstring roaming_app_data()
{
    const DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (!needed) return {};
    std::wstring value(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"APPDATA", value.data(), needed);
    if (!written || written >= needed) return {};
    value.resize(written);
    return value;
}

std::wstring firefox_download_directory(const std::wstring& roaming)
{
    std::string profiles;
    const std::wstring firefox = join(roaming, L"Mozilla\\Firefox");
    if (!read_text_file(join(firefox, L"profiles.ini"), profiles)) return {};
    std::istringstream stream(profiles);
    std::string line, selected, candidate;
    bool relative = true, is_default = false;
    const auto commit = [&]() {
        if (!candidate.empty() && (selected.empty() || is_default)) selected = candidate;
        candidate.clear(); relative = true; is_default = false;
    };
    while (std::getline(stream, line)) {
        line = trim(line);
        if (!line.empty() && line.front() == '[') { commit(); continue; }
        const size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = trim(line.substr(0, separator)), value = trim(line.substr(separator + 1));
        if (key == "Path") candidate = value;
        else if (key == "IsRelative") relative = value != "0";
        else if (key == "Default") is_default = value == "1";
    }
    commit();
    if (selected.empty()) return {};
    const std::wstring profile = relative ? join(firefox, widen(selected)) : widen(selected);
    std::string preferences;
    if (!read_text_file(join(profile, L"prefs.js"), preferences)) return {};
    const std::string marker = "user_pref(\"browser.download.dir\", \"";
    const size_t start = preferences.find(marker);
    if (start == std::string::npos) return {};
    const size_t begin = start + marker.size(), end = preferences.find("\");", begin);
    if (end == std::string::npos) return {};
    const std::wstring result = widen(vdf_unescape(preferences.substr(begin, end - begin)));
    const DWORD attributes = result.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(result.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? result : std::wstring();
}

std::vector<std::wstring> browser_download_directories()
{
    std::vector<std::wstring> directories;
    const auto add_directory = [&](const std::wstring& directory) {
        if (!directory.empty() && std::none_of(directories.begin(), directories.end(), [&](const auto& item) {
                return _wcsicmp(item.c_str(), directory.c_str()) == 0;
            })) directories.push_back(directory);
    };
    add_directory(configured_download_directory(join(local_app_data(), L"Steam\\htmlcache\\Default\\Preferences")));
    std::wstring browser = default_browser_executable();
    std::transform(browser.begin(), browser.end(), browser.begin(), towlower);
    const std::wstring app_data = local_app_data();
    const std::wstring roaming = roaming_app_data();
    std::wstring preferred;
    if (browser.find(L"chrome.exe") != std::wstring::npos) {
        preferred = chromium_download_directory(join(app_data, L"Google\\Chrome\\User Data"));
    } else if (browser.find(L"msedge.exe") != std::wstring::npos) {
        preferred = chromium_download_directory(join(app_data, L"Microsoft\\Edge\\User Data"));
    } else if (browser.find(L"brave.exe") != std::wstring::npos) {
        preferred = chromium_download_directory(join(app_data, L"BraveSoftware\\Brave-Browser\\User Data"));
    } else if (browser.find(L"vivaldi.exe") != std::wstring::npos) {
        preferred = chromium_download_directory(join(app_data, L"Vivaldi\\User Data"));
    } else if (browser.find(L"chromium.exe") != std::wstring::npos) {
        preferred = chromium_download_directory(join(app_data, L"Chromium\\User Data"));
    } else if (browser.find(L"opera.exe") != std::wstring::npos) {
        const std::wstring opera = browser.find(L"opera gx") != std::wstring::npos ?
            join(roaming, L"Opera Software\\Opera GX Stable") : join(roaming, L"Opera Software\\Opera Stable");
        preferred = configured_download_directory(join(opera, L"Preferences"));
    } else if (browser.find(L"firefox.exe") != std::wstring::npos) {
        preferred = firefox_download_directory(roaming);
    }
    add_directory(preferred);
    const std::wstring fallback = downloads_directory();
    add_directory(fallback);
    return directories;
}

std::wstring compact_download_name(const std::wstring& value)
{
    std::wstring result;
    const size_t dot = value.find_last_of(L'.');
    const size_t length = dot == std::wstring::npos ? value.size() : dot;
    for (size_t index = 0; index < length; ++index) if (iswalnum(value[index])) result.push_back(towlower(value[index]));
    return result;
}

bool load_manual_nexus_file(const std::wstring& path, const std::string& expected_filename,
    unsigned long long expected_size, bool want_64_bit, HttpResult& response, std::string& error)
{
    const std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
    if ((lower.size() < 4 || lower.compare(lower.size() - 4, 4, L".zip") != 0) &&
        (lower.size() < 8 || (lower.compare(lower.size() - 8, 8, L".addon64") != 0 &&
                              lower.compare(lower.size() - 8, 8, L".addon32") != 0))) {
        error = "Drop the completed Nexus ZIP or RenoDX add-on file.";
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        error = "The dropped Nexus file could not be read.";
        return false;
    }
    ULARGE_INTEGER size{};
    size.LowPart = attributes.nFileSizeLow;
    size.HighPart = attributes.nFileSizeHigh;
    if (size.QuadPart < 2 || size.QuadPart > 128ull * 1024ull * 1024ull ||
        (expected_size && size.QuadPart != expected_size)) {
        error = "That file size does not match the Nexus file selected by Hubcap.";
        return false;
    }
    const std::wstring expected = compact_download_name(widen(expected_filename));
    const std::wstring actual = compact_download_name(name);
    if (!expected.empty() && actual.find(expected) == std::wstring::npos && expected.find(actual) == std::wstring::npos) {
        error = "That filename does not match the Nexus file selected by Hubcap.";
        return false;
    }
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) { error = "The dropped Nexus file could not be opened."; return false; }
    HttpResult candidate;
    candidate.status = 200;
    candidate.body.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    file.close();
    std::string addon_name, validation_error;
    std::vector<unsigned char> addon_bytes;
    if (candidate.body.size() != size.QuadPart ||
        !extract_reno_addon(candidate, want_64_bit, addon_name, addon_bytes, validation_error)) {
        error = validation_error.empty() ? "That file did not contain the expected RenoDX add-on." : validation_error;
        return false;
    }
    response = std::move(candidate);
    return true;
}

struct NexusDropContext {
    HWND window = nullptr;
    HWND status = nullptr;
    std::wstring dropped_path;
    bool dismissed = false;
};

LRESULT CALLBACK nexus_drop_window_proc(HWND window, UINT message, WPARAM word, LPARAM value)
{
    auto* context = reinterpret_cast<NexusDropContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        context = static_cast<NexusDropContext*>(reinterpret_cast<CREATESTRUCTW*>(value)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        if (context) context->window = window;
    } else if (message == WM_DROPFILES && context) {
        const HDROP drop = reinterpret_cast<HDROP>(word);
        if (DragQueryFileW(drop, 0xffffffff, nullptr, 0) == 1) {
            const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            DragQueryFileW(drop, 0, path.data(), length + 1);
            path.resize(length);
            context->dropped_path = std::move(path);
            if (context->status) SetWindowTextW(context->status, L"Validating dropped file...");
        } else if (context->status) {
            SetWindowTextW(context->status, L"Drop one ZIP file at a time.");
        }
        DragFinish(drop);
        return 0;
    } else if (message == WM_COMMAND && LOWORD(word) == 1 && context) {
        context->dismissed = true;
        DestroyWindow(window);
        return 0;
    } else if (message == WM_CLOSE && context) {
        context->dismissed = true;
        DestroyWindow(window);
        return 0;
    } else if (message == WM_DESTROY && context) {
        context->window = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, word, value);
}

HWND show_nexus_drop_window(NexusDropContext& context, const std::string& expected_filename)
{
    static const wchar_t* class_name = L"HubcapUINexusDropWindow";
    static INIT_ONCE registration = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&registration, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = nexus_drop_window_proc;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = class_name;
        return RegisterClassW(&window_class) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }, nullptr, nullptr);
    constexpr int width = 540, height = 260;
    const int left = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
    const int top = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME | WS_EX_ACCEPTFILES, class_name,
        L"HubcapUI - Nexus Manual Download", WS_CAPTION | WS_SYSMENU, left, top, width, height,
        nullptr, nullptr, GetModuleHandleW(nullptr), &context);
    if (!window) return nullptr;
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HWND heading = CreateWindowExW(0, L"STATIC", L"Drag the downloaded Nexus ZIP here",
        WS_CHILD | WS_VISIBLE, 18, 16, 490, 24, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    const std::wstring expected = L"Waiting for: " + widen(expected_filename.empty() ? "the selected Nexus file" : expected_filename);
    HWND detail = CreateWindowExW(0, L"STATIC", expected.c_str(), WS_CHILD | WS_VISIBLE | SS_CENTER,
        18, 50, 490, 58, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    context.status = CreateWindowExW(0, L"STATIC", L"Automatic browser monitoring is still running.",
        WS_CHILD | WS_VISIBLE | SS_CENTER | WS_BORDER, 18, 116, 490, 48, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Keep Waiting in Background", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        330, 178, 178, 30, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)), GetModuleHandleW(nullptr), nullptr);
    for (HWND control : {heading, detail, context.status, cancel}) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    DragAcceptFiles(window, TRUE);
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    SetForegroundWindow(window);
    return window;
}

bool wait_for_manual_nexus_download(const std::string& expected_filename, unsigned long long expected_size, bool want_64_bit,
    HttpResult& response, std::string& downloaded_path, std::string& error)
{
    std::vector<std::wstring> download_directories = browser_download_directories();
    std::map<std::wstring, std::pair<unsigned long long, unsigned long long>> existing;
    for (const std::wstring& directory : download_directories) {
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(join(directory, L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ULARGE_INTEGER modified{}, size{};
            modified.LowPart = data.ftLastWriteTime.dwLowDateTime;
            modified.HighPart = data.ftLastWriteTime.dwHighDateTime;
            size.LowPart = data.nFileSizeLow;
            size.HighPart = data.nFileSizeHigh;
            existing[join(directory, data.cFileName)] = {modified.QuadPart, size.QuadPart};
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    const std::wstring expected_compact = compact_download_name(widen(expected_filename));
    std::map<std::wstring, unsigned long long> observed_sizes;
    const ULONGLONG started = GetTickCount64(), deadline = started + 15ull * 60ull * 1000ull;
    NexusDropContext drop_context;
    bool drop_prompt_shown = false;
    while (GetTickCount64() < deadline) {
        for (const std::wstring& directory : browser_download_directories()) {
            if (std::none_of(download_directories.begin(), download_directories.end(), [&](const auto& item) {
                    return _wcsicmp(item.c_str(), directory.c_str()) == 0;
                })) download_directories.push_back(directory);
        }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!drop_prompt_shown && GetTickCount64() - started >= 60000) {
            drop_prompt_shown = true;
            show_nexus_drop_window(drop_context, expected_filename);
        }
        if (!drop_context.dropped_path.empty()) {
            const std::wstring dropped = std::move(drop_context.dropped_path);
            drop_context.dropped_path.clear();
            std::string validation_error;
            if (load_manual_nexus_file(dropped, expected_filename, expected_size, want_64_bit, response, validation_error)) {
                downloaded_path = narrow(dropped);
                if (drop_context.window) DestroyWindow(drop_context.window);
                return true;
            }
            if (drop_context.status) SetWindowTextW(drop_context.status, widen(validation_error).c_str());
        }
        for (const std::wstring& downloads : download_directories) {
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW(join(downloads, L"*").c_str(), &data);
            if (find == INVALID_HANDLE_VALUE) continue;
            do {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring name = data.cFileName;
                std::wstring lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), towlower);
                if (lower.size() >= 11 && lower.compare(lower.size() - 11, 11, L".crdownload") == 0) continue;
                if ((lower.size() < 4 || lower.compare(lower.size() - 4, 4, L".zip") != 0) &&
                    (lower.size() < 8 || (lower.compare(lower.size() - 8, 8, L".addon64") != 0 &&
                                          lower.compare(lower.size() - 8, 8, L".addon32") != 0))) continue;
                ULARGE_INTEGER modified{}, size{};
                modified.LowPart = data.ftLastWriteTime.dwLowDateTime;
                modified.HighPart = data.ftLastWriteTime.dwHighDateTime;
                size.LowPart = data.nFileSizeLow;
                size.HighPart = data.nFileSizeHigh;
                const std::wstring path = join(downloads, name);
                const std::wstring actual_compact = compact_download_name(name);
                const bool expected_name = !expected_compact.empty() &&
                    (actual_compact.find(expected_compact) != std::wstring::npos ||
                     expected_compact.find(actual_compact) != std::wstring::npos);
                const auto original = existing.find(path);
                if ((original != existing.end() && original->second.first == modified.QuadPart &&
                        original->second.second == size.QuadPart && !expected_name) ||
                    size.QuadPart < 2 || size.QuadPart > 128ull * 1024ull * 1024ull) continue;
                if (expected_size && size.QuadPart != expected_size) continue;
                if (!expected_compact.empty() && !expected_name) continue;
                const auto previous = observed_sizes.find(path);
                if (previous == observed_sizes.end() || previous->second != size.QuadPart) {
                    observed_sizes[path] = size.QuadPart;
                    continue;
                }
                std::ifstream file(path.c_str(), std::ios::binary);
                if (!file) continue;
                HttpResult candidate;
                candidate.status = 200;
                candidate.body.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                file.close();
                std::string addon_name, validation_error;
                std::vector<unsigned char> addon_bytes;
                if (candidate.body.size() == size.QuadPart &&
                    extract_reno_addon(candidate, want_64_bit, addon_name, addon_bytes, validation_error)) {
                    response = std::move(candidate);
                    downloaded_path = narrow(path);
                    FindClose(find);
                    if (drop_context.window) DestroyWindow(drop_context.window);
                    return true;
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        Sleep(750);
    }
    if (drop_context.window) DestroyWindow(drop_context.window);
    error = "Timed out waiting for the Nexus Manual Download. If the browser asked where to save it, use its configured Downloads folder, then click RenoDX to try again.";
    return false;
}

bool parse_nexus_mod_url(const std::string& url, std::string& game, std::string& mod_id)
{
    const std::string prefix = "https://www.nexusmods.com/";
    if (url.rfind(prefix, 0) != 0) return false;
    const size_t game_end = url.find('/', prefix.size());
    if (game_end == std::string::npos || url.compare(game_end, 6, "/mods/") != 0) return false;
    const size_t id_begin = game_end + 6;
    size_t id_end = url.find_first_of("/?#", id_begin);
    if (id_end == std::string::npos) id_end = url.size();
    game = url.substr(prefix.size(), game_end - prefix.size());
    mod_id = url.substr(id_begin, id_end - id_begin);
    return !game.empty() && numeric_app_id(mod_id);
}

bool nexus_download_url(const RenoMod& mod, const std::string& api_key, bool want_64_bit, bool allow_manual_wait,
    std::string& url, HttpResult& manual_response, std::string& manual_path, bool& manual_required,
    std::string& manual_page, std::string& error)
{
    if (api_key.empty()) {
        error = "This RenoDX mod is Nexus-only. Add a Nexus API key in Hubcap Advanced Settings.";
        return false;
    }
    std::string game, mod_id;
    if (!parse_nexus_mod_url(mod.nexus_url, game, mod_id)) {
        error = "The RenoDX Nexus link was invalid.";
        return false;
    }
    const std::wstring headers = L"apikey: " + widen(api_key);
    const std::wstring prefix = L"/v1/games/" + widen(game) + L"/mods/" + widen(mod_id) + L"/files";
    const HttpResult files = http_get_host(L"api.nexusmods.com", prefix + L".json", "", 4 * 1024 * 1024, 20000, headers);
    if (!files.error.empty() || files.status < 200 || files.status >= 300) {
        error = files.status == 401 || files.status == 403 ? "Nexus rejected the API key." : "Could not list this Nexus mod's files.";
        return false;
    }
    yyjson_doc* document = yyjson_read(files.body.data(), files.body.size(), 0);
    yyjson_val* root = document ? yyjson_doc_get_root(document) : nullptr;
    yyjson_val* array = root ? yyjson_obj_get(root, "files") : nullptr;
    long long selected_id = 0, selected_time = 0;
    bool selected_is_main = false;
    std::string selected_filename;
    unsigned long long selected_size = 0;
    size_t index, maximum;
    yyjson_val* file;
    yyjson_arr_foreach(array, index, maximum, file) {
        const long long file_id = json_integer(file, "file_id", nullptr);
        const long long uploaded = json_integer(file, "uploaded_timestamp", nullptr);
        const long long category = json_integer(file, "category_id", nullptr);
        const bool is_main = category == 1;
        if (file_id > 0 && ((!selected_is_main && is_main) || (selected_is_main == is_main && uploaded >= selected_time))) {
            selected_id = file_id;
            selected_time = uploaded;
            selected_is_main = is_main;
            const char* file_name = yyjson_get_str(yyjson_obj_get(file, "file_name"));
            selected_filename = file_name ? file_name : "";
            const long long file_size = json_integer(file, "size_in_bytes", nullptr);
            selected_size = file_size > 0 ? static_cast<unsigned long long>(file_size) : 0;
        }
    }
    if (document) yyjson_doc_free(document);
    if (!selected_id) {
        error = "Nexus did not return a downloadable RenoDX file.";
        return false;
    }
    const HttpResult links = http_get_host(L"api.nexusmods.com", prefix + L"/" + std::to_wstring(selected_id) + L"/download_link.json",
        "", 1024 * 1024, 20000, headers);
    if (links.status == 403) {
        manual_page = mod.nexus_url.substr(0, mod.nexus_url.find_first_of("?#"));
        manual_page += "?tab=files&file_id=" + std::to_string(selected_id);
        if (!allow_manual_wait) {
            manual_required = true;
            return true;
        }
        return wait_for_manual_nexus_download(selected_filename, selected_size, want_64_bit,
            manual_response, manual_path, error);
    }
    if (!links.error.empty() || links.status < 200 || links.status >= 300) {
        error = "Could not get a Nexus download link.";
        return false;
    }
    document = yyjson_read(links.body.data(), links.body.size(), 0);
    root = document ? yyjson_doc_get_root(document) : nullptr;
    yyjson_val* first = root && yyjson_arr_size(root) ? yyjson_arr_get_first(root) : nullptr;
    const char* uri = yyjson_get_str(yyjson_obj_get(first, "URI"));
    if (uri) url = uri;
    if (document) yyjson_doc_free(document);
    if (url.rfind("https://", 0) != 0) {
        error = "Nexus returned an invalid download link.";
        return false;
    }
    return true;
}

std::string handle_reno_install(const std::string& id, const ResolvedApp& app, bool allow_manual_wait = false)
{
    RenoMod mod;
    if (!find_reno_mod(app.game_name, mod)) return reply_error(id, "reno.unavailable", "RenoDX is not available for this game.");
    if (mod.snapshot_url.empty() && mod.nexus_url.empty() && !mod.discord_url.empty()) {
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", widen(mod.discord_url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            return reply_error(id, "reno.discordOpenFailed",
                "This RenoDX mod is only available through RenoDX Discord. Join the Discord to download it.");
        }
        return reply_ok(id, "renoDiscord", "{\"appId\":" + json_escape(app.app_id) +
            ",\"gameName\":" + json_escape(app.game_name) +
            ",\"statusText\":\"This mod is only available through RenoDX Discord. Join the Discord to download it.\"}");
    }
    RenoInstallTarget target;
    if (!resolve_steam_game_dir(app.app_id, target.game_dir)) {
        return reply_error(id, "reno.notInstalled", "Install this game through Steam before installing RenoDX.");
    }
    if (!select_game_executable(target.game_dir, app.game_name, target.executable)) {
        return reply_error(id, "reno.noExecutable", "Hubcap could not find a safe game executable for ReShade.");
    }
    const bool is_64_bit = executable_is_64_bit(target.executable);
    HttpResult addon_response;
    const std::string selected_snapshot = mod.generic ?
        (is_64_bit ? mod.snapshot_url : mod.snapshot_url_32) : mod.snapshot_url;
    const std::string expected_snapshot = mod.generic ? expected_generic_snapshot(mod.engine, is_64_bit) : "";
    if (mod.generic && selected_snapshot.empty()) {
        return reply_error(id, "reno.architectureUnsupported",
            "The generic RenoDX add-on does not support this game's executable architecture.");
    }
    if (mod.generic && selected_snapshot != expected_snapshot) {
        return reply_error(id, "reno.catalogInvalid",
            "The RenoDX wiki profile did not contain the expected official generic add-on link.");
    }
    std::string addon_download_url = selected_snapshot;
    std::string source = mod.generic ? ("Generic " + mod.engine) : "Snapshot";
    if (!selected_snapshot.empty()) addon_response = http_get_url(selected_snapshot, 64 * 1024 * 1024, 120000);
    if (selected_snapshot.empty() || !addon_response.error.empty() || addon_response.status < 200 || addon_response.status >= 300) {
        std::string nexus_url, nexus_error, manual_path, manual_page;
        bool manual_required = false;
        HttpResult manual_response;
        if (mod.nexus_url.empty() || !nexus_download_url(mod, read_launcher_settings().nexus_api_key, is_64_bit,
                allow_manual_wait, nexus_url, manual_response, manual_path, manual_required, manual_page, nexus_error)) {
            return reply_error(id, "reno.downloadFailed", nexus_error.empty() ? "The RenoDX snapshot download failed." : nexus_error);
        }
        if (manual_required) {
            return reply_ok(id, "renoNexusManual", "{\"appId\":" + json_escape(app.app_id) +
                ",\"gameName\":" + json_escape(app.game_name) + ",\"url\":" + json_escape(manual_page) +
                ",\"statusText\":\"Download the selected Nexus file; Hubcap will detect it and continue automatically.\"}");
        }
        if (!manual_response.body.empty()) {
            addon_response = std::move(manual_response);
            addon_download_url = manual_path;
            source = "Nexus Mods Manual Download";
        } else {
            addon_response = http_get_url(nexus_url, 128 * 1024 * 1024, 120000);
            addon_download_url = nexus_url;
            source = "Nexus Mods";
        }
    }
    std::string addon_name, error;
    std::vector<unsigned char> addon_bytes;
    if (!extract_reno_addon(addon_response, is_64_bit, addon_name, addon_bytes, error)) {
        return reply_error(id, "reno.invalidDownload", error);
    }
    if (addon_response.body.size() >= 2 && addon_response.body[0] == 'M' && addon_response.body[1] == 'Z') {
        std::string candidate = basename_of(addon_download_url.substr(0, addon_download_url.find_first_of("?#")));
        const std::string wanted_suffix = is_64_bit ? ".addon64" : ".addon32";
        if (candidate.size() >= wanted_suffix.size() &&
            _stricmp(candidate.c_str() + candidate.size() - wanted_suffix.size(), wanted_suffix.c_str()) == 0) {
            addon_name = candidate;
        }
    }
    if (mod.generic) {
        const std::string expected_name = basename_of(expected_snapshot);
        if (_stricmp(addon_name.c_str(), expected_name.c_str()) != 0) {
            return reply_error(id, "reno.addonIdentityMismatch",
                "The downloaded add-on did not match the selected generic RenoDX engine profile.");
        }
    }
    const std::wstring base = join(local_app_data(), L"HubcapUI\\reno");
    CreateDirectoryW(join(local_app_data(), L"HubcapUI").c_str(), nullptr);
    if (!CreateDirectoryW(base.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return reply_error(id, "reno.cacheFailed", "Could not create the RenoDX download folder.");
    }
    const std::wstring setup_path = join(base, L"ReShade_Setup_Addon.exe");
    std::string setup_url, setup_version, signer_thumbprint, release_error;
    const bool release_available = official_reshade_addon_release(
        setup_url, setup_version, signer_thumbprint, release_error);
    static constexpr char known_signer[] = "589690208A5E52FB96980C4A6698F50ACD47C49F";
    bool cached = release_available ?
        valid_reshade_setup(setup_path, setup_version, signer_thumbprint, true) :
        valid_reshade_setup(setup_path, "", known_signer, false);
    if (!cached) {
        if (!release_available) {
            return reply_error(id, "reno.reshadeDownloadFailed",
                release_error.empty() ? "Could not check the official full-add-on ReShade installer." : release_error);
        }
        const HttpResult setup_response = http_get_url(setup_url, 32 * 1024 * 1024, 120000);
        if (!setup_response.error.empty() || setup_response.status < 200 || setup_response.status >= 300 ||
            setup_response.body.size() < 2 || setup_response.body[0] != 'M' || setup_response.body[1] != 'Z') {
            return reply_error(id, "reno.reshadeDownloadFailed", "Could not download the official full-add-on ReShade installer.");
        }
        const std::wstring candidate_path = setup_path + L".download";
        DeleteFileW(candidate_path.c_str());
        if (!write_file_atomic(candidate_path, setup_response.body.data(), setup_response.body.size()) ||
            !valid_reshade_setup(candidate_path, setup_version, signer_thumbprint, true)) {
            DeleteFileW(candidate_path.c_str());
            return reply_error(id, "reno.reshadeInvalid", "The downloaded ReShade installer failed version or signer verification.");
        }
        if (!MoveFileExW(candidate_path.c_str(), setup_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(candidate_path.c_str());
            return reply_error(id, "reno.cacheFailed", "Could not replace the cached ReShade installer.");
        }
    }
    if (!find_reshade_directory(target.game_dir, target.reshade_dir) &&
        !run_reshade_setup(setup_path, target.executable, error)) {
        if (open_reshade_setup(setup_path, target.executable)) {
            return reply_ok(id, "renoReshadeManual", "{\"appId\":" + json_escape(app.app_id) +
                ",\"gameName\":" + json_escape(app.game_name) +
                ",\"executable\":" + json_escape(narrow(target.executable)) +
                ",\"statusText\":\"ReShade needs the rendering API selected manually. Finish the preselected installer, then click RenoDX again.\"}");
        }
        return reply_error(id, "reno.reshadeInstallFailed", error);
    }
    if (target.reshade_dir.empty() && !find_reshade_directory(target.game_dir, target.reshade_dir)) {
        return reply_error(id, "reno.reshadeNotFound", "ReShade finished, but Hubcap could not locate its game installation folder.");
    }
    const std::wstring addon_path = join(target.reshade_dir, widen(addon_name));
    if (!write_file_atomic(addon_path, addon_bytes.data(), addon_bytes.size())) {
        return reply_error(id, "reno.addonInstallFailed", "Could not copy the RenoDX add-on into the game folder.");
    }
    if (mod.generic && !write_reno_settings(target.reshade_dir, mod.settings, error)) {
        DeleteFileW(addon_path.c_str());
        return reply_error(id, "reno.settingsFailed", error);
    }
    return reply_ok(id, "renoInstalled", "{\"appId\":" + json_escape(app.app_id) + ",\"gameName\":" +
        json_escape(app.game_name) + ",\"modName\":" + json_escape(mod.name) + ",\"source\":" + json_escape(source) +
        ",\"generic\":" + std::string(mod.generic ? "true" : "false") +
        ",\"engine\":" + json_escape(mod.engine) + ",\"notes\":" + json_escape(mod.notes) +
        ",\"settingsApplied\":" + std::to_string(mod.settings.empty() ? 0 : mod.settings.size() - mod.settings.count("SettingsMode")) +
        ",\"executable\":" + json_escape(narrow(target.executable)) + ",\"installDir\":" + json_escape(narrow(target.reshade_dir)) + "}");
}

std::string handle_reno_remove(const std::string& id, const ResolvedApp& app)
{
    RenoInstallTarget target;
    std::vector<std::wstring> addons;
    if (!reno_installation_state(app, &target, &addons)) {
        return reply_ok(id, "renoRemoved", "{\"appId\":" + json_escape(app.app_id) +
            ",\"gameName\":" + json_escape(app.game_name) + ",\"statusText\":\"RenoDX is already off.\"}");
    }
    if (target.executable.empty()) {
        return reply_error(id, "reno.noExecutable", "Hubcap could not safely identify the game executable for ReShade uninstall.");
    }
    const std::wstring setup = join(local_app_data(), L"HubcapUI\\reno\\ReShade_Setup_Addon.exe");
    static constexpr char known_signer[] = "589690208A5E52FB96980C4A6698F50ACD47C49F";
    if (!valid_reshade_setup(setup, "", known_signer, false)) {
        return reply_error(id, "reno.uninstallerMissing", "The cached official ReShade add-on installer is missing or failed verification.");
    }
    std::string error;
    if (!run_reshade_uninstall(setup, target.executable, target.reshade_dir, error)) {
        return reply_error(id, "reno.uninstallFailed", error);
    }
    std::vector<std::wstring> remove_files = addons;
    for (const wchar_t* name : {L"ReShade.ini", L"ReShade2.ini", L"ReShadePreset.ini", L"ReShade.log"}) {
        remove_files.push_back(join(target.reshade_dir, name));
    }
    for (const std::wstring& path : remove_files) {
        if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            return reply_error(id, "reno.cleanupFailed",
                "ReShade was uninstalled, but a RenoDX or preset file could not be removed. Close the game and try again.");
        }
    }
    if (reno_installation_state(app)) {
        return reply_error(id, "reno.cleanupFailed", "A RenoDX add-on remains in the game folder. Close the game and try again.");
    }
    return reply_ok(id, "renoRemoved", "{\"appId\":" + json_escape(app.app_id) +
        ",\"gameName\":" + json_escape(app.game_name) + ",\"statusText\":\"RenoDX and ReShade were removed.\"}");
}

struct OptiScalerTarget {
    std::wstring game_dir;
    std::wstring executable;
    std::wstring install_dir;
    std::wstring proxy_name;
    bool reshade_chained = false;
};

std::wstring optiscaler_marker(const std::wstring& directory)
{
    return join(directory, L".hubcap-optiscaler.txt");
}

std::wstring optiscaler_index_directory()
{
    return join(join(join(local_app_data(), L"HubcapUI"), L"optiscaler"), L"installed");
}

std::wstring optiscaler_index_path(const std::string& app_id)
{
    return join(optiscaler_index_directory(), widen(app_id) + L".txt");
}

bool optiscaler_indexed_directory(const ResolvedApp& app, const std::wstring& game_dir, std::wstring& directory)
{
    std::string saved;
    if (!read_text_file(optiscaler_index_path(app.app_id), saved)) return false;
    directory = normalize_directory(widen(trim(saved)));
    std::wstring parent = lower_wide(normalize_directory(game_dir));
    std::wstring child = lower_wide(directory);
    if (!parent.empty() && parent.back() != L'\\') parent.push_back(L'\\');
    return !directory.empty() && (child + L'\\').rfind(parent, 0) == 0;
}

bool optiscaler_installation_state(const ResolvedApp& app, OptiScalerTarget* target = nullptr,
    std::vector<std::wstring>* installed_files = nullptr, bool require_active_files = true)
{
    OptiScalerTarget found;
    if (!numeric_app_id(app.app_id) || !resolve_steam_game_dir(app.app_id, found.game_dir)) return false;
    if (!optiscaler_indexed_directory(app, found.game_dir, found.install_dir)) {
        if (!select_game_executable(found.game_dir, app.game_name, found.executable)) return false;
        found.install_dir = found.executable.substr(0, found.executable.find_last_of(L"\\/"));
    }
    std::string marker;
    if (!read_text_file(optiscaler_marker(found.install_dir), marker)) return false;
    std::istringstream lines(marker);
    std::string line;
    std::vector<std::wstring> files;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("proxy=", 0) == 0) found.proxy_name = widen(line.substr(6));
        else if (line == "reshadeChained=1") found.reshade_chained = true;
        else if (line.rfind("file=", 0) == 0) {
            const std::wstring relative = widen(line.substr(5));
            if (!relative.empty() && relative.find(L"..") == std::wstring::npos &&
                relative.front() != L'\\' && relative.front() != L'/') files.push_back(join(found.install_dir, relative));
        }
    }
    if (found.proxy_name.empty() || found.proxy_name.find(L"..") != std::wstring::npos) return false;
    if (require_active_files &&
        (GetFileAttributesW(join(found.install_dir, found.proxy_name).c_str()) == INVALID_FILE_ATTRIBUTES ||
         GetFileAttributesW(join(found.install_dir, L"OptiScaler.ini").c_str()) == INVALID_FILE_ATTRIBUTES)) return false;
    if (target) *target = found;
    if (installed_files) *installed_files = files;
    return true;
}

std::string detected_gpu_vendor()
{
    bool amd = false, intel = false, nvidia = false;
    for (DWORD index = 0;; ++index) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) break;
        const std::wstring id = lower_wide(device.DeviceID);
        nvidia = nvidia || id.find(L"ven_10de") != std::wstring::npos;
        amd = amd || id.find(L"ven_1002") != std::wstring::npos;
        intel = intel || id.find(L"ven_8086") != std::wstring::npos;
    }
    if (nvidia) return "NVIDIA";
    if (amd) return "AMD";
    if (intel) return "Intel";
    return "Unknown";
}

bool delete_directory_tree(const std::wstring& directory)
{
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(join(directory, L"*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = join(directory, name);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(path.c_str());
                else if (!delete_directory_tree(path)) { FindClose(search); return false; }
            } else if (!DeleteFileW(path.c_str())) { FindClose(search); return false; }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    return RemoveDirectoryW(directory.c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool run_archive_extract(const std::wstring& archive, const std::wstring& destination, std::string& error)
{
    wchar_t system[MAX_PATH]{};
    if (!GetSystemDirectoryW(system, MAX_PATH)) { error = "Could not locate the Windows archive extractor."; return false; }
    const std::wstring tar = join(system, L"tar.exe");
    std::wstring command = command_argument(tar) + L" -xf " + command_argument(archive) + L" -C " + command_argument(destination);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            destination.c_str(), &startup, &process)) {
        error = "Could not start the Windows archive extractor.";
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 180000);
    DWORD exit_code = 1;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        error = "OptiScaler extraction timed out.";
    } else if (!GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != 0) {
        error = "The verified OptiScaler package could not be extracted.";
    }
    CloseHandle(process.hProcess);
    return error.empty();
}

bool official_optiscaler_release(std::string& tag, std::string& url, std::string& digest, std::string& error)
{
    const HttpResult response = http_get_url("https://api.github.com/repos/optiscaler/OptiScaler/releases/latest",
        2 * 1024 * 1024, 30000, L"User-Agent: HubcapUI\r\nAccept: application/vnd.github+json\r\n");
    if (!response.error.empty() || response.status < 200 || response.status >= 300) {
        error = response.error.empty() ? "The official OptiScaler release API returned HTTP " +
            std::to_string(response.status) + "." : response.error;
        return false;
    }
    yyjson_doc* document = yyjson_read(response.body.data(), response.body.size(), 0);
    yyjson_val* root = document ? yyjson_doc_get_root(document) : nullptr;
    const char* release_tag = root ? yyjson_get_str(yyjson_obj_get(root, "tag_name")) : nullptr;
    yyjson_val* assets = root ? yyjson_obj_get(root, "assets") : nullptr;
    size_t index = 0, maximum = 0;
    yyjson_val* asset = nullptr;
    yyjson_arr_foreach(assets, index, maximum, asset) {
        const char* name = yyjson_get_str(yyjson_obj_get(asset, "name"));
        const char* download = yyjson_get_str(yyjson_obj_get(asset, "browser_download_url"));
        const char* asset_digest = yyjson_get_str(yyjson_obj_get(asset, "digest"));
        const std::string filename = name ? name : "";
        if (download && asset_digest && filename.size() > 3 && filename.substr(filename.size() - 3) == ".7z") {
            url = download;
            digest = asset_digest;
            break;
        }
    }
    tag = release_tag ? release_tag : "";
    if (document) yyjson_doc_free(document);
    if (!stable_release_tag(tag) || url.empty() || !sha256_digest(digest)) {
        error = "The official OptiScaler release did not contain a verified 7z asset.";
        return false;
    }
    digest.erase(0, 7);
    std::transform(digest.begin(), digest.end(), digest.begin(), ::toupper);
    return true;
}

bool prepare_optiscaler_package(std::wstring& extracted, std::string& version, std::string& error)
{
    std::string url, digest;
    if (!official_optiscaler_release(version, url, digest, error)) return false;
    const std::wstring hubcap = join(local_app_data(), L"HubcapUI");
    const std::wstring base = join(hubcap, L"optiscaler");
    const std::wstring archive = join(base, L"OptiScaler.7z");
    const std::wstring candidate = archive + L".download";
    extracted = join(base, L"package");
    CreateDirectoryW(hubcap.c_str(), nullptr);
    if (!CreateDirectoryW(base.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = "Could not create the OptiScaler cache folder.";
        return false;
    }
    std::string cache_marker;
    std::string cached_archive, cached_dll;
    if (read_text_file(join(base, L"cache.txt"), cache_marker) && cache_marker == version + "\n" + digest &&
        read_text_file(archive, cached_archive) &&
        _stricmp(sha256_bytes(cached_archive.data(), cached_archive.size()).c_str(), digest.c_str()) == 0 &&
        read_text_file(join(extracted, L"OptiScaler.dll"), cached_dll) && valid_amd64_dll(cached_dll)) return true;
    if (GetFileAttributesW(extracted.c_str()) != INVALID_FILE_ATTRIBUTES && !delete_directory_tree(extracted)) {
        error = "Could not refresh the OptiScaler package cache.";
        return false;
    }
    if (!CreateDirectoryW(extracted.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = "Could not create the OptiScaler extraction folder.";
        return false;
    }
    std::string archive_bytes;
    const bool archive_valid = read_text_file(archive, archive_bytes) &&
        _stricmp(sha256_bytes(archive_bytes.data(), archive_bytes.size()).c_str(), digest.c_str()) == 0;
    if (!archive_valid) {
        const HttpResult response = http_get_url(url, 128 * 1024 * 1024, 180000,
            L"User-Agent: HubcapUI\r\nAccept: application/octet-stream\r\n");
        if (!response.error.empty() || response.status < 200 || response.status >= 300 ||
            _stricmp(sha256_bytes(response.body.data(), response.body.size()).c_str(), digest.c_str()) != 0) {
            error = response.error.empty() ? "The OptiScaler download failed GitHub SHA-256 verification." : response.error;
            return false;
        }
        DeleteFileW(candidate.c_str());
        if (!write_file_atomic(candidate, response.body.data(), response.body.size()) ||
            !MoveFileExW(candidate.c_str(), archive.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(candidate.c_str());
            error = "Could not cache the verified OptiScaler package.";
            return false;
        }
    }
    if (!run_archive_extract(archive, extracted, error)) return false;
    std::string dll;
    if (!read_text_file(join(extracted, L"OptiScaler.dll"), dll) || !valid_amd64_dll(dll)) {
        error = "The verified OptiScaler package did not contain a valid AMD64 OptiScaler DLL.";
        return false;
    }
    const std::string marker = version + "\n" + digest;
    if (!write_file_atomic(join(base, L"cache.txt"), marker.data(), marker.size())) {
        error = "Could not save the OptiScaler cache version.";
        return false;
    }
    return true;
}

bool copy_optiscaler_file(const std::wstring& source, const std::wstring& target, std::string& error)
{
    std::string bytes;
    if (!read_text_file(source, bytes) || !write_file_atomic(target, bytes.data(), bytes.size())) {
        error = "Could not copy an OptiScaler runtime file into the game folder.";
        return false;
    }
    return true;
}

bool optipatcher_supports(const std::wstring& executable)
{
    const HttpResult response = http_get_url(
        "https://raw.githubusercontent.com/optiscaler/OptiPatcher/main/OptiPatcher/dllmain.cpp",
        4 * 1024 * 1024, 30000, L"User-Agent: HubcapUI\r\n");
    if (!response.error.empty() || response.status < 200 || response.status >= 300) return false;
    std::string filename = narrow(executable.substr(executable.find_last_of(L"\\/") + 1));
    std::transform(filename.begin(), filename.end(), filename.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    size_t cursor = 0;
    while ((cursor = response.body.find("CHECK_UE", cursor)) != std::string::npos) {
        const size_t open = response.body.find('(', cursor + 8), close = open == std::string::npos ? open : response.body.find(')', open + 1);
        if (open == std::string::npos || close == std::string::npos) break;
        std::string base = trim(response.body.substr(open + 1, close - open - 1));
        if (!base.empty() && std::all_of(base.begin(), base.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; })) {
            std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (filename == base + "-win64-shipping.exe" || filename == base + "-wingdk-shipping.exe") return true;
        }
        cursor = close + 1;
    }
    cursor = 0;
    while ((cursor = response.body.find("exeName", cursor)) != std::string::npos) {
        const size_t equality = response.body.find("==", cursor + 7);
        if (equality == std::string::npos || equality - cursor > 48) { cursor += 7; continue; }
        size_t quote = equality + 2;
        while (quote < response.body.size() && std::isspace(static_cast<unsigned char>(response.body[quote]))) ++quote;
        if (quote < response.body.size() && response.body[quote] == 'L') ++quote;
        if (quote >= response.body.size() || (response.body[quote] != '\"' && response.body[quote] != '\'')) {
            cursor = equality + 2; continue;
        }
        const char delimiter = response.body[quote++];
        const size_t end = response.body.find(delimiter, quote);
        if (end == std::string::npos) break;
        std::string candidate = response.body.substr(quote, end - quote);
        std::transform(candidate.begin(), candidate.end(), candidate.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (candidate == filename) return true;
        cursor = end + 1;
    }
    return false;
}

bool prepare_optipatcher(std::wstring& cached)
{
    const HttpResult release = http_get_url(
        "https://api.github.com/repos/optiscaler/OptiPatcher/releases/tags/rolling",
        2 * 1024 * 1024, 30000, L"User-Agent: HubcapUI\r\nAccept: application/vnd.github+json\r\n");
    if (!release.error.empty() || release.status < 200 || release.status >= 300) return false;
    yyjson_doc* document = yyjson_read(release.body.data(), release.body.size(), 0);
    yyjson_val* root = document ? yyjson_doc_get_root(document) : nullptr;
    yyjson_val* assets = root ? yyjson_obj_get(root, "assets") : nullptr;
    std::string url, digest;
    size_t index = 0, maximum = 0;
    yyjson_val* asset = nullptr;
    yyjson_arr_foreach(assets, index, maximum, asset) {
        const char* name = yyjson_get_str(yyjson_obj_get(asset, "name"));
        if (!name || _stricmp(name, "OptiPatcher.asi") != 0) continue;
        const char* download = yyjson_get_str(yyjson_obj_get(asset, "browser_download_url"));
        const char* asset_digest = yyjson_get_str(yyjson_obj_get(asset, "digest"));
        if (download) url = download;
        if (asset_digest) digest = asset_digest;
        break;
    }
    if (document) yyjson_doc_free(document);
    if (url.empty() || !sha256_digest(digest)) return false;
    digest.erase(0, 7);
    const std::wstring base = join(join(local_app_data(), L"HubcapUI"), L"optiscaler");
    cached = join(base, L"OptiPatcher.asi");
    std::string bytes;
    if (read_text_file(cached, bytes) && valid_amd64_dll(bytes) &&
        _stricmp(sha256_bytes(bytes.data(), bytes.size()).c_str(), digest.c_str()) == 0) return true;
    const HttpResult download = http_get_url(url, 16 * 1024 * 1024, 60000,
        L"User-Agent: HubcapUI\r\nAccept: application/octet-stream\r\n");
    if (!download.error.empty() || download.status < 200 || download.status >= 300 ||
        !valid_amd64_dll(download.body) ||
        _stricmp(sha256_bytes(download.body.data(), download.body.size()).c_str(), digest.c_str()) != 0) return false;
    return write_file_atomic(cached, download.body.data(), download.body.size());
}

std::string handle_optiscaler_install(const std::string& id, const ResolvedApp& app)
{
    if (optiscaler_installation_state(app)) {
        return reply_ok(id, "optiscalerInstalled", "{\"appId\":" + json_escape(app.app_id) +
            ",\"gameName\":" + json_escape(app.game_name) + ",\"statusText\":\"OptiScaler is already enabled.\"}");
    }
    OptiScalerTarget target;
    if (!resolve_steam_game_dir(app.app_id, target.game_dir) ||
        !select_game_executable(target.game_dir, app.game_name, target.executable)) {
        return reply_error(id, "optiscaler.noExecutable", "Hubcap could not safely identify the game executable for OptiScaler.");
    }
    if (!executable_is_64_bit(target.executable)) {
        return reply_error(id, "optiscaler.unsupportedArchitecture", "OptiScaler requires the detected game executable to be 64-bit.");
    }
    target.install_dir = target.executable.substr(0, target.executable.find_last_of(L"\\/"));
    if (GetFileAttributesW(join(target.install_dir, L"OptiScaler.ini").c_str()) != INVALID_FILE_ATTRIBUTES) {
        return reply_error(id, "optiscaler.existingInstall", "An unmanaged OptiScaler installation already exists in the selected game folder.");
    }
    std::wstring package;
    std::string version, error;
    if (!prepare_optiscaler_package(package, version, error)) return reply_error(id, "optiscaler.downloadFailed", error);
    const std::wstring dxgi = join(target.install_dir, L"dxgi.dll");
    const std::wstring chained_reshade = join(target.install_dir, L"ReShade64.dll");
    if (GetFileAttributesW(dxgi.c_str()) == INVALID_FILE_ATTRIBUTES) target.proxy_name = L"dxgi.dll";
    else if (GetFileAttributesW(join(target.install_dir, L"ReShade.ini").c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(chained_reshade.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!MoveFileExW(dxgi.c_str(), chained_reshade.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return reply_error(id, "optiscaler.reshadeChainFailed", "Could not prepare the existing ReShade installation for OptiScaler chaining.");
        }
        target.proxy_name = L"dxgi.dll";
        target.reshade_chained = true;
    } else {
        for (const wchar_t* candidate : {L"winmm.dll", L"version.dll", L"dbghelp.dll", L"d3d12.dll", L"wininet.dll", L"winhttp.dll"}) {
            if (GetFileAttributesW(join(target.install_dir, candidate).c_str()) == INVALID_FILE_ATTRIBUTES) {
                target.proxy_name = candidate;
                break;
            }
        }
        if (target.proxy_name.empty()) {
            return reply_error(id, "optiscaler.proxyConflict", "The game folder already contains every safe OptiScaler proxy filename.");
        }
    }
    std::vector<std::wstring> created, relative_files;
    const auto rollback = [&]() {
        for (auto iterator = created.rbegin(); iterator != created.rend(); ++iterator) DeleteFileW(iterator->c_str());
        RemoveDirectoryW(join(target.install_dir, L"plugins").c_str());
        RemoveDirectoryW(join(target.install_dir, L"D3D12_Optiscaler").c_str());
        if (target.reshade_chained && GetFileAttributesW(dxgi.c_str()) == INVALID_FILE_ATTRIBUTES)
            MoveFileExW(chained_reshade.c_str(), dxgi.c_str(), MOVEFILE_WRITE_THROUGH);
    };
    const std::wstring proxy = join(target.install_dir, target.proxy_name);
    if (!copy_optiscaler_file(join(package, L"OptiScaler.dll"), proxy, error)) {
        rollback(); return reply_error(id, "optiscaler.installFailed", error);
    }
    created.push_back(proxy); relative_files.push_back(target.proxy_name);
    std::string ini;
    if (!read_text_file(join(package, L"OptiScaler.ini"), ini)) {
        rollback(); return reply_error(id, "optiscaler.installFailed", "The OptiScaler configuration template was missing.");
    }
    const LauncherSettings launcher_settings = read_launcher_settings();
    const std::string detected_gpu = detected_gpu_vendor();
    const std::string gpu = launcher_settings.optiscaler_gpu == "Auto" ? detected_gpu : launcher_settings.optiscaler_gpu;
    std::wstring optipatcher_cache;
    const bool use_optipatcher = (gpu == "AMD" || gpu == "Intel") &&
        optipatcher_supports(target.executable) && prepare_optipatcher(optipatcher_cache);
    if (gpu == "NVIDIA") replace_all(ini, "Dxgi=auto", "Dxgi=false");
    else if (gpu == "AMD" || gpu == "Intel") replace_all(ini, "Dxgi=auto", "Dxgi=true");
    if (launcher_settings.optiscaler_upscaler == "FSR31") {
        replace_all(ini, "Dx11Upscaler=auto", "Dx11Upscaler=fsr31_12");
        replace_all(ini, "Dx12Upscaler=auto", "Dx12Upscaler=fsr31");
        replace_all(ini, "VulkanUpscaler=auto", "VulkanUpscaler=fsr31_12");
    } else if (launcher_settings.optiscaler_upscaler == "XeSS") {
        replace_all(ini, "Dx11Upscaler=auto", "Dx11Upscaler=xess_12");
        replace_all(ini, "Dx12Upscaler=auto", "Dx12Upscaler=xess");
        replace_all(ini, "VulkanUpscaler=auto", "VulkanUpscaler=xess");
    } else if (launcher_settings.optiscaler_upscaler == "DLSS") {
        replace_all(ini, "Dx11Upscaler=auto", "Dx11Upscaler=dlss");
        replace_all(ini, "Dx12Upscaler=auto", "Dx12Upscaler=dlss");
        replace_all(ini, "VulkanUpscaler=auto", "VulkanUpscaler=dlss");
    }
    if (use_optipatcher) {
        replace_all(ini, "LoadAsiPlugins=auto", "LoadAsiPlugins=true");
        replace_all(ini, "Dxgi=true", "Dxgi=false");
    }
    if (target.reshade_chained) replace_all(ini, "LoadReshade=auto", "LoadReshade=true");
    const std::wstring ini_path = join(target.install_dir, L"OptiScaler.ini");
    if (!write_file_atomic(ini_path, ini.data(), ini.size())) {
        rollback(); return reply_error(id, "optiscaler.installFailed", "Could not write OptiScaler.ini.");
    }
    created.push_back(ini_path); relative_files.push_back(L"OptiScaler.ini");
    const std::vector<std::wstring> runtime_files = {
        L"fakenvapi.ini", L"amd_fidelityfx_dx12.dll", L"amd_fidelityfx_framegeneration_dx12.dll",
        L"amd_fidelityfx_upscaler_dx12.dll", L"amd_fidelityfx_vk.dll", L"dlssg_to_fsr3_amd_is_better.dll",
        L"fakenvapi.dll", L"libxell.dll", L"libxess.dll", L"libxess_dx11.dll", L"libxess_fg.dll"
    };
    for (const auto& name : runtime_files) {
        const std::wstring destination = join(target.install_dir, name);
        if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (!copy_optiscaler_file(join(package, name), destination, error)) {
            rollback(); return reply_error(id, "optiscaler.installFailed", error);
        }
        created.push_back(destination); relative_files.push_back(name);
    }
    const std::wstring d3d12_dir = join(target.install_dir, L"D3D12_Optiscaler");
    const std::wstring d3d12_core = join(d3d12_dir, L"D3D12Core.dll");
    if (GetFileAttributesW(d3d12_core.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(d3d12_dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            rollback(); return reply_error(id, "optiscaler.installFailed", "Could not create OptiScaler's D3D12 runtime folder.");
        }
        if (!copy_optiscaler_file(join(package, L"D3D12_Optiscaler\\D3D12Core.dll"), d3d12_core, error)) {
            rollback(); return reply_error(id, "optiscaler.installFailed", error);
        }
        created.push_back(d3d12_core); relative_files.push_back(L"D3D12_Optiscaler\\D3D12Core.dll");
    }
    if (use_optipatcher) {
        const std::wstring plugins = join(target.install_dir, L"plugins");
        const std::wstring patcher = join(plugins, L"OptiPatcher.asi");
        if (!CreateDirectoryW(plugins.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            rollback(); return reply_error(id, "optiscaler.installFailed", "Could not create OptiScaler's plugin folder.");
        }
        if (!copy_optiscaler_file(optipatcher_cache, patcher, error)) {
            rollback(); return reply_error(id, "optiscaler.installFailed", error);
        }
        created.push_back(patcher); relative_files.push_back(L"plugins\\OptiPatcher.asi");
    }
    std::string marker = "version=" + version + "\nproxy=" + narrow(target.proxy_name) +
        "\nreshadeChained=" + (target.reshade_chained ? "1" : "0") + "\ngpu=" + gpu + "\n";
    for (const auto& name : relative_files) marker += "file=" + narrow(name) + "\n";
    if (!write_file_atomic(optiscaler_marker(target.install_dir), marker.data(), marker.size())) {
        rollback(); return reply_error(id, "optiscaler.installFailed", "Could not write the OptiScaler install manifest.");
    }
    const std::wstring index_directory = optiscaler_index_directory();
    const std::string indexed_path = narrow(target.install_dir);
    if ((!CreateDirectoryW(index_directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) ||
        !write_file_atomic(optiscaler_index_path(app.app_id), indexed_path.data(), indexed_path.size())) {
        DeleteFileW(optiscaler_marker(target.install_dir).c_str());
        rollback();
        return reply_error(id, "optiscaler.installFailed", "Could not save the OptiScaler installation index.");
    }
    return reply_ok(id, "optiscalerInstalled", "{\"appId\":" + json_escape(app.app_id) +
        ",\"gameName\":" + json_escape(app.game_name) + ",\"version\":" + json_escape(version) +
        ",\"gpuVendor\":" + json_escape(gpu) + ",\"proxy\":" + json_escape(narrow(target.proxy_name)) +
        ",\"optipatcher\":" + (use_optipatcher ? "true" : "false") +
        ",\"reshadeChained\":" + (target.reshade_chained ? "true" : "false") +
        ",\"statusText\":\"OptiScaler was installed successfully.\"}");
}

std::string handle_optiscaler_remove(const std::string& id, const ResolvedApp& app)
{
    OptiScalerTarget target;
    std::vector<std::wstring> files;
    if (!optiscaler_installation_state(app, &target, &files, false)) {
        return reply_ok(id, "optiscalerRemoved", "{\"appId\":" + json_escape(app.app_id) +
            ",\"gameName\":" + json_escape(app.game_name) + ",\"statusText\":\"OptiScaler is already off.\"}");
    }
    for (const auto& path : files) {
        if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            return reply_error(id, "optiscaler.cleanupFailed", "An OptiScaler file could not be removed. Close the game and try again.");
        }
    }
    if (target.reshade_chained) {
        const std::wstring reshade = join(target.install_dir, L"ReShade64.dll");
        const std::wstring dxgi = join(target.install_dir, L"dxgi.dll");
        if (GetFileAttributesW(reshade.c_str()) != INVALID_FILE_ATTRIBUTES &&
            !MoveFileExW(reshade.c_str(), dxgi.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return reply_error(id, "optiscaler.reshadeRestoreFailed", "OptiScaler was removed, but ReShade could not be restored as dxgi.dll.");
        }
    }
    DeleteFileW(join(target.install_dir, L"OptiScaler.log").c_str());
    DeleteFileW(join(target.install_dir, L"fakenvapi.log").c_str());
    DeleteFileW(join(target.install_dir, L"dlssg_to_fsr3.log").c_str());
    DeleteFileW(optiscaler_marker(target.install_dir).c_str());
    DeleteFileW(optiscaler_index_path(app.app_id).c_str());
    RemoveDirectoryW(join(target.install_dir, L"D3D12_Optiscaler").c_str());
    RemoveDirectoryW(join(target.install_dir, L"plugins").c_str());
    return reply_ok(id, "optiscalerRemoved", "{\"appId\":" + json_escape(app.app_id) +
        ",\"gameName\":" + json_escape(app.game_name) + ",\"statusText\":\"OptiScaler was removed.\"}");
}

std::string handle_library(const std::string& id, const ResolvedApp& app)
{
    const std::wstring uri = L"steam://nav/games/details/" + widen(app.app_id);
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        return reply_error(id, "steam.navigationFailed", "Could not open this game in the Steam Library.");
    }
    return reply_ok(id, "navigation", "{}");
}

std::string library_state_json(const ResolvedApp& app, bool exists, bool removed, bool online_fix = false,
    const std::string& text = "", const std::string& tone = "idle")
{
    std::wstring game_dir;
    const bool game_installed = numeric_app_id(app.app_id) && resolve_steam_game_dir(app.app_id, game_dir);
    const bool optiscaler_installed = game_installed && optiscaler_installation_state(app);
    const bool optiscaler_managed = game_installed && optiscaler_installation_state(app, nullptr, nullptr, false);
    const LauncherSettings launcher_settings = read_launcher_settings();
    return "{\"appId\":" + json_escape(app.app_id) + ",\"gameName\":" + json_escape(app.game_name) +
        ",\"exists\":" + (exists ? "true" : "false") + ",\"removed\":" + (removed ? "true" : "false") +
        ",\"gameInstalled\":" + (game_installed ? "true" : "false") +
        ",\"optiscalerAvailable\":" + (game_installed ? "true" : "false") +
        ",\"optiscalerInstalled\":" + (optiscaler_installed ? "true" : "false") +
        ",\"optiscalerManaged\":" + (optiscaler_managed ? "true" : "false") +
        ",\"optiscalerGpu\":" + json_escape(launcher_settings.optiscaler_gpu) +
        ",\"optiscalerDetectedGpu\":" + json_escape(detected_gpu_vendor()) +
        ",\"optiscalerUpscaler\":" + json_escape(launcher_settings.optiscaler_upscaler) +
        ",\"onlineFixEnabled\":" + (online_fix ? "true" : "false") +
        ",\"statusText\":" + json_escape(text) + ",\"statusTone\":" + json_escape(tone) +
        reno_state_fields(app) + "}";
}

std::string handle_library_state(const std::string& id, const ResolvedApp& app)
{
    const Config config = read_config();
    if (!config.error.empty()) return reply_ok(id, "libraryState", library_state_json(app, false, false, false, config.error, "error"));
    std::string text;
    read_text_file(config.path, text);
    return reply_ok(id, "libraryState", library_state_json(
        app, lua_exists(config, app.app_id), false, online_fix_enabled(text, app.app_id)));
}

std::string handle_library_remove(const std::string& id, const ResolvedApp& app)
{
    const Config config = read_config();
    if (!config.error.empty()) return reply_ok(id, "libraryState", library_state_json(app, false, false, false, config.error, "error"));
    std::string config_text;
    if (!read_text_file(config.path, config_text)) {
        return reply_ok(id, "libraryState", library_state_json(app, lua_exists(config, app.app_id), false, false,
            "Could not read HubcapTools config.", "error"));
    }
    const bool online_fix = online_fix_enabled(config_text, app.app_id);
    if (online_fix) {
        std::string mutation_error;
        const bool saved = mutate_text_file_atomic(config.path, [&](std::string& text) {
            set_online_fix(text, app.app_id, false, "", mutation_error);
        });
        std::string persisted;
        const bool reread = read_text_file(config.path, persisted);
        const bool persisted_online_fix = reread && online_fix_enabled(persisted, app.app_id);
        if (!saved || !mutation_error.empty() || !reread || persisted_online_fix) {
            const std::string message = !mutation_error.empty() ? mutation_error :
                "Could not disable Multiplayer Mode before removing Lua.";
            return reply_ok(id, "libraryState", library_state_json(app, lua_exists(config, app.app_id), false,
                persisted_online_fix, message, "error"));
        }
    }
    const std::wstring lua = join(config.lua_dir, widen(app.app_id + ".lua"));
    if (!DeleteFileW(lua.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        bool restored_online_fix = false;
        if (online_fix) {
            std::string rollback_error;
            const bool restored = mutate_text_file_atomic(config.path, [&](std::string& text) {
                set_online_fix(text, app.app_id, true, "", rollback_error);
            });
            std::string persisted;
            restored_online_fix = restored && rollback_error.empty() && read_text_file(config.path, persisted) &&
                online_fix_enabled(persisted, app.app_id);
        }
        const std::string message = !online_fix ? "Could not remove the Lua file." :
            (restored_online_fix ? "Could not remove the Lua file. Multiplayer Mode was restored." :
                "Could not remove the Lua file, and Multiplayer Mode could not be restored.");
        return reply_ok(id, "libraryState", library_state_json(app, true, false, restored_online_fix, message, "error"));
    }
    const std::wstring root = module_root();
    DeleteFileW(join(root, L"config\\hubcapplugin-manifest-" + widen(app.app_id) + L".txt").c_str());
    DeleteFileW(join(root, L"depotcache\\.hubcapmanifest-" + widen(app.app_id)).c_str());
    return reply_ok(id, "libraryState", library_state_json(app, false, true, false, "", "success"));
}

std::string handle_library_check(const std::string& id, const ResolvedApp& app)
{
    const Config config = read_config();
    if (!config.error.empty()) return reply_ok(id, "libraryState", library_state_json(app, false, false, false, config.error, "error"));
    std::string config_text;
    read_text_file(config.path, config_text);
    const bool online_fix = online_fix_enabled(config_text, app.app_id);
    const std::wstring lua = join(config.lua_dir, widen(app.app_id + ".lua"));
    if (GetFileAttributesW(lua.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return reply_ok(id, "libraryState", library_state_json(app, false, false, online_fix, "Lua file no longer exists.", "error"));
    }
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", lua.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        return reply_ok(id, "libraryState", library_state_json(app, true, false, online_fix, "Could not open the Lua file.", "error"));
    }
    return reply_ok(id, "libraryState", library_state_json(app, true, false, online_fix));
}

std::string handle_library_online_fix(const std::string& id, const ResolvedApp& app, bool enabled)
{
    hubcap_runtime_log(("Multiplayer Mode request received for app " + app.app_id +
        ": " + (enabled ? "enable" : "disable") + ".").c_str());
    const Config config = read_config();
    if (!config.error.empty()) {
        hubcap_runtime_log(("Multiplayer Mode request rejected: " + config.error).c_str());
        return reply_ok(id, "libraryState", library_state_json(app, false, false, false, config.error, "error"));
    }
    std::string initial;
    if (!read_text_file(config.path, initial)) {
        hubcap_runtime_log("Multiplayer Mode request failed: HubcapTools config could not be read.");
        return reply_ok(id, "libraryState", library_state_json(app, lua_exists(config, app.app_id), false, false,
            "Could not read HubcapTools config.", "error"));
    }
    std::string mutation_error;
    const bool saved = mutate_text_file_atomic(config.path, [&](std::string& text) {
        set_online_fix(text, app.app_id, enabled, "", mutation_error);
    });

    std::string persisted;
    const bool reread = read_text_file(config.path, persisted);
    const bool persisted_enabled = reread && online_fix_enabled(persisted, app.app_id);
    if (!saved || !mutation_error.empty() || !reread || persisted_enabled != enabled) {
        const std::string message = !mutation_error.empty() ? mutation_error :
            (!saved ? "Could not save Multiplayer Mode." : "Multiplayer Mode could not be verified after saving.");
        hubcap_runtime_log(("Multiplayer Mode request failed for app " + app.app_id + ": " + message +
            " saved=" + (saved ? "true" : "false") + " reread=" + (reread ? "true" : "false") +
            " persisted=" + (persisted_enabled ? "true" : "false") + ".").c_str());
        return reply_ok(id, "libraryState", library_state_json(app, lua_exists(config, app.app_id), false,
            persisted_enabled, message, "error"));
    }
    hubcap_runtime_log(("Multiplayer Mode request completed for app " + app.app_id +
        ": " + (persisted_enabled ? "enabled" : "disabled") + ".").c_str());
    return reply_ok(id, "libraryState", library_state_json(app, lua_exists(config, app.app_id), false, persisted_enabled));
}

std::string handle_multiplayer_mode_list(const std::string& id)
{
    const Config config = read_config();
    if (!config.error.empty()) return reply_error(id, "config.invalid", config.error);
    std::string text;
    if (!read_text_file(config.path, text)) return reply_error(id, "config.readFailed", "Could not read HubcapTools config.");
    return reply_ok(id, "multiplayerModeList", "{\"appIds\":" + app_ids_json(multiplayer_mode_app_ids(text)) + "}");
}

std::string handle_reno_compatible_list(const std::string& id, bool force = false)
{
    const auto catalog = reno_catalog(force);
    AcquireSRWLockShared(&g_reno_catalog_lock);
    const auto updated_ms = g_reno_catalog_updated_ms;
    const bool refresh_failed = g_reno_catalog_failed;
    ReleaseSRWLockShared(&g_reno_catalog_lock);
    std::vector<std::string> names;
    for (const auto& mod : catalog) if (!mod.blocked && !mod.name.empty()) names.push_back(mod.name);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::vector<std::string> ids;
    std::vector<std::string> enabled_ids;
    for (const auto& library : steam_library_roots()) {
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW(join(library, L"steamapps\\appmanifest_*.acf").c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring filename = data.cFileName;
            constexpr size_t prefix_length = 12;
            constexpr size_t suffix_length = 4;
            if (filename.size() <= prefix_length + suffix_length ||
                filename.rfind(L"appmanifest_", 0) != 0 ||
                filename.substr(filename.size() - suffix_length) != L".acf") continue;
            const std::string app_id = narrow(filename.substr(prefix_length,
                filename.size() - prefix_length - suffix_length));
            if (!numeric_app_id(app_id)) continue;
            std::string manifest;
            if (!read_text_file(join(library, L"steamapps\\" + filename), manifest)) continue;
            const std::string game_name = vdf_value(manifest, "name");
            RenoMod mod;
            if (!game_name.empty() && find_reno_mod(game_name, mod)) {
                ids.push_back(app_id);
                const ResolvedApp app{app_id, app_id, game_name, "", false};
                if (reno_installation_state(app)) enabled_ids.push_back(app_id);
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::sort(enabled_ids.begin(), enabled_ids.end());
    enabled_ids.erase(std::unique(enabled_ids.begin(), enabled_ids.end()), enabled_ids.end());
    std::string names_json = "[";
    for (size_t index = 0; index < names.size(); ++index) {
        if (index) names_json += ',';
        names_json += json_escape(names[index]);
    }
    names_json += ']';
    return reply_ok(id, "renoCompatibleList", "{\"appIds\":" + app_ids_json(ids) +
        ",\"enabledAppIds\":" + app_ids_json(enabled_ids) + ",\"names\":" + names_json +
        ",\"catalogUpdatedAt\":" + std::to_string(updated_ms) +
        ",\"catalogAvailable\":" + (catalog.empty() ? "false" : "true") +
        ",\"refreshFailed\":" + (refresh_failed ? "true" : "false") + "}");
}

std::string handle_optiscaler_enabled_list(const std::string& id)
{
    std::vector<std::string> enabled_ids;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(join(optiscaler_index_directory(), L"*.txt").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            std::wstring filename = data.cFileName;
            if (filename.size() <= 4 || filename.substr(filename.size() - 4) != L".txt") continue;
            filename.resize(filename.size() - 4);
            const std::string app_id = narrow(filename);
            const ResolvedApp installed{app_id, app_id, "", "", false};
            if (numeric_app_id(app_id) && optiscaler_installation_state(installed)) enabled_ids.push_back(app_id);
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    std::sort(enabled_ids.begin(), enabled_ids.end());
    enabled_ids.erase(std::unique(enabled_ids.begin(), enabled_ids.end()), enabled_ids.end());
    return reply_ok(id, "optiscalerEnabledList", "{\"appIds\":" + app_ids_json(enabled_ids) + "}");
}

std::string handle_list(const std::string& id)
{
    const Config config = read_config();
    if (!config.error.empty()) return reply_error(id, "config.invalid", config.error);
    std::vector<std::string> ids;
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(join(config.lua_dir, L"*.lua").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = data.cFileName;
            if (name.size() > 4) {
                name.resize(name.size() - 4);
                const std::string id_value = narrow(name);
                if (numeric_app_id(id_value)) ids.push_back(id_value);
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    std::sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) { return _strtoui64(a.c_str(), nullptr, 10) < _strtoui64(b.c_str(), nullptr, 10); });
    std::string state = "{\"appIds\":[";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) state += ',';
        state += json_escape(ids[i]);
    }
    state += "]}";
    return reply_ok(id, "luaList", state);
}
} // namespace

std::string hubcap_service_handle(const std::string& payload)
{
    if (payload.size() > 64 * 1024) return reply_error("", "input.tooLarge", "Hubcap command was too large.");
    yyjson_doc* document = yyjson_read(payload.data(), payload.size(), 0);
    if (!document) return reply_error("", "input.invalidJson", "Hubcap command was invalid.");
    yyjson_val* root = yyjson_doc_get_root(document);
    const uint64_t version = yyjson_get_uint(yyjson_obj_get(root, "v"));
    const char* id_value = yyjson_get_str(yyjson_obj_get(root, "id"));
    const char* action_value = yyjson_get_str(yyjson_obj_get(root, "action"));
    const char* app_value = yyjson_get_str(yyjson_obj_get(root, "appId"));
    yyjson_val* data_value = yyjson_obj_get(root, "data");
    const char* lua_dir_value = yyjson_get_str(yyjson_obj_get(data_value, "luaDir"));
    const char* api_key_value = yyjson_get_str(yyjson_obj_get(data_value, "apiKey"));
    const char* nexus_api_key_value = yyjson_get_str(yyjson_obj_get(data_value, "nexusApiKey"));
    const char* optiscaler_gpu_value = yyjson_get_str(yyjson_obj_get(data_value, "optiscalerGpu"));
    const char* optiscaler_upscaler_value = yyjson_get_str(yyjson_obj_get(data_value, "optiscalerUpscaler"));
    yyjson_val* collection_sync_value = yyjson_obj_get(data_value, "collectionSyncEnabled");
    yyjson_val* show_reno_button_value = yyjson_obj_get(data_value, "showRenoButton");
    yyjson_val* show_optiscaler_button_value = yyjson_obj_get(data_value, "showOptiScalerButton");
    const char* collection_name_value = yyjson_get_str(yyjson_obj_get(data_value, "collectionName"));
    yyjson_val* enabled_value = yyjson_obj_get(data_value, "enabled");
    yyjson_val* settings_anchor_value = yyjson_obj_get(data_value, "settingsAnchorRight");
    yyjson_val* settings_screen_left_value = yyjson_obj_get(data_value, "settingsAnchorScreenLeft");
    yyjson_val* settings_screen_top_value = yyjson_obj_get(data_value, "settingsAnchorScreenTop");
    yyjson_val* settings_client_left_value = yyjson_obj_get(data_value, "settingsAnchorClientLeft");
    yyjson_val* settings_client_top_value = yyjson_obj_get(data_value, "settingsAnchorClientTop");
    yyjson_val* settings_prefer_source_value = yyjson_obj_get(data_value, "settingsPreferSource");
    const char* settings_surface_value = yyjson_get_str(yyjson_obj_get(data_value, "settingsSurface"));
    const char* plugin_id_value = yyjson_get_str(yyjson_obj_get(data_value, "pluginId"));
    const std::string id = id_value ? id_value : "";
    const std::string action = action_value ? action_value : "";
    const std::string app_id = app_value ? app_value : "";
    const std::string lua_dir = lua_dir_value ? lua_dir_value : "";
    const std::string api_key = api_key_value ? api_key_value : "";
    const LauncherSettings launcher_defaults = action == "saveSettings" ? read_launcher_settings() : LauncherSettings{};
    const std::string nexus_api_key = nexus_api_key_value ? nexus_api_key_value : launcher_defaults.nexus_api_key;
    const std::string optiscaler_gpu = optiscaler_gpu_value ? optiscaler_gpu_value : launcher_defaults.optiscaler_gpu;
    const std::string optiscaler_upscaler = optiscaler_upscaler_value ? optiscaler_upscaler_value : launcher_defaults.optiscaler_upscaler;
    const bool collection_sync_enabled = yyjson_get_bool(collection_sync_value);
    const bool collection_sync_valid = yyjson_is_bool(collection_sync_value);
    const bool show_reno_button = show_reno_button_value ? yyjson_get_bool(show_reno_button_value) : launcher_defaults.show_reno_button;
    const bool show_reno_button_valid = !show_reno_button_value || yyjson_is_bool(show_reno_button_value);
    const bool show_optiscaler_button = show_optiscaler_button_value ? yyjson_get_bool(show_optiscaler_button_value) : launcher_defaults.show_optiscaler_button;
    const bool show_optiscaler_button_valid = !show_optiscaler_button_value || yyjson_is_bool(show_optiscaler_button_value);
    const std::string collection_name = collection_name_value ? collection_name_value : "";
    const bool enabled = yyjson_get_bool(enabled_value);
    const bool enabled_valid = yyjson_is_bool(enabled_value);
    const long long settings_anchor_right = settings_anchor_value && yyjson_is_num(settings_anchor_value) ?
        yyjson_get_sint(settings_anchor_value) : 0;
    const long long settings_anchor_screen_left = settings_screen_left_value && yyjson_is_num(settings_screen_left_value) ? yyjson_get_sint(settings_screen_left_value) : 0;
    const long long settings_anchor_screen_top = settings_screen_top_value && yyjson_is_num(settings_screen_top_value) ? yyjson_get_sint(settings_screen_top_value) : 0;
    const long long settings_anchor_client_left = settings_client_left_value && yyjson_is_num(settings_client_left_value) ? yyjson_get_sint(settings_client_left_value) : 0;
    const long long settings_anchor_client_top = settings_client_top_value && yyjson_is_num(settings_client_top_value) ? yyjson_get_sint(settings_client_top_value) : 0;
    const bool settings_prefer_source = yyjson_is_true(settings_prefer_source_value);
    const std::string settings_surface = settings_surface_value ? settings_surface_value : "";
    const std::string plugin_id = plugin_id_value ? plugin_id_value : "";

    if (version != 1) {
        yyjson_doc_free(document);
        return reply_error(id, "input.unsupportedVersion", "Unsupported Hubcap command version.");
    }
    if (id.empty() || id.size() > 96) {
        yyjson_doc_free(document);
        return reply_error("", "input.invalidId", "Hubcap command ID was invalid.");
    }
    yyjson_doc_free(document);
    if (action == "plugins") return reply_ok(id, "plugins", hubcap_plugins_catalog_json());
    if (action == "pluginEnable") {
        if (plugin_id.empty() || !enabled_valid) {
            return reply_error(id, "input.invalidPluginSetting", "Plugin setting was invalid.");
        }
        return reply_ok(id, "pluginChanged", "{\"pluginId\":" + json_escape(plugin_id) +
            ",\"result\":" + hubcap_plugins_set_enabled(plugin_id, enabled) + "}");
    }
    if (action == "pluginCommand") {
        if (plugin_id.empty()) return reply_error(id, "input.invalidPlugin", "Plugin ID was invalid.");
        const std::string response = hubcap_plugins_handle(plugin_id, payload);
        yyjson_doc* response_document = yyjson_read(response.data(), response.size(), 0);
        if (!response_document || !yyjson_is_obj(yyjson_doc_get_root(response_document))) {
            if (response_document) yyjson_doc_free(response_document);
            return reply_error(id, "plugin.invalidResponse", "Plugin returned an invalid response.");
        }
        yyjson_doc_free(response_document);
        return reply_ok(id, "pluginResult", "{\"pluginId\":" + json_escape(plugin_id) +
            ",\"response\":" + response + "}");
    }
    if (action == "settings") return handle_settings(
        id, settings_anchor_right, settings_anchor_screen_left, settings_anchor_screen_top,
        settings_anchor_client_left, settings_anchor_client_top, settings_prefer_source, settings_surface);
    if (action == "saveSettings") return handle_save_settings(
        id, lua_dir, api_key, nexus_api_key, show_reno_button, show_reno_button_valid,
        optiscaler_gpu, optiscaler_upscaler, show_optiscaler_button, show_optiscaler_button_valid,
        collection_sync_enabled, collection_name, collection_sync_valid);
    if (action == "closeSettings") return reply_ok(id, "settingsClosed", "{}");
    if (action == "restartSteam") return handle_restart_steam(id);
    if (action == "selectLuaFolder") return handle_select_lua_folder(id, lua_dir);
    if (action == "validateApiKey") return handle_validate_api_key(id, api_key);
    if (action == "copyApiKey") return handle_copy_api_key(id);
    if (action == "openApiKeys") return handle_open_api_keys(id);
    if (action == "settingsSurfaceReady") return reply_ok(id, "storeSettingsReady", "{}");
    if (action == "uiReady") {
        hubcap_runtime_log(("Hubcap Store UI reported ready for app " + app_id + ".").c_str());
        return reply_ok(id, "uiReady", "{}");
    }
    if (action == "usage") return handle_usage(id);
    if (action == "luaList") return handle_list(id);
    if (action == "groupLuaConfig") return handle_group_lua_config(id);
    if (action == "multiplayerModeList") return handle_multiplayer_mode_list(id);
    if (action == "renoRefreshCompatibleList") return handle_reno_compatible_list(id, true);
    if (action == "renoCompatibleList") return handle_reno_compatible_list(id);
    if (action == "optiscalerEnabledList") return handle_optiscaler_enabled_list(id);
    if (action == "updateStatus") { consume_update_result(); return reply_ok(id, "updateState", update_state_json()); }
    if (action == "updateBootCheck" || action == "updateCheck") {
        if (action == "updateBootCheck") hubcap_service_start_update_boot_check();
        else start_update_check(false);
        return reply_ok(id, "updateState", update_state_json());
    }
    if (action == "updateDefer") {
        AcquireSRWLockExclusive(&g_update_lock);
        g_update_state.boot_prompt_pending = false; g_update_state.manual_prompt_pending = false; g_update_state.deferred = true;
        ReleaseSRWLockExclusive(&g_update_lock);
        return reply_ok(id, "updateState", update_state_json());
    }
    if (action == "updateShowPrompt") {
        AcquireSRWLockExclusive(&g_update_lock);
        if (!g_update_state.latest_version.empty() &&
            (g_update_state.phase == "available" || g_update_state.phase == "staged" || g_update_state.phase == "error")) {
            g_update_state.manual_prompt_pending = true;
        }
        ReleaseSRWLockExclusive(&g_update_lock);
        return reply_ok(id, "updateState", update_state_json());
    }
    if (action == "updateDownload") {
        start_update_download();
        return reply_ok(id, "updateState", update_state_json());
    }
    if (action == "updateRestart") return handle_update_restart(id);
    if (action == "disableGroupLuaIfCollection") {
        return handle_disable_group_lua_if_collection(id, collection_name);
    }
    if (!numeric_app_id(app_id)) return reply_error(id, "input.invalidAppId", "Steam app ID was invalid.");
    const ResolvedApp direct{app_id, app_id, installed_game_name(app_id), "", false};
    if (action == "libraryRoute") {
        const Config config = read_config();
        if (!config.error.empty() || lua_exists(config, app_id)) return handle_library_state(id, direct);
        return handle_library_state(id, resolve_app(app_id));
    }
    if (action == "libraryRemove") return handle_library_remove(id, direct);
    if (action == "libraryCheck") return handle_library_check(id, direct);
    if (action == "libraryOnlineFix") {
        if (!enabled_valid) return reply_error(id, "input.invalidOnlineFix", "Multiplayer Mode state was invalid.");
        return handle_library_online_fix(id, direct, enabled);
    }
    const ResolvedApp resolved = resolve_app(app_id);
    if (action == "route" || action == "refresh" || action == "state") return handle_state(id, resolved);
    if (action == "remove") return handle_remove(id, resolved);
    if (action == "download") return handle_download(id, resolved);
    if (action == "library") return handle_library(id, resolved);
    if (action == "renoInstall") return handle_reno_install(id, resolved);
    if (action == "renoInstallManual") return handle_reno_install(id, resolved, true);
    if (action == "renoRemove") return handle_reno_remove(id, resolved);
    if (action == "optiscalerInstall") return handle_optiscaler_install(id, resolved);
    if (action == "optiscalerRemove") return handle_optiscaler_remove(id, resolved);
    return reply_error(id, "input.unknownAction", "Unknown Hubcap action.");
}

std::string hubcap_service_update_state_json(void)
{
    return update_state_json();
}

void hubcap_service_start_update_boot_check(void)
{
    std::wstring command = GetCommandLineW();
    std::transform(command.begin(), command.end(), command.begin(), towlower);
    if (command.find(L"--hubcap-harness") != std::wstring::npos) return;
    consume_update_result();
    if (!g_update_boot_started.exchange(true)) start_update_check(true);
}

void hubcap_service_start_lua_watcher(void)
{
    InitOnceExecuteOnce(&g_lua_watcher_once, start_lua_watcher_once, nullptr, nullptr);
}

void hubcap_service_notify_lua_config_changed(void)
{
    hubcap_service_start_lua_watcher();
    if (g_lua_watcher_reconfigure) SetEvent(g_lua_watcher_reconfigure);
}

std::uint64_t hubcap_service_lua_generation(void)
{
    hubcap_service_start_lua_watcher();
    return g_lua_generation.load();
}

std::uint64_t hubcap_service_config_signature(void)
{
    const std::wstring path = join(module_root(), L"config\\hubcaptools\\config.yaml");
    std::string text;
    if (!read_text_file(path, text)) return 0;
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<std::uint64_t>(text.size());
    hash *= 1099511628211ull;
    return hash | 1;
}

std::uint64_t hubcap_service_non_multiplayer_config_signature(void)
{
    const std::wstring path = join(module_root(), L"config\\hubcaptools\\config.yaml");
    std::string text;
    if (!read_text_file(path, text)) return 0;
    std::string filtered;
    bool skipping_fake_app_ids = false;
    size_t line_begin = 0;
    while (line_begin <= text.size()) {
        const size_t newline = text.find('\n', line_begin);
        const size_t line_end = newline == std::string::npos ? text.size() : newline;
        std::string line = text.substr(line_begin, line_end - line_begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t first = line.find_first_not_of(" \t");
        if (skipping_fake_app_ids && first != std::string::npos && first == 0 && line[first] != '#') {
            skipping_fake_app_ids = false;
        }
        if (!skipping_fake_app_ids) {
            size_t separator = 0;
            std::string value;
            if (yaml_top_level_field(line, "FakeAppIds", separator, value)) {
                skipping_fake_app_ids = true;
            } else {
                filtered.append(text, line_begin, line_end - line_begin);
                if (newline != std::string::npos) filtered.push_back('\n');
            }
        }
        if (newline == std::string::npos) break;
        line_begin = newline + 1;
    }
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : filtered) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<std::uint64_t>(filtered.size());
    hash *= 1099511628211ull;
    return hash | 1;
}

std::vector<std::string> hubcap_service_lua_app_ids(void)
{
    hubcap_service_start_lua_watcher();
    AcquireSRWLockShared(&g_lua_snapshot_lock);
    std::vector<std::string> ids = g_lua_snapshot_ids;
    ReleaseSRWLockShared(&g_lua_snapshot_lock);
    return ids;
}
