#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include "subprocess.hpp"
#include "gpu_inventory.hpp"
#include "third_party/nlohmann/json.hpp"

namespace gpuLaunch {
using Environment=std::map<std::string,std::string>;
struct Plan {std::string name,selector,ids,driver;bool nvidia=false;bool systemDrivers=false;std::string discovery;};
inline std::string pciSelector(const nlohmann::json&gpu){
 if(!gpu.value("has_pci",false))return {};
 auto domain=gpu.value("pci_domain",0u),bus=gpu.value("pci_bus",0u),device=gpu.value("pci_device",0u),function=gpu.value("pci_function",0u);
 if(domain>65535||bus>255||device>31||function>7)throw std::runtime_error("Invalid GPU PCI identity");
 char text[40];snprintf(text,sizeof(text),"pci-%04x_%02x_%02x_%x",domain,bus,device,function);return text;
}
inline Plan plan(const nlohmann::json&inventory){
 if(!inventory.contains("devices")||!inventory["devices"].is_array())throw std::runtime_error("Invalid GPU inventory");
 std::vector<Plan>discrete;
 for(const auto&gpu:inventory["devices"]){
  if(gpu.value("type",-1)!=2)continue; // Only VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU.
  char ids[32];snprintf(ids,sizeof(ids),"%04x:%04x",gpu.value("vendor",0u),gpu.value("device",0u));
  Plan p{gpu.value("name",std::string("GPU")),pciSelector(gpu),ids,gpu.value("driver_name",std::string()),gpu.value("driver_id",0)==4}; // VK_DRIVER_ID_NVIDIA_PROPRIETARY
  if(p.selector.empty())p.selector=p.ids;
  discrete.push_back(p);
 }
 auto&candidates=discrete;
 if(candidates.empty())throw std::runtime_error("No discrete Vulkan GPU is visible. Refusing to render on an integrated or software GPU");
 // Stable selection for multi-dGPU systems; prefer a caller's explicit discrete
 // PCI/device selector when supplied. No model/vendor preference is imposed.
 std::sort(candidates.begin(),candidates.end(),[](const Plan&a,const Plan&b){return a.selector<b.selector;});
 if(const char*requested=getenv("DRI_PRIME")){
  std::string selector=requested;if(!selector.empty()&&selector.back()=='!')selector.pop_back();
  for(const auto&p:candidates)if(p.selector==selector||p.ids==selector)return p;
  // DRI_PRIME is inherited by ordinary desktop sessions too. An integrated
  // selection is not a request for a particular discrete GPU: replace it with
  // our discrete selection instead of misreporting the visible iGPU as missing.
  for(const auto&gpu:inventory["devices"]){
   if(gpu.value("type",-1)!=1)continue; // VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU.
   char ids[32];snprintf(ids,sizeof(ids),"%04x:%04x",gpu.value("vendor",0u),gpu.value("device",0u));
   if(selector==pciSelector(gpu)||selector==ids){
    std::fprintf(stderr,"Muxless VR Bridge: inherited DRI_PRIME selects an integrated GPU; using discrete GPU %s (%s) instead\n",candidates.front().name.c_str(),candidates.front().selector.c_str());
    return candidates.front();
   }
  }
  if(selector.rfind("pci-",0)==0||selector.find(':')!=std::string::npos)
   throw std::runtime_error("Requested discrete GPU "+selector+" is not visible; refusing to select a different GPU");
 }
 return candidates.front();
}
inline Plan detect(const std::filesystem::path&probe){
 std::string failures;
 for(bool systemDrivers:{false,true}){
  if(systemDrivers&&!gpuInventory::restricted())break;
  auto result=boundedCommand({probe.string(),systemDrivers?"--gpus-system":"--gpus"},std::chrono::seconds(15),false);
  try{
   if(result.first)throw std::runtime_error("GPU probe failed");
   auto inventory=nlohmann::json::parse(result.second);auto selected=plan(inventory);
   selected.systemDrivers=systemDrivers;selected.discovery=inventory.dump();
   if(systemDrivers)selected.discovery="Inherited driver restriction hid the discrete GPU; using system discovery. Initial attempt: "+failures+" System inventory: "+selected.discovery;
   return selected;
  }catch(const std::exception&e){
   failures+=(systemDrivers?"System discovery: ":"Inherited driver discovery: ")+std::string(e.what())+"; probe exit="+std::to_string(result.first)+"; "+result.second.substr(0,12000)+"\n";
  }
 }
 throw std::runtime_error("Cannot select a discrete rendering GPU; no integrated fallback was used. "+failures);
}
inline Environment settings(const Plan&p){
 Environment e{{"DRI_PRIME",p.selector}};
 if(p.nvidia){e["__NV_PRIME_RENDER_OFFLOAD"]="1";e["__VK_LAYER_NV_optimus"]="NVIDIA_only";e["__GLX_VENDOR_LIBRARY_NAME"]="nvidia";}
 return e;
}
inline void apply(const Plan&p){
 if(p.systemDrivers)gpuInventory::clearDrivers();
 for(auto name:gpuInventory::preferences)unsetenv(name);
 for(const auto&e:settings(p))if(setenv(e.first.c_str(),e.second.c_str(),1))throw std::runtime_error("Cannot set GPU launch environment");
}
inline bool compatible(const Environment&e,const Plan&p){
 auto get=[&](const char*name){auto i=e.find(name);return i==e.end()?std::string():i->second;};
 if(p.systemDrivers)for(auto name:gpuInventory::driverOverrides)if(!get(name).empty())return false;
 const bool target=get("DRI_PRIME")==p.selector||get("DRI_PRIME")==p.ids;
 if(!target||!get("VK_LOADER_DEVICE_SELECT").empty()||!get("NODEVICE_SELECT").empty()||!get("MESA_VK_DEVICE_SELECT").empty()||!get("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE").empty()||!get("__NV_PRIME_RENDER_OFFLOAD_PROVIDER").empty())return false;
 if(p.nvidia)return get("__NV_PRIME_RENDER_OFFLOAD")=="1"&&get("__VK_LAYER_NV_optimus")=="NVIDIA_only"&&get("__GLX_VENDOR_LIBRARY_NAME")=="nvidia";
 // Numeric DRI_PRIME=1 does not identify a physical GPU and is not sufficient.
 return get("__GLX_VENDOR_LIBRARY_NAME")!="nvidia"&&get("__NV_PRIME_RENDER_OFFLOAD")!="1"&&get("__VK_LAYER_NV_optimus").empty();
}
// SteamVR changes its main-thread comm. argv[0] still identifies the executable.
inline bool processNamed(const std::filesystem::path& dir,const std::string& wanted){
 std::ifstream nameFile(dir/"comm");std::string name;std::getline(nameFile,name);
 if(name==wanted)return true;
 std::ifstream command(dir/"cmdline",std::ios::binary);std::string executable;std::getline(command,executable,'\0');
 return !executable.empty()&&std::filesystem::path(executable).filename()==wanted;
}
inline bool zombieProcess(const std::filesystem::path& dir){
 std::ifstream statFile(dir/"stat");std::string status;std::getline(statFile,status);auto close=status.rfind(')');
 return close!=std::string::npos&&status.size()>close+2&&status[close+2]=='Z';
}
struct SteamProcess {std::string pid;bool dgpu;};
inline std::vector<SteamProcess> steamProcesses(const Plan&p,const std::filesystem::path&proc="/proc"){
 std::vector<SteamProcess>result;std::error_code error;
 for(const auto&entry:std::filesystem::directory_iterator(proc,error)){
  auto pid=entry.path().filename().string();if(pid.empty()||pid.find_first_not_of("0123456789")!=std::string::npos)continue;
  struct stat owner{};if(stat(entry.path().c_str(),&owner)||owner.st_uid!=getuid())continue;
  if(!processNamed(entry.path(),"steam")||zombieProcess(entry.path()))continue;
  std::ifstream environment(entry.path()/"environ",std::ios::binary);std::string variable;Environment values;
  while(std::getline(environment,variable,'\0')){auto equal=variable.find('=');if(equal!=std::string::npos)values[variable.substr(0,equal)]=variable.substr(equal+1);}
  result.push_back({pid,compatible(values,p)});
 }
 return result;
}
inline std::string incompatible(const std::vector<SteamProcess>&processes){
 std::string ids;for(const auto&process:processes)if(!process.dgpu){if(!ids.empty())ids+=",";ids+=process.pid;}return ids;
}
inline void requireDgpuSteam(const Plan&p){
 auto ids=incompatible(steamProcesses(p));
 if(!ids.empty())throw std::runtime_error("Steam is already running with a different or unverified GPU selection (PID "+ids+"). Fully exit Steam, then use wmr-gpu-bridge --steam. Its GPU cannot be changed after startup.");
}
}
