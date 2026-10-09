#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#define HUBCAP_PLUGIN_CALL __cdecl
#else
#define HUBCAP_PLUGIN_CALL
#endif

#define HUBCAP_PLUGIN_ABI_VERSION 1u
#define HUBCAP_PLUGIN_MANIFEST_RESOURCE_ID 1

struct HubcapPluginHostV1 {
    std::uint32_t size;
    std::uint32_t abi_version;
    void (HUBCAP_PLUGIN_CALL* log)(const char* plugin_id, const char* message);
    const wchar_t* (HUBCAP_PLUGIN_CALL* steam_root)();
    const wchar_t* (HUBCAP_PLUGIN_CALL* local_app_data)();
};

struct HubcapPluginV1 {
    std::uint32_t size;
    std::uint32_t abi_version;
    const char* id;
    const char* name;
    const char* version;
    const char* description;
    const char* ui_script;
    const char* (HUBCAP_PLUGIN_CALL* handle)(const char* request_json);
    void (HUBCAP_PLUGIN_CALL* free_string)(const char* value);
    void (HUBCAP_PLUGIN_CALL* shutdown)();
};

using HubcapPluginInitializeV1Fn = bool (HUBCAP_PLUGIN_CALL*)(
    const HubcapPluginHostV1* host,
    HubcapPluginV1* plugin);

#define HUBCAP_PLUGIN_INITIALIZE_EXPORT "HubcapPluginInitializeV1"
