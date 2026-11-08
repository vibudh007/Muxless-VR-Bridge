#pragma once
#include <fstream>
#include <filesystem>
#include <cstring>
#include <cstdlib>
#include <vulkan/vulkan.h>
#include "config_text.hpp"
// Per-user opt-in survives Steam's runtime launcher without global Steam env changes.
static std::string bridgeConfigDirectory(){
 const char*xdg=getenv("XDG_CONFIG_HOME");if(xdg&&*xdg)return std::string(xdg)+"/wmr-gpu-bridge";
 const char*home=getenv("HOME");return std::string(home?home:"/tmp")+"/.config/wmr-gpu-bridge";
}
static bool bridgeConfigBool(const char*key,bool fallback=false){
 // Runtime options are a startup snapshot; changing them requires a VR restart.
 static const std::string text=[](){std::ifstream file(bridgeConfigDirectory()+"/settings.conf");return std::string(std::istreambuf_iterator<char>(file),{});}();
 return bridgeSettings::value(text,key,fallback?"1":"0")=="1";
}
static bool bridgeExplicitlyEnabled(){
 const char*disabled=getenv("DISABLE_WMR_GPU_BRIDGE");if(disabled&&!strcmp(disabled,"1"))return false;
 const char*env=getenv("WMR_GPU_BRIDGE");return (env&&!strcmp(env,"1"))||bridgeConfigBool("enabled");
}
inline thread_local bool bridgeCreatingPrivateInstance = false;
struct BridgePrivateInstanceScope {
 const bool previous = bridgeCreatingPrivateInstance;
 BridgePrivateInstanceScope(){bridgeCreatingPrivateInstance = true;}
 ~BridgePrivateInstanceScope(){bridgeCreatingPrivateInstance = previous;}
};

static bool bridgeEligibleInstance(const VkInstanceCreateInfo& info){
 if(bridgeCreatingPrivateInstance)return false;
 for(uint32_t i=0;i<info.enabledExtensionCount;i++){
  if(!std::strcmp(info.ppEnabledExtensionNames[i],VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME))
   return bridgeExplicitlyEnabled();
 }
 return false;
}

static void bridgeConfigureProcess(){
 // The launcher or runtime chooses the render GPU before instance creation.
 if(!getenv("WMR_BRIDGE_MODE"))setenv("WMR_BRIDGE_MODE","alpha",1);
 if(!getenv("WMR_BRIDGE_LOG_DIR")){
  const char*xdg=getenv("XDG_STATE_HOME");const char*home=getenv("HOME");
  std::string path=xdg&&*xdg?xdg:std::string(home?home:"/tmp")+"/.local/state";path+="/wmr-gpu-bridge";
  std::error_code ec;std::filesystem::create_directories(path,ec);
  if(!ec)setenv("WMR_BRIDGE_LOG_DIR",path.c_str(),1);
 }
}
