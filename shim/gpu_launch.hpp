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
#include "third_party/nlohmann/json.hpp"

namespace gpuLaunch {
using Environment=std::map<std::string,std::string>;
struct Plan {std::string name,selector,ids,driver;bool nvidia=false;};
inline std::string pciSelector(const nlohmann::json&gpu){
 if(!gpu.value("has_pci",false))return {};
 auto domain=gpu.value("pci_domain",0u),bus=gpu.value("pci_bus",0u),device=gpu.value("pci_device",0u),function=gpu.value("pci_function",0u);
 if(domain>65535||bus>255||device>31||function>7)throw std::runtime_error("Invalid GPU PCI identity");
 char text[40];snprintf(text,sizeof(text),"pci-%04x_%02x_%02x_%x",domain,bus,device,function);return text;
}
inline Plan plan(const nlohmann::json&inventory){
 if(!inventory.contains("devices")||!inventory["devices"].is_array())throw std::runtime_error("Invalid GPU inventory");
 std::vector<Plan>discrete,other;
 for(const auto&gpu:inventory["devices"]){
  auto type=gpu.value("type",4);if(type==4)continue; // Never select software rendering.
  char ids[32];snprintf(ids,sizeof(ids),"%04x:%04x",gpu.value("vendor",0u),gpu.value("device",0u));
  Plan p{gpu.value("name",std::string("GPU")),pciSelector(gpu),ids,gpu.value("driver_name",std::string()),gpu.value("driver_id",0)==4}; // VK_DRIVER_ID_NVIDIA_PROPRIETARY
  if(p.selector.empty())p.selector=p.ids;
  (type==2?discrete:other).push_back(p);
 }
 auto&candidates=discrete.empty()?other:discrete;
 if(candidates.empty())throw std::runtime_error("No hardware Vulkan GPU available for rendering");
 // Stable selection for multi-dGPU systems; prefer a caller's explicit discrete
 // PCI/device selector when supplied. No model/vendor preference is imposed.
 std::sort(candidates.begin(),candidates.end(),[](const Plan&a,const Plan&b){return a.selector<b.selector;});
 if(const char*requested=getenv("DRI_PRIME"))for(const auto&p:candidates)if(p.selector==requested||p.ids==requested)return p;
 return candidates.front();
}
inline Plan detect(const std::filesystem::path&probe){
 auto result=boundedCommand({probe.string(),"--gpus"});
 if(result.first)throw std::runtime_error("Cannot discover rendering GPU: "+result.second.substr(0,500));
 try{return plan(nlohmann::json::parse(result.second));}
 catch(const nlohmann::json::exception&){throw std::runtime_error("GPU discovery returned invalid data: "+result.second.substr(0,500));}
}
inline Environment settings(const Plan&p){
 Environment e{{"DRI_PRIME",p.selector}};
 if(p.nvidia){e["__NV_PRIME_RENDER_OFFLOAD"]="1";e["__VK_LAYER_NV_optimus"]="NVIDIA_only";e["__GLX_VENDOR_LIBRARY_NAME"]="nvidia";}
 return e;
}
inline void apply(const Plan&p){
 for(const char*name:{"MESA_VK_DEVICE_SELECT","MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE","__NV_PRIME_RENDER_OFFLOAD","__VK_LAYER_NV_optimus","__GLX_VENDOR_LIBRARY_NAME","__NV_PRIME_RENDER_OFFLOAD_PROVIDER"})unsetenv(name);
 for(const auto&e:settings(p))if(setenv(e.first.c_str(),e.second.c_str(),1))throw std::runtime_error("Cannot set GPU launch environment");
}
inline bool compatible(const Environment&e,const Plan&p){
 auto get=[&](const char*name){auto i=e.find(name);return i==e.end()?std::string():i->second;};
 const bool target=get("DRI_PRIME")==p.selector||get("DRI_PRIME")==p.ids;
 if(!target||!get("MESA_VK_DEVICE_SELECT").empty()||!get("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE").empty()||!get("__NV_PRIME_RENDER_OFFLOAD_PROVIDER").empty())return false;
 if(p.nvidia)return get("__NV_PRIME_RENDER_OFFLOAD")=="1"&&get("__VK_LAYER_NV_optimus")=="NVIDIA_only"&&get("__GLX_VENDOR_LIBRARY_NAME")=="nvidia";
 // Numeric DRI_PRIME=1 does not identify a physical GPU and is not sufficient.
 return get("__GLX_VENDOR_LIBRARY_NAME")!="nvidia"&&get("__NV_PRIME_RENDER_OFFLOAD")!="1"&&get("__VK_LAYER_NV_optimus").empty();
}
struct SteamProcess {std::string pid;bool dgpu;};
inline std::vector<SteamProcess> steamProcesses(const Plan&p,const std::filesystem::path&proc="/proc"){
 std::vector<SteamProcess>result;std::error_code error;
 for(const auto&entry:std::filesystem::directory_iterator(proc,error)){
  auto pid=entry.path().filename().string();if(pid.empty()||pid.find_first_not_of("0123456789")!=std::string::npos)continue;
  struct stat owner{};if(stat(entry.path().c_str(),&owner)||owner.st_uid!=getuid())continue;
  std::ifstream nameFile(entry.path()/"comm");std::string name;std::getline(nameFile,name);if(name!="steam")continue;
  std::ifstream statFile(entry.path()/"stat");std::string status;std::getline(statFile,status);auto close=status.rfind(')');if(close!=std::string::npos&&status.size()>close+2&&status[close+2]=='Z')continue;
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
