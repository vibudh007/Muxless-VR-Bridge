#include <vulkan/vulkan.h>
#include <cstdio>
#include <vector>
int main(){
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;
 VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;VkInstance instance;
 if(vkCreateInstance(&ci,nullptr,&instance)!=VK_SUCCESS)return 1;
 uint32_t n=0;vkEnumeratePhysicalDevices(instance,&n,nullptr);std::vector<VkPhysicalDevice>devices(n);vkEnumeratePhysicalDevices(instance,&n,devices.data());
 for(auto gpu:devices){
  VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(gpu,&p);if(p.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
  std::printf("GPU %s\n",p.deviceName);
  for(auto format:{VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_B8G8R8A8_SRGB}){
   VkFormatProperties fp;vkGetPhysicalDeviceFormatProperties(gpu,format,&fp);
   for(auto tiling:{VK_IMAGE_TILING_LINEAR,VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT})for(auto usage:{VkImageUsageFlags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT),VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_SRC_BIT)}){
    VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};external.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};info.pNext=&external;info.format=format;info.type=VK_IMAGE_TYPE_2D;info.tiling=tiling;VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};modifier.drmFormatModifier=0;modifier.sharingMode=VK_SHARING_MODE_EXCLUSIVE;if(tiling==VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT)external.pNext=&modifier;info.usage=usage;
    VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};props.pNext=&ep;
    auto result=vkGetPhysicalDeviceImageFormatProperties2(gpu,&info,&props);
    printf("tiling=%d format=%d linear_features=0x%x usage=0x%x result=%d external=0x%x max=%ux%u\n",tiling,format,fp.linearTilingFeatures,usage,result,ep.externalMemoryProperties.externalMemoryFeatures,props.imageFormatProperties.maxExtent.width,props.imageFormatProperties.maxExtent.height);
   }
  }
 }
 vkDestroyInstance(instance,nullptr);
}
