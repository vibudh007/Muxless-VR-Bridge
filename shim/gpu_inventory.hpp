#pragma once
#include <cstdlib>
#include <string>
#include "third_party/nlohmann/json.hpp"

// Driver manifests may be container remappings or intentional custom drivers.
// Preserve them first; retry system discovery only if they hide the dGPU.
namespace gpuInventory {
inline constexpr const char* driverOverrides[]={"VK_DRIVER_FILES","VK_ICD_FILENAMES","VK_ADD_DRIVER_FILES","VK_LOADER_DRIVERS_SELECT","VK_LOADER_DRIVERS_DISABLE"};
inline constexpr const char* preferences[]={"DRI_PRIME","VK_LOADER_DEVICE_SELECT","NODEVICE_SELECT","MESA_VK_DEVICE_SELECT","MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE","__NV_PRIME_RENDER_OFFLOAD","__VK_LAYER_NV_optimus","__GLX_VENDOR_LIBRARY_NAME","__NV_PRIME_RENDER_OFFLOAD_PROVIDER"};
inline nlohmann::json environment(){
 nlohmann::json result=nlohmann::json::object();
 for(auto name:driverOverrides)if(const char*value=getenv(name))result[name]=value;
 for(auto name:preferences)if(const char*value=getenv(name))result[name]=value;
 for(auto name:{"LD_LIBRARY_PATH","VK_LAYER_PATH","VK_IMPLICIT_LAYER_PATH","VK_INSTANCE_LAYERS","VK_LOADER_LAYERS_ENABLE","VK_LOADER_LAYERS_DISABLE","VK_LOADER_LAYERS_ALLOW"})if(const char*value=getenv(name))result[name]=value;
 return result;
}
inline bool restricted(){for(auto name:driverOverrides)if(const char*value=getenv(name))if(*value)return true;return false;}
inline void clearDrivers(){for(auto name:driverOverrides)unsetenv(name);}
inline void prepare(bool systemDrivers){
 for(auto name:preferences)unsetenv(name);
 // Inventory must see physical devices, not a Steam/overlay/device-selection
 // layer's filtered view. These changes are confined to the probe process.
 unsetenv("VK_INSTANCE_LAYERS");unsetenv("VK_LOADER_LAYERS_ENABLE");unsetenv("VK_LOADER_LAYERS_ALLOW");
 setenv("VK_LOADER_LAYERS_DISABLE","~implicit~",1);
 setenv("NODEVICE_SELECT","1",1); // Also covers older Vulkan loaders.
 setenv("DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1","1",1);
 setenv("DISABLE_MUXLESS_VR_BRIDGE","1",1);
 setenv("DISABLE_WMR_GPU_BRIDGE","1",1);
 setenv("DISABLE_MUXLESS_STEAMVR_PATCHES","1",1);
 setenv("DISABLE_STEAMVR_RUNTIME_PATCHES","1",1);
 // Request offload-capable enumeration without selecting or hiding a vendor.
 // Other drivers ignore this; the optimus selection layer stays disabled above.
 setenv("__NV_PRIME_RENDER_OFFLOAD","1",1);
 if(systemDrivers)clearDrivers();
}
}
