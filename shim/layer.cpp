#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <atomic>
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <limits>
#include <set>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

#include "runtime_config.hpp"

#include "run_logging.hpp"
#include "function_timing.hpp"
static std::mutex mapsMutex,logMutex;
static FILE*logFile=nullptr;
static void log(const char*fmt,...){ WMR_TIMING_SCOPE("log");
 std::lock_guard<std::mutex>lock(logMutex);
 if(!logFile){logFile=bridgeOpenRunLog();if(!logFile)logFile=stderr;setvbuf(logFile,nullptr,_IOLBF,0);std::fprintf(logFile,"%s pid=%d WMR_GPU_BRIDGE version=%s session=%s\n",bridgeUtc().c_str(),getpid(),WMR_BRIDGE_VERSION,bridgeRunDirectory.filename().c_str());bridgeHealth("starting");}
 if(logFile!=stderr&&ftell(logFile)>4*1024*1024){
  fclose(logFile);std::error_code ec;auto previous=bridgeLogPath;previous+=".previous";std::filesystem::remove(previous,ec);std::filesystem::rename(bridgeLogPath,previous,ec);logFile=fopen(bridgeLogPath.c_str(),"a");if(!logFile)logFile=stderr;setvbuf(logFile,nullptr,_IOLBF,0);
 }
 std::fprintf(logFile,"%s pid=%d ",bridgeUtc().c_str(),getpid());va_list args;va_start(args,fmt);vfprintf(logFile,fmt,args);va_end(args);fputc('\n',logFile);
}
static void*key(const void*h){return h?*reinterpret_cast<void*const*>(h):nullptr;}
struct Instance{VkInstance handle;PFN_vkGetInstanceProcAddr gipa;bool active;};
struct Device{
 PFN_vkCmdPipelineBarrier pipelineBarrier=nullptr;PFN_vkWaitForFences waitFences=nullptr;PFN_vkResetFences resetFences=nullptr;PFN_vkGetFenceStatus fenceStatus=nullptr;PFN_vkQueueSubmit queueSubmit=nullptr;PFN_vkQueueSubmit2 queueSubmit2=nullptr;
 bool modifierImages=false,externalTransfer=false;VkDevice handle;VkPhysicalDevice physical;PFN_vkGetDeviceProcAddr gdpa;std::shared_ptr<Instance>instance;std::mutex queueFamiliesMutex;std::unordered_map<VkQueue,uint32_t>queueFamilies;};
static std::unordered_map<void*,std::shared_ptr<Instance>>instances;
static std::unordered_map<void*,std::shared_ptr<Device>>devices;
static std::shared_ptr<Instance>istate(const void*h){ WMR_TIMING_SCOPE("istate");std::lock_guard<std::mutex>lock(mapsMutex);auto it=instances.find(key(h));return it==instances.end()?nullptr:it->second;}
static std::shared_ptr<Device>dstate(const void*h){ WMR_TIMING_SCOPE("dstate");std::lock_guard<std::mutex>lock(mapsMutex);auto it=devices.find(key(h));return it==devices.end()?nullptr:it->second;}
static void gpu(const std::shared_ptr<Instance>&s,VkPhysicalDevice p){ WMR_TIMING_SCOPE("gpu");auto f=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(s->gipa(s->handle,"vkGetPhysicalDeviceProperties2"));VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};id.pNext=&drm;VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};props.pNext=&id;f(p,&props);log("GPU handle=%p name=%s primary=%lld:%lld render=%lld:%lld uuid=%02x%02x%02x%02x%02x%02x%02x%02x",(void*)p,props.properties.deviceName,(long long)drm.primaryMajor,(long long)drm.primaryMinor,(long long)drm.renderMajor,(long long)drm.renderMinor,id.deviceUUID[0],id.deviceUUID[1],id.deviceUUID[2],id.deviceUUID[3],id.deviceUUID[4],id.deviceUUID[5],id.deviceUUID[6],id.deviceUUID[7]);}
static VKAPI_ATTR VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo*ci,const VkAllocationCallbacks*a,VkInstance*out){ WMR_TIMING_SCOPE("createInstance");auto*link=(VkLayerInstanceCreateInfo*)ci->pNext;while(link&&(link->sType!=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO||link->function!=VK_LAYER_LINK_INFO))link=(VkLayerInstanceCreateInfo*)link->pNext;if(!link)return VK_ERROR_INITIALIZATION_FAILED;auto gipa=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;link->u.pLayerInfo=link->u.pLayerInfo->pNext;auto f=(PFN_vkCreateInstance)gipa(nullptr,"vkCreateInstance");const bool active=bridgeEligibleInstance(*ci);if(active)bridgeConfigureProcess();VkResult r=f(ci,a,out);if(r==VK_SUCCESS){auto s=std::make_shared<Instance>();s->handle=*out;s->gipa=gipa;s->active=active;{std::lock_guard<std::mutex>lock(mapsMutex);instances[key(*out)]=s;}if(s->active){log("LAYER trace active pid=%d DRI_PRIME=%s instance=%p app=%s",getpid(),getenv("DRI_PRIME"), (void*)*out,ci->pApplicationInfo&&ci->pApplicationInfo->pApplicationName?ci->pApplicationInfo->pApplicationName:"");if(bridgeConfigBool("function_timing")){static std::once_flag timingOnce;std::call_once(timingOnce,[]{bridgeTiming::start(bridgeRunDirectory,bridgeConfigBool("function_timing_pacing_only"));});}for(uint32_t i=0;i<ci->enabledExtensionCount;i++)log("INSTANCE_EXT %s",ci->ppEnabledExtensionNames[i]);uint32_t n=0;auto en=(PFN_vkEnumeratePhysicalDevices)gipa(*out,"vkEnumeratePhysicalDevices");en(*out,&n,nullptr);std::vector<VkPhysicalDevice>ps(n);en(*out,&n,ps.data());for(auto p:ps)gpu(s,p);}}return r;}
static VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance i,const VkAllocationCallbacks*a){ WMR_TIMING_SCOPE("destroyInstance");auto s=istate(i);if(!s)return;if(s->active)bridgeHealth("stopped");auto f=(PFN_vkDestroyInstance)s->gipa(i,"vkDestroyInstance");{std::lock_guard<std::mutex>lock(mapsMutex);instances.erase(key(i));}f(i,a);}
static bool enabled();
static void bridgeDeviceCleanup(VkDevice);
static void descriptorDeviceCleanup(VkDevice);
static VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice p,const VkDeviceCreateInfo*ci,const VkAllocationCallbacks*a,VkDevice*out){ WMR_TIMING_SCOPE("createDevice");auto s=istate(p);if(!s)return VK_ERROR_INITIALIZATION_FAILED;auto*link=(VkLayerDeviceCreateInfo*)ci->pNext;while(link&&(link->sType!=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO||link->function!=VK_LAYER_LINK_INFO))link=(VkLayerDeviceCreateInfo*)link->pNext;if(!link)return VK_ERROR_INITIALIZATION_FAILED;auto gipa=link->u.pLayerInfo->pfnNextGetInstanceProcAddr;auto gdpa=link->u.pLayerInfo->pfnNextGetDeviceProcAddr;link->u.pLayerInfo=link->u.pLayerInfo->pNext;auto f=(PFN_vkCreateDevice)gipa(s->handle,"vkCreateDevice");if(s->active){log("CREATE_DEVICE physical=%p",(void*)p);for(uint32_t qi=0;qi<ci->queueCreateInfoCount;qi++)log("CREATE_QUEUE family=%u count=%u flags=0x%x",ci->pQueueCreateInfos[qi].queueFamilyIndex,ci->pQueueCreateInfos[qi].queueCount,ci->pQueueCreateInfos[qi].flags);gpu(s,p);for(uint32_t i=0;i<ci->enabledExtensionCount;i++)log("DEVICE_EXT %s",ci->ppEnabledExtensionNames[i]);}VkDeviceCreateInfo local=*ci;
std::vector<const char*> extensions;
bool modifierEnabled=false,externalTransfer=false;
if(s->active&&enabled()){
 if(ci->enabledExtensionCount)extensions.assign(ci->ppEnabledExtensionNames,ci->ppEnabledExtensionNames+ci->enabledExtensionCount);
 auto enumerate=(PFN_vkEnumerateDeviceExtensionProperties)s->gipa(s->handle,"vkEnumerateDeviceExtensionProperties");
 uint32_t count=0;
 if(enumerate&&enumerate(p,nullptr,&count,nullptr)==VK_SUCCESS){
  std::vector<VkExtensionProperties> available(count);
  if(enumerate(p,nullptr,&count,available.data())==VK_SUCCESS){
   const auto supported=[&](const char*name){
    return std::any_of(available.begin(),available.begin()+count,[&](const auto&ext){return !strcmp(ext.extensionName,name);});
   };
   const auto enableExtension=[&](const char*name){
    if(supported(name)&&std::none_of(extensions.begin(),extensions.end(),[&](const char*ext){return !strcmp(ext,name);}))extensions.push_back(name);
   };
   for(auto name:{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME})enableExtension(name);
   modifierEnabled=bridgeConfigBool("direct_scanout")&&supported(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)&&supported(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
   if(modifierEnabled){enableExtension(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);enableExtension(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);}
  }
 }
 const auto has=[&](const char*name){return std::any_of(extensions.begin(),extensions.end(),[&](const char*ext){return !strcmp(ext,name);});};
 externalTransfer=has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)&&has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)&&has(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
 local.enabledExtensionCount=extensions.size();
 local.ppEnabledExtensionNames=extensions.data();
}
VkResult r=f(p,&local,a,out);if(r==VK_SUCCESS){auto d=std::make_shared<Device>();d->modifierImages=modifierEnabled;d->externalTransfer=externalTransfer;d->handle=*out;d->physical=p;d->gdpa=gdpa;d->instance=s;
 d->pipelineBarrier=(PFN_vkCmdPipelineBarrier)gdpa(*out,"vkCmdPipelineBarrier");d->waitFences=(PFN_vkWaitForFences)gdpa(*out,"vkWaitForFences");d->resetFences=(PFN_vkResetFences)gdpa(*out,"vkResetFences");d->fenceStatus=(PFN_vkGetFenceStatus)gdpa(*out,"vkGetFenceStatus");d->queueSubmit=(PFN_vkQueueSubmit)gdpa(*out,"vkQueueSubmit");d->queueSubmit2=(PFN_vkQueueSubmit2)gdpa(*out,"vkQueueSubmit2");std::lock_guard<std::mutex>lock(mapsMutex);devices[key(*out)]=d;}if(s->active)log("CREATE_DEVICE result=%d",r);return r;}
static VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice d,const VkAllocationCallbacks*a){ WMR_TIMING_SCOPE("destroyDevice");auto s=dstate(d);if(!s)return;descriptorDeviceCleanup(d);bridgeDeviceCleanup(d);auto f=(PFN_vkDestroyDevice)s->gdpa(d,"vkDestroyDevice");{std::lock_guard<std::mutex>lock(mapsMutex);devices.erase(key(d));}f(d,a);}
static void check(VkResult r,const char*where){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(where)+" result="+std::to_string(r));}
#define VK(call) check((call),#call)
#include "../common/vulkan_helpers.hpp"
#include "bridge.hpp"
#include "trace-generated.h"
extern "C" __attribute__((visibility("default"))) PFN_vkVoidFunction VKAPI_CALL WMR_GetInstanceProcAddr(VkInstance i,const char*n){
 if(!strcmp(n,"vkCreateInstance"))return (PFN_vkVoidFunction)createInstance;
 if(!strcmp(n,"vkDestroyInstance"))return (PFN_vkVoidFunction)destroyInstance;
 if(!strcmp(n,"vkCreateDevice"))return (PFN_vkVoidFunction)createDevice;
 if(!strcmp(n,"vkDestroyDevice"))return (PFN_vkVoidFunction)destroyDevice;
 if(!strcmp(n,"vkGetInstanceProcAddr"))return (PFN_vkVoidFunction)WMR_GetInstanceProcAddr;
 auto s=istate(i);if(s&&s->active){auto bridge=bridgeHook(n);if(bridge)return bridge;auto hook=traceHook(n);if(hook)return hook;}
 return s?s->gipa(i,n):nullptr;
}
extern "C" __attribute__((visibility("default"))) PFN_vkVoidFunction VKAPI_CALL WMR_GetDeviceProcAddr(VkDevice d,const char*n){
 if(!strcmp(n,"vkDestroyDevice"))return (PFN_vkVoidFunction)destroyDevice;
 if(!strcmp(n,"vkGetDeviceProcAddr"))return (PFN_vkVoidFunction)WMR_GetDeviceProcAddr;
 auto s=dstate(d);if(s&&s->instance->active){auto bridge=bridgeHook(n);if(bridge)return bridge;auto hook=traceHook(n);if(hook)return hook;}
 return s?s->gdpa(d,n):nullptr;
}
extern "C" __attribute__((visibility("default"))) VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface*v){if(v->loaderLayerInterfaceVersion>2)v->loaderLayerInterfaceVersion=2;v->pfnGetInstanceProcAddr=WMR_GetInstanceProcAddr;v->pfnGetDeviceProcAddr=WMR_GetDeviceProcAddr;v->pfnGetPhysicalDeviceProcAddr=nullptr;return VK_SUCCESS;}
