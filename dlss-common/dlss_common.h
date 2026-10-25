#pragma once

// Shared, header-only helpers for the DLSS Enabler / dlssg-to-fsr3 / DLSSTweaks
// HubcapUI plugins. Each plugin includes this directly into its single plugin.cpp
// translation unit (see docs/plugin-development.md: plugins are self-contained
// copies, not a shared library), so every function here is `inline` to avoid ODR
// trouble if that ever changes.
//
// This intentionally does NOT use the legacy "v"/"replyTo" reply envelope that
// RenoDX/OptiScaler's older feature_engine.cpp emits. ABI v1's handle() should
// return a flat {"ok":...,"kind":...,"state":{...}} or {"ok":false,"error":{...}}
// object; the host adds its own envelope (including replyTo) around it.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "miniz.h"
#include "yyjson.h"

namespace dlsscommon {

struct HttpResult {
    DWORD status = 0;
    std::string body;
    std::string error;
};

inline std::wstring widen(const std::string& value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

inline std::string narrow(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

inline std::wstring join(const std::wstring& left, const std::wstring& right)
{
    if (left.empty()) return right;
    if (right.empty()) return left;
    return left + (left.back() == L'\\' ? L"" : L"\\") + right;
}

inline std::string trim(std::string value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

inline std::wstring lower_wide(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), ::towlower);
    return value;
}

inline std::string lower_narrow(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline bool numeric_app_id(const std::string& app_id)
{
    return !app_id.empty() && app_id.size() <= 12 &&
        std::all_of(app_id.begin(), app_id.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

inline std::string json_escape(const std::string& value)
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

inline std::string reply_ok(const std::string& kind, const std::string& state_json)
{
    return "{\"ok\":true,\"kind\":" + json_escape(kind) + ",\"state\":" + state_json + "}";
}

inline std::string reply_error(const std::string& code, const std::string& message)
{
    return "{\"ok\":false,\"error\":{\"code\":" + json_escape(code) + ",\"message\":" + json_escape(message) + "}}";
}

inline long long json_integer(yyjson_val* root, const char* key)
{
    yyjson_val* value = root ? yyjson_obj_get(root, key) : nullptr;
    return value ? yyjson_get_sint(value) : 0;
}

inline std::string json_string(yyjson_val* root, const char* key)
{
    const char* value = root ? yyjson_get_str(yyjson_obj_get(root, key)) : nullptr;
    return value ? value : std::string();
}

inline bool read_text_file(const std::wstring& path, std::string& text)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file) return false;
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return static_cast<bool>(file) || file.eof();
}

inline bool write_file_atomic(const std::wstring& target, const void* data, size_t size)
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
        if (!WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) { ok = false; break; }
        cursor += written;
        remaining -= written;
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

// Creates every directory component of `path` (the full path to a directory, not a file).
inline bool create_directories(const std::wstring& path)
{
    if (path.empty()) return false;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const size_t separator = path.find_last_of(L"\\/");
    if (separator != std::wstring::npos && separator > 1) {
        if (!create_directories(path.substr(0, separator))) return false;
    }
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
}

inline bool delete_file_if_exists(const std::wstring& path)
{
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    return DeleteFileW(path.c_str()) != FALSE;
}

// -------------------------------------------------------------------------------------------
// HTTP (WinHTTP, HTTPS only)
// -------------------------------------------------------------------------------------------

inline HttpResult http_get_host(const std::wstring& host, const std::wstring& path,
    size_t maximum_bytes = 8 * 1024 * 1024, int response_timeout_ms = 20000,
    const std::wstring& extra_headers = L"")
{
    HttpResult result;
    HINTERNET session = WinHttpOpen(L"HubcapUI-DLSS-Plugins/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) { result.error = "Could not initialize HTTP."; return result; }
    WinHttpSetTimeouts(session, 10000, 10000, response_timeout_ms, response_timeout_ms);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (!request) {
        result.error = "Could not create HTTP request.";
    } else {
        if (!extra_headers.empty()) {
            WinHttpAddRequestHeaders(request, extra_headers.c_str(), static_cast<DWORD>(-1),
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
            result.error = "HTTP request failed or timed out.";
        } else {
            DWORD size = sizeof(result.status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &result.status, &size, nullptr);
            for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
                if (result.body.size() + available > maximum_bytes) { result.error = "Response was too large."; break; }
                const size_t old_size = result.body.size();
                result.body.resize(old_size + available);
                DWORD read = 0;
                if (!WinHttpReadData(request, result.body.data() + old_size, available, &read)) {
                    result.error = "Could not read HTTP response.";
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

inline HttpResult http_get_url(const std::string& url, size_t maximum_bytes = 128 * 1024 * 1024,
    int response_timeout_ms = 120000, const std::wstring& extra_headers = L"")
{
    HttpResult result;
    const std::wstring wide_url = widen(url);
    if (wide_url.empty() || wide_url.size() > 8192) { result.error = "Download URL was invalid."; return result; }
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
    if (components.lpszExtraInfo && components.dwExtraInfoLength) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    return http_get_host(host, path.empty() ? L"/" : path, maximum_bytes, response_timeout_ms, extra_headers);
}

inline std::string http_error(DWORD status)
{
    switch (status) {
    case 401: return "Invalid or missing Nexus API key.";
    case 403: return "Nexus rejected this request (premium-only download or expired key).";
    case 404: return "Nexus does not have this file.";
    case 429: return "Nexus rate limit reached. Try again later.";
    default: return "Nexus returned HTTP " + std::to_string(status) + ".";
    }
}

// -------------------------------------------------------------------------------------------
// Settings: the Nexus API key is shared with RenoDX/OptiScaler at
// %LOCALAPPDATA%\HubcapUI\settings.json ("NexusApiKey").
// -------------------------------------------------------------------------------------------

inline std::string read_nexus_api_key(const std::wstring& local_app_data)
{
    if (local_app_data.empty()) return {};
    std::string text;
    if (!read_text_file(join(local_app_data, L"HubcapUI\\settings.json"), text)) return {};
    yyjson_doc* document = yyjson_read(text.data(), text.size(), 0);
    if (!document) return {};
    yyjson_val* root = yyjson_doc_get_root(document);
    const std::string key = trim(json_string(root, "NexusApiKey"));
    yyjson_doc_free(document);
    return key;
}

// Writes NexusApiKey into the shared settings.json, preserving every other field already
// in it (CollectionSyncEnabled, CollectionName, etc.) so this plugin never clobbers
// settings some other part of Hubcap owns. Safe to call from any of several plugins that
// all read/write this same shared field: a plain read-modify-write, no cross-plugin
// coordination needed beyond that.
inline bool write_nexus_api_key(const std::wstring& local_app_data, const std::string& api_key, std::string& error)
{
    if (local_app_data.empty()) { error = "Could not determine the settings folder."; return false; }
    const std::wstring dir = join(local_app_data, L"HubcapUI");
    if (!create_directories(dir)) { error = "Could not create the HubcapUI settings folder."; return false; }
    const std::wstring path = join(dir, L"settings.json");

    std::string existing;
    read_text_file(path, existing); // missing/unreadable is fine; we start a fresh object below

    yyjson_doc* read_doc = existing.empty() ? nullptr : yyjson_read(existing.data(), existing.size(), 0);
    yyjson_mut_doc* doc = read_doc ? yyjson_doc_mut_copy(read_doc, nullptr) : nullptr;
    if (read_doc) yyjson_doc_free(read_doc);
    if (!doc) doc = yyjson_mut_doc_new(nullptr);

    yyjson_mut_val* root = yyjson_mut_doc_get_root(doc);
    if (!root || !yyjson_mut_is_obj(root)) {
        root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
    }
    yyjson_mut_obj_remove_str(root, "NexusApiKey");
    yyjson_mut_obj_add_strcpy(doc, root, "NexusApiKey", api_key.c_str());

    size_t length = 0;
    char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY, &length);
    bool ok = false;
    if (json) {
        ok = write_file_atomic(path, json, length);
        free(json);
    }
    yyjson_mut_doc_free(doc);
    if (!ok) error = "Could not save the Nexus API key.";
    return ok;
}

// Validates a Nexus API key against api.nexusmods.com. Returns an empty string on success,
// or a human-readable error otherwise.
inline std::string validate_nexus_api_key(const std::string& api_key)
{
    if (api_key.empty()) return "Enter a Nexus API key.";
    if (api_key.size() > 4096 || api_key.find_first_of("\r\n") != std::string::npos) return "That does not look like a valid Nexus API key.";
    const HttpResult response = http_get_host(L"api.nexusmods.com", L"/v1/users/validate.json", 1024 * 1024, 15000,
        L"apikey: " + widen(api_key));
    if (!response.error.empty()) return "Could not validate the Nexus API key: " + response.error;
    if (response.status == 401 || response.status == 403) return "Nexus rejected that API key.";
    if (response.status < 200 || response.status >= 300) return "Could not validate the Nexus API key (HTTP " + std::to_string(response.status) + ").";
    return {};
}

// -------------------------------------------------------------------------------------------
// Nexus Mods API (api.nexusmods.com), "site" domain (non-game-specific "Modding Tools" mods).
// -------------------------------------------------------------------------------------------

struct NexusFile {
    long long file_id = 0;
    long long uploaded_timestamp = 0;
    long long category_id = 0;
    std::string file_name;
    std::string name;
    unsigned long long size_in_bytes = 0;
};

// Lists every file Nexus has for a "site" mod. Requires the user's own Nexus API key
// (Hubcap Advanced Settings; shared with RenoDX/OptiScaler).
inline bool nexus_list_files(const std::string& api_key, const std::string& mod_id,
    std::vector<NexusFile>& files, std::string& error)
{
    if (api_key.empty()) {
        error = "Add a Nexus API key in Hubcap Advanced Settings to download from Nexus Mods.";
        return false;
    }
    const std::wstring headers = L"apikey: " + widen(api_key);
    const std::wstring path = L"/v1/games/site/mods/" + widen(mod_id) + L"/files.json";
    const HttpResult response = http_get_host(L"api.nexusmods.com", path, 4 * 1024 * 1024, 20000, headers);
    if (!response.error.empty()) { error = response.error; return false; }
    if (response.status == 401 || response.status == 403) { error = "Nexus rejected the API key."; return false; }
    if (response.status < 200 || response.status >= 300) { error = http_error(response.status); return false; }
    yyjson_doc* document = yyjson_read(response.body.data(), response.body.size(), 0);
    yyjson_val* root = document ? yyjson_doc_get_root(document) : nullptr;
    yyjson_val* array = root ? yyjson_obj_get(root, "files") : nullptr;
    size_t index = 0, maximum = 0;
    yyjson_val* file = nullptr;
    yyjson_arr_foreach(array, index, maximum, file) {
        NexusFile entry;
        entry.file_id = json_integer(file, "file_id");
        entry.uploaded_timestamp = json_integer(file, "uploaded_timestamp");
        entry.category_id = json_integer(file, "category_id");
        entry.file_name = json_string(file, "file_name");
        entry.name = json_string(file, "name");
        entry.size_in_bytes = static_cast<unsigned long long>(std::max<long long>(0, json_integer(file, "size_in_bytes")));
        if (entry.file_id > 0) files.push_back(std::move(entry));
    }
    if (document) yyjson_doc_free(document);
    if (files.empty()) { error = "Nexus did not return any downloadable files for this mod."; return false; }
    return true;
}

// Resolves the premium/API download URL for one specific file_id of a "site" mod.
inline bool nexus_download_link(const std::string& api_key, const std::string& mod_id, long long file_id,
    std::string& url, std::string& error)
{
    const std::wstring headers = L"apikey: " + widen(api_key);
    const std::wstring path = L"/v1/games/site/mods/" + widen(mod_id) + L"/files/" + std::to_wstring(file_id) + L"/download_link.json";
    const HttpResult response = http_get_host(L"api.nexusmods.com", path, 1024 * 1024, 20000, headers);
    if (response.status == 403) {
        error = "This Nexus file needs Premium (or a manual download). Nexus's non-Premium API cannot fetch a direct link for it.";
        return false;
    }
    if (!response.error.empty()) { error = response.error; return false; }
    if (response.status < 200 || response.status >= 300) { error = http_error(response.status); return false; }
    yyjson_doc* document = yyjson_read(response.body.data(), response.body.size(), 0);
    yyjson_val* root = document ? yyjson_doc_get_root(document) : nullptr;
    yyjson_val* first = root && yyjson_arr_size(root) ? yyjson_arr_get_first(root) : nullptr;
    url = json_string(first, "URI");
    if (document) yyjson_doc_free(document);
    if (url.rfind("https://", 0) != 0) { error = "Nexus returned an invalid download link."; return false; }
    return true;
}

// Picks the newest "main" file whose file_name or name contains `name_filter`
// (case-insensitive substring; empty matches any file). Used to pick between
// dlssg-to-fsr3's "Universal" and "DLSSTweaks Edition" builds, for example.
inline bool nexus_select_file(const std::vector<NexusFile>& files, const std::string& name_filter, NexusFile& selected)
{
    const std::string filter = lower_narrow(name_filter);
    bool found = false;
    for (const auto& file : files) {
        if (!filter.empty() &&
            lower_narrow(file.file_name).find(filter) == std::string::npos &&
            lower_narrow(file.name).find(filter) == std::string::npos) continue;
        const bool is_main = file.category_id == 1;
        const bool selected_is_main = found && selected.category_id == 1;
        if (!found || (!selected_is_main && is_main) || (selected_is_main == is_main && file.uploaded_timestamp >= selected.uploaded_timestamp)) {
            selected = file;
            found = true;
        }
    }
    return found;
}

// -------------------------------------------------------------------------------------------
// Zip extraction (miniz), with zip-slip protection.
// -------------------------------------------------------------------------------------------

inline bool safe_zip_entry_name(const std::string& entry)
{
    if (entry.empty() || entry.front() == '/' || entry.front() == '\\') return false;
    if (entry.find("..") != std::string::npos) return false;
    if (entry.find(':') != std::string::npos) return false;
    return true;
}

// Extracts every file in the zip to `destination_dir`, preserving the archive's relative
// folder structure. Returns the full path of each file written in `written`.
inline bool extract_zip_to_directory(const std::string& zip_bytes, const std::wstring& destination_dir,
    std::vector<std::wstring>& written, std::string& error)
{
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zip_bytes.data(), zip_bytes.size(), 0)) {
        error = "The downloaded file was not a valid ZIP archive.";
        return false;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    bool ok = true;
    for (mz_uint index = 0; ok && index < count; ++index) {
        if (mz_zip_reader_is_file_a_directory(&archive, index)) continue;
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) { error = "Could not read ZIP entry metadata."; ok = false; break; }
        const std::string entry = stat.m_filename;
        if (!safe_zip_entry_name(entry) || stat.m_is_encrypted) { error = "The downloaded ZIP contained an unsafe entry."; ok = false; break; }
        if (stat.m_uncomp_size > 256ull * 1024 * 1024) { error = "The downloaded ZIP contained an unexpectedly large file."; ok = false; break; }
        size_t size = 0;
        void* extracted = mz_zip_reader_extract_to_heap(&archive, index, &size, 0);
        if (!extracted) { error = "Could not extract a file from the downloaded ZIP."; ok = false; break; }
        std::wstring relative = widen(entry);
        std::replace(relative.begin(), relative.end(), L'/', L'\\');
        const std::wstring target = join(destination_dir, relative);
        const size_t separator = target.find_last_of(L"\\/");
        if (separator != std::wstring::npos) create_directories(target.substr(0, separator));
        const bool wrote = write_file_atomic(target, extracted, size);
        mz_free(extracted);
        if (!wrote) { error = "Could not write an extracted file into place."; ok = false; break; }
        written.push_back(target);
    }
    mz_zip_reader_end(&archive);
    return ok;
}

// -------------------------------------------------------------------------------------------
// Steam library / game resolution.
// -------------------------------------------------------------------------------------------

inline std::string vdf_unescape(const std::string& value)
{
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && (value[i + 1] == '\\' || value[i + 1] == '"')) ++i;
        result.push_back(value[i]);
    }
    return result;
}

inline std::string vdf_value(const std::string& text, const std::string& wanted)
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

inline std::vector<std::wstring> steam_library_roots(const std::wstring& steam_root)
{
    std::vector<std::wstring> roots{steam_root};
    std::string text;
    if (!read_text_file(join(steam_root, L"steamapps\\libraryfolders.vdf"), text)) return roots;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.find("\"path\"") == std::string::npos) continue;
        const std::wstring wide_path = widen(vdf_value(line, "path"));
        if (!wide_path.empty() && std::none_of(roots.begin(), roots.end(), [&](const auto& item) {
                return _wcsicmp(item.c_str(), wide_path.c_str()) == 0;
            })) roots.push_back(wide_path);
    }
    return roots;
}

inline bool resolve_steam_game_dir(const std::wstring& steam_root, const std::string& app_id, std::wstring& game_dir)
{
    for (const auto& library : steam_library_roots(steam_root)) {
        std::string manifest;
        if (!read_text_file(join(library, L"steamapps\\appmanifest_" + widen(app_id) + L".acf"), manifest)) continue;
        const std::string install_dir = vdf_value(manifest, "installdir");
        if (install_dir.empty()) continue;
        const std::wstring candidate = join(library, L"steamapps\\common\\" + widen(install_dir));
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) { game_dir = candidate; return true; }
    }
    return false;
}

inline std::string installed_game_name(const std::wstring& steam_root, const std::string& app_id)
{
    for (const auto& library : steam_library_roots(steam_root)) {
        std::string manifest;
        if (!read_text_file(join(library, L"steamapps\\appmanifest_" + widen(app_id) + L".acf"), manifest)) continue;
        const std::string name = vdf_value(manifest, "name");
        if (!name.empty()) return name;
    }
    return {};
}

inline void collect_files(const std::wstring& directory, int depth, const std::wstring& extension,
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

inline bool find_file_in_tree(const std::wstring& directory, const std::wstring& file_name, std::wstring& found)
{
    std::vector<std::pair<std::wstring, unsigned long long>> candidates;
    const size_t dot = file_name.find_last_of(L'.');
    const std::wstring extension = dot == std::wstring::npos ? L"" : file_name.substr(dot);
    collect_files(directory, 6, extension, candidates);
    for (const auto& candidate : candidates) {
        const size_t separator = candidate.first.find_last_of(L"\\/");
        const std::wstring name = candidate.first.substr(separator == std::wstring::npos ? 0 : separator + 1);
        if (_wcsicmp(name.c_str(), file_name.c_str()) == 0) { found = candidate.first; return true; }
    }
    return false;
}

inline bool select_game_executable(const std::wstring& game_dir, const std::string& game_name, std::wstring& executable)
{
    std::vector<std::pair<std::wstring, unsigned long long>> candidates;
    collect_files(game_dir, 7, L".exe", candidates);
    long long best_score = std::numeric_limits<long long>::min();
    const std::wstring normalized_title = lower_wide(widen(game_name));
    std::wstring compact_title;
    for (wchar_t c : normalized_title) if (iswalnum(c)) compact_title.push_back(c);
    for (const auto& candidate : candidates) {
        const std::wstring path = lower_wide(candidate.first);
        if (path.find(L"\\_commonredist\\") != std::wstring::npos || path.find(L"\\redist\\") != std::wstring::npos ||
            path.find(L"crashreport") != std::wstring::npos || path.find(L"unins") != std::wstring::npos ||
            path.find(L"setup") != std::wstring::npos || path.find(L"unitycrashhandler") != std::wstring::npos ||
            path.find(L"easyanticheat") != std::wstring::npos || path.find(L"battleye") != std::wstring::npos ||
            path.find(L"crashpad") != std::wstring::npos || path.find(L"cefsubprocess") != std::wstring::npos) continue;
        long long score = static_cast<long long>(std::min<unsigned long long>(candidate.second / (1024 * 1024), 500));
        if (path.find(L"shipping") != std::wstring::npos) score += 3000;
        if (path.find(L"\\binaries\\win64\\") != std::wstring::npos) score += 1000;
        else if (path.find(L"\\win64\\") != std::wstring::npos || path.find(L"\\x64\\") != std::wstring::npos) score += 450;
        if (path.find(L"launcher") != std::wstring::npos || path.find(L"bootstrap") != std::wstring::npos) score -= 1200;
        if (path.find(L"server") != std::wstring::npos || path.find(L"editor") != std::wstring::npos ||
            path.find(L"tool") != std::wstring::npos) score -= 700;
        std::wstring filename = path.substr(path.find_last_of(L"\\/") + 1);
        std::wstring compact_filename;
        for (wchar_t c : filename) if (iswalnum(c)) compact_filename.push_back(c);
        if (compact_title.size() >= 5 && compact_filename.find(compact_title) != std::wstring::npos) score += 500;
        if (score > best_score) { best_score = score; executable = candidate.first; }
    }
    return !executable.empty();
}

inline bool executable_is_64_bit(const std::wstring& path)
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

// -------------------------------------------------------------------------------------------
// Install manifests: record exactly which files a plugin wrote into a game folder, so
// "remove" only deletes what this plugin placed there, never a file that was already present.
// -------------------------------------------------------------------------------------------

inline bool write_install_manifest(const std::wstring& game_dir, const std::wstring& marker_name,
    const std::vector<std::wstring>& written_files, const std::string& version)
{
    std::string json = "{\"version\":" + json_escape(version) + ",\"files\":[";
    for (size_t i = 0; i < written_files.size(); ++i) {
        if (i) json += ",";
        json += json_escape(narrow(written_files[i]));
    }
    json += "]}";
    return write_file_atomic(join(game_dir, marker_name), json.data(), json.size());
}

inline bool read_install_manifest(const std::wstring& game_dir, const std::wstring& marker_name,
    std::vector<std::wstring>& files, std::string& version)
{
    std::string text;
    if (!read_text_file(join(game_dir, marker_name), text)) return false;
    yyjson_doc* document = yyjson_read(text.data(), text.size(), 0);
    yyjson_val* root = document ? yyjson_doc_get_root(document) : nullptr;
    version = json_string(root, "version");
    yyjson_val* array = root ? yyjson_obj_get(root, "files") : nullptr;
    size_t index = 0, maximum = 0;
    yyjson_val* entry = nullptr;
    yyjson_arr_foreach(array, index, maximum, entry) {
        const char* path = yyjson_get_str(entry);
        if (path) files.push_back(widen(path));
    }
    if (document) yyjson_doc_free(document);
    return true;
}

// Deletes every file the manifest lists plus the manifest itself. Best-effort: continues
// past individual failures so a partially-locked install can still be cleaned up mostly.
inline bool remove_installed_files(const std::wstring& game_dir, const std::wstring& marker_name)
{
    std::vector<std::wstring> files;
    std::string version;
    const bool had_manifest = read_install_manifest(game_dir, marker_name, files, version);
    bool ok = true;
    for (const auto& file : files) ok = delete_file_if_exists(file) && ok;
    ok = delete_file_if_exists(join(game_dir, marker_name)) && ok;
    return had_manifest && ok;
}

// Lists the immediate top-level folder names present in a zip (e.g. "dll_dbghelp" for an
// entry "dll_dbghelp/dbghelp.dll"). Used to detect a zip that packages several alternative
// proxy-name options as sibling folders, rather than one flat set of files.
inline std::vector<std::string> zip_top_level_folders(const std::string& zip_bytes)
{
    std::vector<std::string> folders;
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zip_bytes.data(), zip_bytes.size(), 0)) return folders;
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) continue;
        const std::string entry = stat.m_filename;
        const size_t slash = entry.find('/');
        if (slash == std::string::npos || slash == 0) continue;
        const std::string folder = entry.substr(0, slash);
        if (std::find(folders.begin(), folders.end(), folder) == folders.end()) folders.push_back(folder);
    }
    mz_zip_reader_end(&archive);
    return folders;
}

// Extracts only the entries under `folder_prefix/` (e.g. "dll_dbghelp"), stripping that
// prefix, flat into `destination_dir`. Used for a zip that packages several alternative
// proxy-name options as sibling folders: pick one folder, extract just its files.
inline bool extract_zip_folder_to_directory(const std::string& zip_bytes, const std::string& folder_prefix,
    const std::wstring& destination_dir, std::vector<std::wstring>& written, std::string& error)
{
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zip_bytes.data(), zip_bytes.size(), 0)) {
        error = "The downloaded file was not a valid ZIP archive.";
        return false;
    }
    const std::string prefix = folder_prefix + "/";
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    bool ok = true;
    for (mz_uint index = 0; ok && index < count; ++index) {
        if (mz_zip_reader_is_file_a_directory(&archive, index)) continue;
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive, index, &stat)) { error = "Could not read ZIP entry metadata."; ok = false; break; }
        const std::string entry = stat.m_filename;
        if (entry.size() <= prefix.size() || entry.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string relative_entry = entry.substr(prefix.size());
        if (!safe_zip_entry_name(relative_entry) || stat.m_is_encrypted) { error = "The downloaded ZIP contained an unsafe entry."; ok = false; break; }
        if (stat.m_uncomp_size > 256ull * 1024 * 1024) { error = "The downloaded ZIP contained an unexpectedly large file."; ok = false; break; }
        size_t size = 0;
        void* extracted = mz_zip_reader_extract_to_heap(&archive, index, &size, 0);
        if (!extracted) { error = "Could not extract a file from the downloaded ZIP."; ok = false; break; }
        std::wstring relative = widen(relative_entry);
        std::replace(relative.begin(), relative.end(), L'/', L'\\');
        const std::wstring target = join(destination_dir, relative);
        const size_t separator = target.find_last_of(L"\\/");
        if (separator != std::wstring::npos) create_directories(target.substr(0, separator));
        const bool wrote = write_file_atomic(target, extracted, size);
        mz_free(extracted);
        if (!wrote) { error = "Could not write an extracted file into place."; ok = false; break; }
        written.push_back(target);
    }
    mz_zip_reader_end(&archive);
    return ok;
}

} // namespace dlsscommon
