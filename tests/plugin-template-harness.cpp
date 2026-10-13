#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#include "hubcap_plugin.h"
#include "yyjson.h"

static_assert(sizeof(void*) == 8, "The host and plugin must use AMD64.");
static_assert(sizeof(HubcapPluginHostV1) == 32, "Host ABI layout changed.");
static_assert(sizeof(HubcapPluginV1) == 72, "Plugin ABI layout changed.");

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Json {
    yyjson_doc* document;
    explicit Json(const char* text, size_t size)
        : document(yyjson_read(text, size, 0)) {}
    ~Json() { if (document) yyjson_doc_free(document); }
    yyjson_val* root() const { return yyjson_doc_get_root(document); }
};

void require_string(yyjson_val* object, const char* key, const char* expected)
{
    const char* actual = yyjson_get_str(yyjson_obj_get(object, key));
    require(actual && expected && std::strcmp(actual, expected) == 0, key);
}

std::string read_file(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "Cannot open the source manifest.");
    return std::string(std::istreambuf_iterator<char>(file), {});
}

void check_response(const HubcapPluginV1& plugin, const char* request, bool success)
{
    const char* response = plugin.handle(request);
    require(response != nullptr, "Plugin returned a null response.");
    // Copy before freeing: no returned pointer may outlive free_string.
    const std::string copied(response);
    plugin.free_string(response);
    Json parsed(copied.data(), copied.size());
    require(parsed.document && yyjson_is_obj(parsed.root()), "Response must be a JSON object.");
    auto ok = yyjson_obj_get(parsed.root(), "ok");
    require(yyjson_is_bool(ok) && yyjson_get_bool(ok) == success, "Unexpected response ok value.");
    if (success) {
        require_string(parsed.root(), "kind", "hello");
        require_string(yyjson_obj_get(parsed.root(), "state"), "message", "Hello from a native HubcapUI plugin.");
    } else {
        auto error = yyjson_obj_get(parsed.root(), "error");
        const char* message = yyjson_get_str(yyjson_obj_get(error, "message"));
        require(message && *message, "Errors must provide error.message.");
    }
}
}

int main(int argc, char** argv)
{
    HMODULE module = nullptr;
    try {
        require(argc >= 2 && argc <= 4, "Usage: plugin-template-harness.exe DLL [ui.js] [source-manifest.json]");
        module = LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        require(module != nullptr, "Cannot load the sample DLL (check architecture and dependencies).");
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        require(dos->e_magic == IMAGE_DOS_SIGNATURE, "DLL does not have a PE DOS header.");
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(module) + dos->e_lfanew);
        require(nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64,
            "DLL must be an AMD64 PE image.");

        auto initialize = reinterpret_cast<HubcapPluginInitializeV1Fn>(GetProcAddress(module, HUBCAP_PLUGIN_INITIALIZE_EXPORT));
        require(initialize != nullptr, "DLL must export undecorated HubcapPluginInitializeV1.");

        HubcapPluginHostV1 host{};
        host.size = sizeof(host);
        host.abi_version = HUBCAP_PLUGIN_ABI_VERSION;
        HubcapPluginV1 plugin{};
        plugin.size = sizeof(plugin);
        require(!initialize(nullptr, &plugin), "Initializer accepted a null host.");
        require(!initialize(&host, nullptr), "Initializer accepted a null output.");
        host.abi_version = HUBCAP_PLUGIN_ABI_VERSION + 1;
        require(!initialize(&host, &plugin), "Initializer accepted an unsupported host ABI.");
        host.abi_version = HUBCAP_PLUGIN_ABI_VERSION;
        host.size = sizeof(host) - 1;
        require(!initialize(&host, &plugin), "Initializer accepted a truncated host structure.");
        host.size = sizeof(host);
        plugin.size = sizeof(plugin) - 1;
        require(!initialize(&host, &plugin), "Initializer accepted a truncated plugin structure.");
        plugin.size = sizeof(plugin);
        require(initialize(&host, &plugin), "Initializer rejected a compatible host with optional callbacks absent.");
        require(plugin.size == sizeof(plugin) && plugin.abi_version == HUBCAP_PLUGIN_ABI_VERSION,
            "Plugin returned an incompatible ABI.");
        require(plugin.id && plugin.name && plugin.version && plugin.description, "Plugin metadata is incomplete.");
        require(std::strcmp(plugin.id, "example.hello-hubcap") == 0, "Unexpected sample plugin id.");
        require(std::strcmp(plugin.name, "Hello Hubcap") == 0, "Unexpected sample plugin name.");
        require(std::strcmp(plugin.version, "0.1.0") == 0, "Unexpected sample version.");
        require(plugin.handle && plugin.free_string && plugin.ui_script && *plugin.ui_script,
            "Plugin must supply handle, free_string, and its UI script.");

        HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(HUBCAP_PLUGIN_MANIFEST_RESOURCE_ID), MAKEINTRESOURCEW(10));
        require(resource != nullptr, "DLL must contain RCDATA resource 1.");
        HGLOBAL loaded = LoadResource(module, resource);
        const char* bytes = static_cast<const char*>(LockResource(loaded));
        const DWORD length = SizeofResource(module, resource);
        require(bytes && length, "Embedded manifest is empty.");
        Json manifest(bytes, length);
        require(manifest.document && yyjson_is_obj(manifest.root()), "Embedded manifest must be a JSON object.");
        require(yyjson_get_uint(yyjson_obj_get(manifest.root(), "schemaVersion")) == 1, "Unsupported manifest schema.");
        require_string(manifest.root(), "id", plugin.id);
        require_string(manifest.root(), "name", plugin.name);
        require_string(manifest.root(), "version", plugin.version);
        require_string(manifest.root(), "description", plugin.description);
        if (argc >= 4) {
            const auto source = read_file(argv[3]);
            require(source.size() == length && std::memcmp(source.data(), bytes, length) == 0,
                "Embedded manifest differs from src/plugin.json.");
        }

        check_response(plugin, R"({"v":1,"id":"test-hello","action":"pluginCommand","appId":"123","data":{"pluginId":"example.hello-hubcap","pluginAction":"hello","pluginData":{"appId":"123"}}})", true);
        check_response(plugin, R"({"v":1,"id":"test-state","action":"pluginCommand","appId":"123","data":{"pluginId":"example.hello-hubcap","pluginAction":"state","pluginData":{"appId":"123"}}})", true);
        check_response(plugin, nullptr, false);
        check_response(plugin, "{broken", false);
        check_response(plugin, "null", false);
        check_response(plugin, "[]", false);
        check_response(plugin, "{}", false);
        check_response(plugin, R"({"data":{"pluginAction":42}})", false);
        check_response(plugin, R"({"data":{"pluginAction":"unknown"}})", false);
        plugin.free_string(nullptr);

        if (argc >= 3) {
            std::ofstream script(argv[2], std::ios::binary);
            require(script.good(), "Cannot open UI script artifact.");
            script << plugin.ui_script;
            require(script.good(), "Cannot write UI script artifact.");
        }
        if (plugin.shutdown) plugin.shutdown();
        FreeLibrary(module);
        module = nullptr;
        std::cout << "Plugin template native checks passed (AMD64, ABI, manifest, requests, ownership).\n";
        return 0;
    } catch (const std::exception& error) {
        if (module) FreeLibrary(module);
        std::cerr << "Plugin template check failed: " << error.what() << '\n';
        return 1;
    }
}
