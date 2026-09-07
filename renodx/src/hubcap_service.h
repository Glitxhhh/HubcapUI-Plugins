#pragma once

#include <cstdint>
#include <string>
#include <vector>

std::string hubcap_service_handle(const std::string& payload);
std::string hubcap_service_update_state_json(void);
void hubcap_service_start_lua_watcher(void);
void hubcap_service_start_update_boot_check(void);
void hubcap_service_notify_lua_config_changed(void);
std::uint64_t hubcap_service_lua_generation(void);
std::uint64_t hubcap_service_config_signature(void);
std::uint64_t hubcap_service_non_multiplayer_config_signature(void);
std::vector<std::string> hubcap_service_lua_app_ids(void);
