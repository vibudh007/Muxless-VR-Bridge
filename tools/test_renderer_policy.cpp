#include "../shim/layer.cpp"
#include <cassert>
static VkPhysicalDeviceType kind=VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
static unsigned creates=0;static int deviceToken;
static bool advertiseMemory=false;
static uint32_t receivedExtensionCount=0;
static VkResult VKAPI_CALL extensions(VkPhysicalDevice,const char*,uint32_t*count,VkExtensionProperties*out){
 if(!out){*count=advertiseMemory?1:0;return VK_SUCCESS;}
 if(advertiseMemory&&*count){strcpy(out[0].extensionName,VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);*count=1;}
 else *count=0;
 return VK_SUCCESS;
}
static void*deviceDispatch=&deviceToken;
static void VKAPI_CALL properties(VkPhysicalDevice,VkPhysicalDeviceProperties*p){*p={};p->deviceType=kind;strcpy(p->deviceName,"test GPU");}
static void VKAPI_CALL properties2(VkPhysicalDevice p,VkPhysicalDeviceProperties2*out){properties(p,&out->properties);}
static VkResult VKAPI_CALL create(VkPhysicalDevice,const VkDeviceCreateInfo*ci,const VkAllocationCallbacks*,VkDevice*out){creates++;receivedExtensionCount=ci->enabledExtensionCount;*out=(VkDevice)&deviceDispatch;return VK_SUCCESS;}
static PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice,const char*){return nullptr;}
static PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance,const char*n){
 if(!strcmp(n,"vkGetPhysicalDeviceProperties"))return (PFN_vkVoidFunction)properties;
 if(!strcmp(n,"vkGetPhysicalDeviceProperties2"))return (PFN_vkVoidFunction)properties2;
 if(!strcmp(n,"vkEnumerateDeviceExtensionProperties"))return (PFN_vkVoidFunction)extensions;
 if(!strcmp(n,"vkCreateDevice"))return (PFN_vkVoidFunction)create;
 return nullptr;
}
int main(){
 char tmp[]="/tmp/wmr-policy-XXXXXX";assert(mkdtemp(tmp));setenv("XDG_CONFIG_HOME",tmp,1);setenv("WMR_BRIDGE_LOG_DIR",tmp,1);
 setenv("WMR_GPU_BRIDGE","1",1);
 VkInstanceCreateInfo ordinary{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
 assert(!bridgeEligibleInstance(ordinary));
 const char* extension=VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME;
 ordinary.enabledExtensionCount=1;ordinary.ppEnabledExtensionNames=&extension;
 assert(bridgeEligibleInstance(ordinary));
 {BridgePrivateInstanceScope guard;assert(!bridgeEligibleInstance(ordinary));}
 assert(bridgeEligibleInstance(ordinary));
 setenv("WMR_BRIDGE_MODE","alpha",1);int token;void*dispatch=&token;auto physical=(VkPhysicalDevice)&dispatch;
 auto instance=std::make_shared<Instance>();instance->handle=(VkInstance)&dispatch;instance->gipa=gipa;instance->active=true;instances[&token]=instance;
 VkLayerDeviceLink link{};link.pfnNextGetInstanceProcAddr=gipa;link.pfnNextGetDeviceProcAddr=gdpa;
 VkLayerDeviceCreateInfo chain{};chain.sType=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;chain.function=VK_LAYER_LINK_INFO;
 VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};ci.pNext=&chain;VkDevice out;
 chain.u.pLayerInfo=&link;assert(createDevice(physical,&ci,nullptr,&out)==VK_SUCCESS&&creates==1&&receivedExtensionCount==0);
 advertiseMemory=true;kind=VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;chain.u.pLayerInfo=&link;assert(createDevice(physical,&ci,nullptr,&out)==VK_SUCCESS&&creates==2&&receivedExtensionCount==1);
 instance->active=false;kind=VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;chain.u.pLayerInfo=&link;assert(createDevice(physical,&ci,nullptr,&out)==VK_SUCCESS&&creates==3&&receivedExtensionCount==0);
 instances.clear();devices.clear();std::filesystem::remove_all(tmp);puts("PASS: DRM capability opts in; private and ordinary instances skip activation; integrated and discrete devices pass through with supported additions only.");
}
