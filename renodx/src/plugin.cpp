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
    else if (requested && strcmp(requested, "refreshCompatibility") == 0) action = "renoRefreshCompatibleList";
    else if (requested && strcmp(requested, "list") == 0) action = "renoCompatibleList";
    else if (requested && strcmp(requested, "install") == 0) action = "renoInstall";
    else if (requested && strcmp(requested, "installManual") == 0) action = "renoInstallManual";
    else if (requested && strcmp(requested, "remove") == 0) action = "renoRemove";
    yyjson_doc_free(document);
    if (action.empty()) return copy_result("{\"ok\":false,\"error\":\"Unknown RenoDX plugin action.\"}");
    const std::string legacy = "{\"v\":1,\"id\":\"renodx-plugin\",\"action\":\"" + action +
        "\",\"appId\":\"" + app + "\",\"data\":{}}";
    return copy_result(hubcap_service_handle(legacy));
}

void HUBCAP_PLUGIN_CALL free_string(const char* value)
{
    if (value) HeapFree(GetProcessHeap(), 0, const_cast<char*>(value));
}

const char k_ui[] = R"HUBCAPPLUGIN((()=>{
const host=globalThis.HubcapPluginHost,ID='hubcap.renodx';if(!host||globalThis.__hubcapRenoDxPlugin)return;globalThis.__hubcapRenoDxPlugin=true;
const ICON='<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="hp-library-icon" style="display:block;flex:0 0 15px;height:15px;width:15px"><rect width="20" height="14" x="2" y="3" rx="2"/><line x1="8" x2="16" y1="21" y2="21"/><line x1="12" x2="12" y1="17" y2="21"/></svg>';
let currentApp='',currentState=null,pending=null,compatible=new Set(),enabled=new Set(),exactNames=new Set(),nameMatchers=[],nameCache=new Map(),badgeQueued=false;
const command=(action,data={})=>host.send(ID,action,{...data,appId:data.appId||currentApp||host.libraryAppId()});
let catalogBusy=false,catalogUpdatedAt=0,catalogFailed=false,catalogTimeout=0;
const syncCatalogControls=()=>{for(const doc of host.documents())for(const row of doc.querySelectorAll('.hp-plugin-row[data-plugin-id="hubcap.renodx"]')){let controls=row.querySelector('.hp-renodx-catalog');if(!controls){controls=doc.createElement('div');controls.className='hp-renodx-catalog';row.style.flexWrap='wrap';controls.style.cssText='flex-basis:100%;display:flex;flex-wrap:wrap;align-items:center;gap:8px;grid-column:1/-1;font-size:11px;color:#8f98a0';const button=doc.createElement('button');button.type='button';button.className='hp-icon-button';button.style.cssText='width:auto;padding:4px 8px;font-size:11px';button.onclick=e=>{e.preventDefault();e.stopPropagation();requestCatalog(true)};const status=doc.createElement('span');status.setAttribute('role','status');controls.append(button,status);row.append(controls)}const button=controls.firstElementChild,status=controls.lastElementChild,label=catalogBusy?'Refreshing...':'Refresh compatibility',text=catalogBusy?'Checking RenoDX compatibility...':(catalogUpdatedAt?`Last updated: ${new Date(catalogUpdatedAt).toLocaleString()}`:'Compatibility has not loaded yet.')+(catalogFailed?(catalogUpdatedAt?' — Refresh failed; using the previous list.':' — Refresh failed; try again.'):'');if(button.disabled!==catalogBusy)button.disabled=catalogBusy;if(button.textContent!==label)button.textContent=label;if(status.textContent!==text)status.textContent=text}};
const requestCatalog=(force=false)=>{if(catalogBusy||pending)return;catalogBusy=true;syncCatalogControls();catalogTimeout=setTimeout(()=>{catalogBusy=false;catalogFailed=true;syncCatalogControls()},45000);command(force?'refreshCompatibility':'list')};
const gameName=state=>state?.gameName||`app ${state?.appId||currentApp}`;
const normalizedName=value=>String(value||'').normalize('NFKD').replace(/[\u0300-\u036f]/g,'').toLocaleLowerCase().replace(/[^a-z0-9]+/g,' ').trim();
const nameTokens=value=>new Set(normalizedName(value).split(/\s+/).filter(Boolean).map(token=>({ii:'2',iii:'3',iv:'4',v:'5'})[token]||token).filter(token=>!['the','a','edition','tm'].includes(token)));
const nameMatches=title=>{const normalized=normalizedName(title);if(!normalized)return false;if(nameCache.has(normalized))return nameCache.get(normalized);let matched=exactNames.has(normalized);if(!matched){const left=nameTokens(normalized);if(left.size)matched=nameMatchers.some(right=>{let intersection=0;for(const token of left)if(right.has(token))intersection++;const union=left.size+right.size-intersection,jaccard=union?intersection/union:0,coverage=intersection/Math.min(left.size,right.size);return(intersection>=2&&coverage===1&&jaccard>=.66)||(intersection>=2&&jaccard>=.82)})}nameCache.set(normalized,matched);return matched};
const browser=url=>{let target;try{target=new URL(String(url||''));if(!['https:','http:'].includes(target.protocol))return false}catch{return false}for(const owner of [...host.documents().map(d=>d.defaultView),globalThis].filter(Boolean)){try{const open=owner.SteamClient?.System?.OpenInSystemBrowser;if(typeof open==='function'){open.call(owner.SteamClient.System,target.href);return true}}catch{}try{const execute=owner.SteamClient?.URL?.ExecuteSteamURL;if(typeof execute==='function'){execute.call(owner.SteamClient.URL,`steam://openurl_external/${target.href}`);return true}}catch{}}return false};
const armPending=(request,delay,title,message)=>{clearTimeout(request.timeoutId);request.timeoutId=setTimeout(()=>{if(pending!==request)return;pending=null;request.button.disabled=false;host.setModalState(request.modal,{phase:'error',title,message,onClose:()=>{command('state',{appId:request.state.appId});requestCatalog()}})},delay)};
const begin=(state,button,modal,remove=false)=>{button.disabled=true;host.setModalState(modal,{phase:'busy',title:remove?'Removing RenoDX...':state.renoDiscordOnly?'Opening RenoDX Discord...':state.renoNeedsNexus?'Opening Nexus Mods...':'Installing RenoDX...',message:remove?'Removing the RenoDX mod, its ReShade preset, and ReShade.':state.renoNeedsNexus?'Preparing the Nexus Mods download page.':'Downloading and installing RenoDX and the required ReShade build. This may take a moment.'});const request={state,button,modal,remove,timeoutId:0};pending=request;armPending(request,state.renoNeedsNexus?45000:180000,state.renoNeedsNexus?'Nexus Timed Out':'RenoDX Timed Out',state.renoNeedsNexus?'Nexus Mods did not open within 45 seconds. Close this message and try again.':'Hubcap did not receive a result within three minutes. The background operation may still finish; close this message, wait briefly, then reopen Game Settings to refresh the detected state.');command(remove?'remove':'install',{appId:state.appId});return false};
const confirm=({state,button,document:doc})=>{const remove=!!state.renoInstalled,name=gameName(state),notes=state.renoNotes?` Wiki notes: ${state.renoNotes}`:'';let options;if(remove)options={kind:'reno-remove',title:'Remove RenoDX?',message:`Remove RenoDX, its ReShade preset, and ReShade from ${name}?`,confirmLabel:'Uninstall',cancelLabel:'Cancel'};else if(state.renoConstruction)options={kind:'reno-install',title:'Under Construction',message:`This mod is still under construction. Enable it anyway?${notes}`,confirmLabel:'Enable',cancelLabel:'Cancel'};else if(state.renoDiscordOnly)options={kind:'reno-install',title:'Join RenoDX Discord?',message:`RenoDX for ${name} is only available through the RenoDX Discord. Join the Discord to download it.`,confirmLabel:'Join Discord',cancelLabel:'Cancel'};else if(state.renoNeedsNexus)options={kind:'online-fix',title:'Manual Nexus Download',message:'This RenoDX mod is hosted exclusively on Nexus Mods. Download it in the browser that opens; Hubcap will detect the correct file and install it automatically.',confirmLabel:'Open Nexus',cancelLabel:'Cancel'};else options={kind:'online-fix',title:'Install RenoDX?',message:`Install the official full-add-on ReShade build and ${state.renoName||'RenoDX'} for ${name}?${notes} Full add-on support is intended for single-player use and may trigger anti-cheat systems.`,confirmLabel:'Install',cancelLabel:'Cancel'};host.showConfirm(options,modal=>begin(state,button,modal,remove))};
const applyState=state=>{if(!state?.appId)return;currentState=state;const app=String(state.appId),available=!!state.renoAvailable||compatible.has(app)||nameMatches(state.gameName),installed=!!state.renoInstalled||enabled.has(app),effective={...state,renoAvailable:available,renoInstalled:installed};const show=available&&(!!state.gameInstalled||compatible.has(app)||installed);host.setGameSetting(ID,state.appId,show?{label:installed?'Remove RenoDX':'Enable RenoDX',icon:ICON,checked:installed,onActivate:context=>confirm({...context,state:effective})}:null)};
const assetId=src=>String(src||'').split('/assets/')[1]?.split('/')[0]||'';
const titleFor=(row,badge)=>{const remembered=badge?.dataset.titleText||'',status=/^(launching|running|updating|downloading|ready to launch|cloud sync|synchronizing|stopping)$/i,candidates=Array.from(row.querySelectorAll('span')).filter(x=>!x.closest('.hp-renodx-plugin-badge,.hp-optiscaler-plugin-badge,.hp-multiplayer-badge')&&x.children.length===0&&x.textContent.trim()&&!status.test(x.textContent.trim()));return candidates.find(x=>x.textContent.trim()===remembered)||candidates.sort((a,b)=>b.textContent.trim().length-a.textContent.trim().length)[0]||null};
const publishEnabled=()=>{const value=JSON.stringify(Array.from(enabled));for(const doc of host.documents())doc?.documentElement?.setAttribute('data-hubcap-renodx-enabled',value)};
const sharedEnabled=(doc,app)=>{try{const value=doc?.documentElement?.getAttribute('data-hubcap-renodx-enabled');return value===null?enabled.has(app):JSON.parse(value).map(String).includes(app)}catch{return enabled.has(app)}};
const setEnabledState=(app,on)=>{app=String(app||'');if(!app)return;if(on)enabled.add(app);else enabled.delete(app);publishEnabled();for(const doc of host.documents())for(const badge of doc.querySelectorAll('.hp-renodx-plugin-badge'))if(badge.dataset.appId===app){badge.dataset.enabled=on?'true':'false';badge.title=on?'RenoDX enabled':'RenoDX compatible'}queueBadges()};
const syncBadges=()=>{badgeQueued=false;syncCatalogControls();for(const doc of host.documents()){if(doc.head&&!doc.getElementById('hubcap-renodx-plugin-style')){const style=doc.createElement('style');style.id='hubcap-renodx-plugin-style';style.textContent='.hp-renodx-plugin-badge{align-items:center;background:#dcdedf;border:1px solid #dcdedf;border-radius:2px;box-sizing:border-box;color:#3d4450;display:inline-flex;font:700 8px/10px "Motiva Sans",Arial,sans-serif;height:12px;letter-spacing:.35px;margin-left:6px;opacity:.7;padding:0 3px;pointer-events:none;white-space:nowrap}.hp-renodx-plugin-badge[data-enabled="true"]{background:#a4d007;border-color:#a4d007;color:#1b2838;opacity:1}';doc.head.append(style)}for(const row of doc.querySelectorAll('[role="gridcell"]')){const id=assetId(row.querySelector('img[src*="/assets/"]')?.getAttribute('src')),old=row.querySelector('.hp-renodx-plugin-badge'),title=titleFor(row,old),titleText=title?.textContent?.trim()||old?.dataset.titleText||'',available=compatible.has(id)||nameMatches(titleText);if(!available){old?.remove();continue}if(!title)continue;const badge=old||doc.createElement('span'),on=sharedEnabled(doc,id);if(!old){badge.className='hp-renodx-plugin-badge';badge.textContent='RENODX'}badge.dataset.appId=id;badge.dataset.enabled=on?'true':'false';badge.dataset.titleText=titleText;badge.title=on?'RenoDX enabled':'RenoDX compatible';title.insertAdjacentElement('afterend',badge)}}};
const queueBadges=()=>{if(badgeQueued)return;badgeQueued=true;requestAnimationFrame(syncBadges)};
for(const doc of host.documents()){const start=()=>{if(!doc?.documentElement)return;new MutationObserver(queueBadges).observe(doc.documentElement,{childList:true,subtree:true});queueBadges()};doc?.readyState==='loading'?doc.addEventListener('DOMContentLoaded',start,{once:true}):start()}
host.onResult(ID,response=>{if(response?.kind==='libraryState'){const state=response.state||{};setEnabledState(state.appId,!!state.renoInstalled);applyState(state);return}if(response?.kind==='renoCompatibleList'){clearTimeout(catalogTimeout);catalogBusy=false;catalogUpdatedAt=Number(response.state?.catalogUpdatedAt||catalogUpdatedAt);catalogFailed=!!response.state?.refreshFailed;syncCatalogControls();if(response.state?.catalogAvailable===false)return;compatible=new Set((response.state?.appIds||[]).map(String));enabled=new Set((response.state?.enabledAppIds||[]).map(String));publishEnabled();exactNames=new Set((response.state?.names||[]).map(normalizedName).filter(Boolean));nameMatchers=Array.from(exactNames,nameTokens).filter(tokens=>tokens.size);nameCache=new Map();if(currentState)applyState(currentState);queueBadges();return}if(!pending)return;const request=pending;clearTimeout(request.timeoutId);if(response?.kind==='renoNexusManual'&&response.ok){if(browser(response.state?.url)){host.setModalState(request.modal,{phase:'busy',title:'Waiting for Nexus Download...',message:'Download the RenoDX file from the browser that opened. Hubcap is watching for the correct file for 75 seconds; if it is not detected, use the drag-and-drop fallback window.'});armPending(request,75000,'Download Not Detected','Hubcap did not detect the Nexus download within 75 seconds. Use the drag-and-drop fallback window if it opened, or close this message and try again.');command('installManual',{appId:request.state.appId});return}response={...response,ok:false,error:{message:'Could not open the system default browser.'}};}pending=null;const installed=response.ok&&response.kind==='renoInstalled',removed=response.ok&&response.kind==='renoRemoved',discord=response.ok&&response.kind==='renoDiscord',manual=response.ok&&response.kind==='renoReshadeManual',applied=Number(response.state?.settingsApplied||0),message=!response.ok?(response.error?.message||'RenoDX operation failed.'):removed?(response.state?.statusText||'RenoDX and ReShade were removed.'):installed?`RenoDX installed.${applied?` Applied ${applied} wiki setting${applied===1?'':'s'}.`:''}`:(response.state?.statusText||'Finish the opened step, then return to Hubcap.');if(installed||removed)setEnabledState(response.state?.appId||request.state.appId,installed);request.button.disabled=false;host.setModalState(request.modal,{phase:response.ok?'success':'error',title:!response.ok?'RenoDX Failed':removed?'RenoDX Removed':installed?'RenoDX Installed':discord?'RenoDX Discord Opened':manual?'Finish ReShade Setup':'RenoDX',message,onClose:()=>{command('state',{appId:request.state.appId});requestCatalog()}});if(installed||removed){command('state',{appId:request.state.appId});requestCatalog()}});
const tick=()=>{const app=host.libraryAppId();if(app===currentApp)return;if(currentApp)host.setGameSetting(ID,currentApp,null);currentApp=app;currentState=null;if(app)command('state',{appId:app})};tick();setInterval(tick,300);requestCatalog();setInterval(queueBadges,1500);setInterval(()=>requestCatalog(),60000);
})())HUBCAPPLUGIN";
}

extern "C" void hubcap_runtime_log(const char* message)
{
    if (g_host && g_host->log) g_host->log("hubcap.renodx", message);
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
    plugin->id = "hubcap.renodx";
    plugin->name = "RenoDX";
    plugin->version = "0.1.2";
    plugin->description = "Detects, installs, configures, and removes RenoDX modifications.";
    plugin->ui_script = k_ui;
    plugin->handle = handle;
    plugin->free_string = free_string;
    plugin->shutdown = nullptr;
    return true;
}
