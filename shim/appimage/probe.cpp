#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include "../third_party/nlohmann/json.hpp"
using json=nlohmann::json;
static void global(void*data,wl_registry*,uint32_t,const char*interface,uint32_t){if(!std::strcmp(interface,"wp_drm_lease_device_v1"))(*(unsigned*)data)++;}
static void removed(void*,wl_registry*,uint32_t){}
int main(int argc,char**argv){json report;
 if(argc>1&&!std::strcmp(argv[1],"--wayland")){
  wl_display*display=wl_display_connect(nullptr);if(!display){std::puts("{\"connected\":false,\"drm_lease_globals\":0}");return 1;}
  unsigned leases=0;auto*registry=wl_display_get_registry(display);wl_registry_listener listener{global,removed};wl_registry_add_listener(registry,&listener,&leases);int result=wl_display_roundtrip(display);wl_registry_destroy(registry);wl_display_disconnect(display);
  report={{"connected",result>=0},{"drm_lease_globals",leases}};std::puts(report.dump().c_str());return 0;
 }
 uint32_t instanceCount=0;vkEnumerateInstanceExtensionProperties(nullptr,&instanceCount,nullptr);std::vector<VkExtensionProperties>instanceExtensions(instanceCount);vkEnumerateInstanceExtensionProperties(nullptr,&instanceCount,instanceExtensions.data());std::set<std::string>instanceNames;for(auto&e:instanceExtensions)instanceNames.insert(e.extensionName);report["missing_instance_extensions"]=json::array();
 for(const char*required:{"VK_KHR_surface","VK_KHR_display","VK_EXT_direct_mode_display","VK_EXT_acquire_drm_display","VK_EXT_display_surface_counter"})if(!instanceNames.count(required))report["missing_instance_extensions"].push_back(required);
 if(!report["missing_instance_extensions"].empty()){report["error"]="required direct-display instance extensions missing";std::puts(report.dump().c_str());return 1;}
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="Muxless VR Bridge capability probe";app.apiVersion=VK_API_VERSION_1_1;VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;VkInstance instance;
 VkResult result=vkCreateInstance(&ci,nullptr,&instance);if(result!=VK_SUCCESS){report={{"error","vkCreateInstance"},{"result",result}};std::puts(report.dump().c_str());return 1;}
 uint32_t count=0;vkEnumeratePhysicalDevices(instance,&count,nullptr);std::vector<VkPhysicalDevice>devices(count);vkEnumeratePhysicalDevices(instance,&count,devices.data());report["devices"]=json::array();
 for(auto device:devices){VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};props.pNext=&drm;vkGetPhysicalDeviceProperties2(device,&props);uint32_t n=0;vkEnumerateDeviceExtensionProperties(device,nullptr,&n,nullptr);std::vector<VkExtensionProperties>extensions(n);vkEnumerateDeviceExtensionProperties(device,nullptr,&n,extensions.data());std::set<std::string>names;for(auto&e:extensions)names.insert(e.extensionName);
  json missing=json::array();for(const char*required:{"VK_KHR_swapchain","VK_KHR_external_memory_fd","VK_EXT_external_memory_dma_buf","VK_KHR_external_semaphore_fd","VK_EXT_display_control"})if(!names.count(required))missing.push_back(required);
  VkExternalBufferProperties buffer{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};VkPhysicalDeviceExternalBufferInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;info.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  if(names.count("VK_EXT_external_memory_dma_buf"))vkGetPhysicalDeviceExternalBufferProperties(device,&info,&buffer);
  VkExternalSemaphoreProperties semaphore{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};VkPhysicalDeviceExternalSemaphoreInfo sem{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};sem.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;vkGetPhysicalDeviceExternalSemaphoreProperties(device,&sem,&semaphore);
  report["devices"].push_back({{"name",props.properties.deviceName},{"vendor",props.properties.vendorID},{"device",props.properties.deviceID},{"type",props.properties.deviceType},{"api",props.properties.apiVersion},{"primary_major",drm.primaryMajor},{"primary_minor",drm.primaryMinor},{"render_major",drm.renderMajor},{"render_minor",drm.renderMinor},{"missing_extensions",missing},{"dma_buf_features",buffer.externalMemoryProperties.externalMemoryFeatures},{"sync_fd_features",semaphore.externalSemaphoreFeatures}});
 }
 vkDestroyInstance(instance,nullptr);std::puts(report.dump(2).c_str());return 0;
}
