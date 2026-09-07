#include <windows.h>

#include <cstring>
#include <string>

#include "hubcap_plugin.h"
#include "hubcap_service.h"
#include "yyjson.h"

namespace {
const HubcapPluginHostV1* g_host = nullptr;

char* copy_result(const std::string& value)
{
    char* result = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, value.size() + 1));
    if (!result) return nullptr;
    memcpy(result, value.data(), value.size());
    result[value.size()] = '\0';
    return result;
}

const char* HUBCAP_PLUGIN_CALL handle(const char* request_json)
{
    if (!request_json) return copy_result("{\"ok\":false,\"error\":\"Missing plugin request.\"}");
    yyjson_doc* document = yyjson_read(request_json, strlen(request_json), 0);
    if (!document) return copy_result("{\"ok\":false,\"error\":\"Invalid plugin request.\"}");
    yyjson_val* root = yyjson_doc_get_root(document);
    yyjson_val* data = yyjson_obj_get(root, "data");
    const char* requested = yyjson_get_str(yyjson_obj_get(data, "pluginAction"));
    const char* app_id = yyjson_get_str(yyjson_obj_get(root, "appId"));
    const std::string app = app_id ? app_id : "";
    std::string action;
    if (requested && strcmp(requested, "state") == 0) action = "libraryRoute";
    else if (requested && strcmp(requested, "list") == 0) action = "optiscalerEnabledList";
    else if (requested && strcmp(requested, "install") == 0) action = "optiscalerInstall";
    else if (requested && strcmp(requested, "remove") == 0) action = "optiscalerRemove";
    yyjson_doc_free(document);
    if (action.empty()) return copy_result("{\"ok\":false,\"error\":\"Unknown OptiScaler plugin action.\"}");
    const std::string legacy = "{\"v\":1,\"id\":\"optiscaler-plugin\",\"action\":\"" + action +
        "\",\"appId\":\"" + app + "\",\"data\":{}}";
    return copy_result(hubcap_service_handle(legacy));
}

void HUBCAP_PLUGIN_CALL free_string(const char* value)
{
    if (value) HeapFree(GetProcessHeap(), 0, const_cast<char*>(value));
}

const char k_ui[] = R"HUBCAPPLUGIN((()=>{
const host=globalThis.HubcapPluginHost,ID='hubcap.optiscaler';if(!host||globalThis.__hubcapOptiScalerPlugin)return;globalThis.__hubcapOptiScalerPlugin=true;
const ICON='<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="hp-library-icon" style="display:block;flex:0 0 15px;height:15px;width:15px"><path d="M12 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/><path d="M14 15H9v-5"/><path d="M16 3h5v5"/><path d="M21 3 9 15"/></svg>';
let currentApp='',pending=null,enabled=new Set(),badgeQueued=false;
const command=(action,data={})=>host.send(ID,action,{...data,appId:data.appId||currentApp||host.libraryAppId()});
const applyState=state=>{if(!state?.appId)return;const app=String(state.appId),listed=enabled.has(app),managed=!!(state.optiscalerManaged||state.optiscalerInstalled||listed),show=!!(state.gameInstalled||state.optiscalerAvailable||managed);host.setGameSetting(ID,state.appId,show?{label:managed?'Remove OptiScaler':'Enable OptiScaler',icon:ICON,checked:!!(state.optiscalerInstalled||listed),onActivate:({button})=>{const remove=managed,name=state.gameName||`app ${state.appId}`,gpu=state.optiscalerGpu==='Auto'?`Auto (${state.optiscalerDetectedGpu||'Unknown'} detected)`:state.optiscalerGpu||'Auto';host.showConfirm({kind:remove?'optiscaler-remove':'online-fix',title:remove?'Remove OptiScaler?':'Enable OptiScaler?',message:remove?`Remove the Hubcap-managed OptiScaler installation from ${name}? Existing files Hubcap did not install will be preserved.`:`Install the latest verified OptiScaler release for ${name}? Hubcap will detect the game executable and configure the safest proxy automatically. GPU: ${gpu}. Compatibility varies by game; disable anti-cheat before using graphics injection mods.`,confirmLabel:remove?'Remove':'Enable',cancelLabel:'Cancel'},modal=>{button.disabled=true;host.setModalState(modal,{phase:'busy',title:remove?'Removing OptiScaler...':'Installing OptiScaler...',message:remove?'Removing the Hubcap-managed OptiScaler files.':'Downloading, detecting the game executable and GPU, then configuring OptiScaler.'});pending={state,button,modal,remove};command(remove?'remove':'install',{appId:state.appId});return false})}}:null)};
const assetId=src=>String(src||'').split('/assets/')[1]?.split('/')[0]||'';
const titleFor=(row,badge)=>{const remembered=badge?.dataset.titleText||'',status=/^(launching|running|updating|downloading|ready to launch|cloud sync|synchronizing|stopping)$/i,candidates=Array.from(row.querySelectorAll('span')).filter(x=>!x.closest('.hp-renodx-plugin-badge,.hp-optiscaler-plugin-badge,.hp-multiplayer-badge')&&x.children.length===0&&x.textContent.trim()&&!status.test(x.textContent.trim()));return candidates.find(x=>x.textContent.trim()===remembered)||candidates.sort((a,b)=>b.textContent.trim().length-a.textContent.trim().length)[0]||null};
const syncBadges=()=>{badgeQueued=false;for(const doc of host.documents()){if(doc.head&&!doc.getElementById('hubcap-optiscaler-plugin-style')){const style=doc.createElement('style');style.id='hubcap-optiscaler-plugin-style';style.textContent='.hp-optiscaler-plugin-badge{align-items:center;background:#a4d007;border:1px solid #a4d007;border-radius:2px;box-sizing:border-box;color:#1b2838;display:inline-flex;font:700 8px/10px "Motiva Sans",Arial,sans-serif;height:12px;letter-spacing:.35px;margin-left:6px;padding:0 3px;pointer-events:none;white-space:nowrap}';doc.head.append(style)}for(const row of doc.querySelectorAll('[role="gridcell"]')){const id=assetId(row.querySelector('img[src*="/assets/"]')?.getAttribute('src')),old=row.querySelector('.hp-optiscaler-plugin-badge');if(!enabled.has(id)){old?.remove();continue}const title=titleFor(row,old);if(!title)continue;const badge=old||doc.createElement('span');if(!old){badge.className='hp-optiscaler-plugin-badge';badge.textContent='OPTISCALER'}badge.dataset.titleText=title.textContent.trim();badge.title='OptiScaler enabled';const reno=row.querySelector('.hp-renodx-plugin-badge');(reno||title).insertAdjacentElement('afterend',badge)}}};
const queueBadges=()=>{if(badgeQueued)return;badgeQueued=true;requestAnimationFrame(syncBadges)};
for(const doc of host.documents()){const start=()=>{if(!doc?.documentElement)return;new MutationObserver(queueBadges).observe(doc.documentElement,{childList:true,subtree:true});queueBadges()};doc?.readyState==='loading'?doc.addEventListener('DOMContentLoaded',start,{once:true}):start()}
host.onResult(ID,response=>{if(response?.kind==='libraryState'){applyState(response.state||{});return}if(response?.kind==='optiscalerEnabledList'){enabled=new Set((response.state?.appIds||[]).map(String));queueBadges();return}if(!pending)return;const request=pending;pending=null,installed=response.ok&&response.kind==='optiscalerInstalled',removed=response.ok&&response.kind==='optiscalerRemoved',gpu=String(response.state?.gpuVendor||''),proxy=String(response.state?.proxy||''),message=!response.ok?(response.error?.message||'OptiScaler operation failed.'):removed?(response.state?.statusText||'OptiScaler was removed.'):`${response.state?.statusText||'OptiScaler was installed.'}${gpu?` GPU: ${gpu}.`:''}${proxy?` Proxy: ${proxy}.`:''}`;request.button.disabled=false;host.setModalState(request.modal,{phase:response.ok?'success':'error',title:!response.ok?'OptiScaler Failed':removed?'OptiScaler Removed':'OptiScaler Enabled',message,onClose:()=>{command('state',{appId:request.state.appId});command('list')}});if(installed||removed){command('state',{appId:request.state.appId});command('list')}});
const tick=()=>{const app=host.libraryAppId();if(app===currentApp)return;if(currentApp)host.setGameSetting(ID,currentApp,null);currentApp=app;if(app)command('state',{appId:app})};tick();setInterval(tick,300);command('list');setInterval(queueBadges,1500);
})())HUBCAPPLUGIN";
}

extern "C" void hubcap_runtime_log(const char* message)
{
    if (g_host && g_host->log) g_host->log("hubcap.optiscaler", message);
}

std::string hubcap_plugins_catalog_json() { return "{\"plugins\":[]}"; }
std::string hubcap_plugins_set_enabled(const std::string&, bool) { return "{\"ok\":false}"; }
std::string hubcap_plugins_handle(const std::string&, const std::string&) { return "{\"ok\":false}"; }
std::string hubcap_plugins_ui_script() { return {}; }

extern "C" __declspec(dllexport) bool HUBCAP_PLUGIN_CALL HubcapPluginInitializeV1(
    const HubcapPluginHostV1* host, HubcapPluginV1* plugin)
{
    if (!host || host->abi_version != HUBCAP_PLUGIN_ABI_VERSION || !plugin || plugin->size < sizeof(*plugin)) return false;
    g_host = host;
    plugin->size = sizeof(*plugin);
    plugin->abi_version = HUBCAP_PLUGIN_ABI_VERSION;
    plugin->id = "hubcap.optiscaler";
    plugin->name = "OptiScaler";
    plugin->version = "0.1.2";
    plugin->description = "Detects, installs, configures, and removes OptiScaler.";
    plugin->ui_script = k_ui;
    plugin->handle = handle;
    plugin->free_string = free_string;
    plugin->shutdown = nullptr;
    return true;
}
