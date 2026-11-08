// Read-only capability intersection probe. Does not acquire a lease or modeset.
#include <vulkan/vulkan.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <filesystem>
#include <cstdint>
int main(){
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;VkInstance instance;
 if(vkCreateInstance(&ci,nullptr,&instance)!=VK_SUCCESS)return 1;
 uint32_t count=0;if(vkEnumeratePhysicalDevices(instance,&count,nullptr)!=VK_SUCCESS)return 1;std::vector<VkPhysicalDevice>gpus(count);if(vkEnumeratePhysicalDevices(instance,&count,gpus.data())!=VK_SUCCESS)return 1;
 for(auto gpu:gpus){VkPhysicalDeviceProperties p;vkGetPhysicalDeviceProperties(gpu,&p);if(p.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
  VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};identity.pNext=&ids;
  vkGetPhysicalDeviceProperties2(gpu,&identity);
  printf("IDENTITY gpu=\"%s\" vendor=0x%x device=0x%x device_uuid=",p.deviceName,p.vendorID,p.deviceID);
  for(auto byte:ids.deviceUUID)printf("%02x",byte);
  printf(" driver_uuid=");for(auto byte:ids.driverUUID)printf("%02x",byte);printf("\n");
  uint32_t extensionCount=0;
  if(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extensionCount,nullptr)==VK_SUCCESS){
   std::vector<VkExtensionProperties>extensions(extensionCount);
   if(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extensionCount,extensions.data())==VK_SUCCESS){
    for(const char*name:{VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,"VK_EXT_calibrated_timestamps","VK_KHR_calibrated_timestamps"}){
     bool supported=false;for(uint32_t i=0;i<extensionCount;i++)supported|=!strcmp(extensions[i].extensionName,name);
     printf("CAPABILITY gpu=\"%s\" extension=%s supported=%u\n",p.deviceName,name,unsigned(supported));
    }
   }
  }
  for(auto format:{VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM}){
   VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};props.pNext=&list;vkGetPhysicalDeviceFormatProperties2(gpu,format,&props);
   std::vector<VkDrmFormatModifierPropertiesEXT>mods(list.drmFormatModifierCount);list.pDrmFormatModifierProperties=mods.data();vkGetPhysicalDeviceFormatProperties2(gpu,format,&props);
   for(auto&m:mods)printf("VULKAN gpu=\"%s\" type=%u format=%u modifier=0x%016llx planes=%u features=0x%x\n",p.deviceName,p.deviceType,format,(unsigned long long)m.drmFormatModifier,m.drmFormatModifierPlaneCount,m.drmFormatModifierTilingFeatures);
  }
 }
 vkDestroyInstance(instance,nullptr);
 for(auto&entry:std::filesystem::directory_iterator("/dev/dri")){
  auto name=entry.path().filename().string();if(name.compare(0,4,"card"))continue;
  int fd=open(entry.path().c_str(),O_RDONLY|O_CLOEXEC);if(fd<0){printf("KMS_UNAVAILABLE node=%s\n",entry.path().c_str());continue;}
  if(drmSetClientCap(fd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1)){close(fd);continue;}
  auto*planes=drmModeGetPlaneResources(fd);if(!planes){close(fd);continue;}
  for(uint32_t i=0;i<planes->count_planes;i++){
   auto*properties=drmModeObjectGetProperties(fd,planes->planes[i],DRM_MODE_OBJECT_PLANE);if(!properties)continue;uint32_t blobId=0;uint64_t type=UINT64_MAX;
   for(uint32_t j=0;j<properties->count_props;j++){auto*p=drmModeGetProperty(fd,properties->props[j]);if(!p)continue;if(!strcmp(p->name,"IN_FORMATS"))blobId=properties->prop_values[j];if(!strcmp(p->name,"type"))type=properties->prop_values[j];drmModeFreeProperty(p);}
   drmModeFreeObjectProperties(properties);auto*blob=blobId?drmModeGetPropertyBlob(fd,blobId):nullptr;if(!blob)continue;
   if(blob->length>=sizeof(drm_format_modifier_blob)){
    auto*header=(const drm_format_modifier_blob*)blob->data;
    bool valid=uint64_t(header->formats_offset)+uint64_t(header->count_formats)*sizeof(uint32_t)<=blob->length&&uint64_t(header->modifiers_offset)+uint64_t(header->count_modifiers)*sizeof(drm_format_modifier)<=blob->length;
    if(valid){auto*formats=(const uint32_t*)((const char*)blob->data+header->formats_offset);auto*mods=(const drm_format_modifier*)((const char*)blob->data+header->modifiers_offset);
     for(uint32_t j=0;j<header->count_modifiers;j++)for(unsigned bit=0;bit<64;bit++)if((mods[j].formats&(uint64_t(1)<<bit))&&uint64_t(mods[j].offset)+bit<header->count_formats){auto format=formats[mods[j].offset+bit];if(format==DRM_FORMAT_ARGB8888||format==DRM_FORMAT_ABGR8888)printf("KMS node=%s plane=%u type=%llu fourcc=0x%x modifier=0x%016llx\n",entry.path().c_str(),planes->planes[i],(unsigned long long)type,format,(unsigned long long)mods[j].modifier);}
    }
   }
   drmModeFreePropertyBlob(blob);
  }
  drmModeFreePlaneResources(planes);close(fd);
 }
}
