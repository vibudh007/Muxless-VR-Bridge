#pragma once
// Experimental single-plane DRM_FORMAT_MOD_LINEAR DMA-BUF image pair.
// Physical queries are supplied by the caller so layer-level handles never go
// back through the loader's physical-device trampoline.
struct SharedLinearImage {
 GPU *source=nullptr,*output=nullptr;
 VkImage image=VK_NULL_HANDLE,alias=VK_NULL_HANDLE;
 VkDeviceMemory memory=VK_NULL_HANDLE,imported=VK_NULL_HANDLE;
 VkDeviceSize allocation=0;VkSubresourceLayout layout{};
 SharedLinearImage()=default;
 SharedLinearImage(const SharedLinearImage&)=delete;
 SharedLinearImage&operator=(const SharedLinearImage&)=delete;
 void make(GPU&s,GPU&o,VkFormat format,VkExtent2D extent,VkImageUsageFlags usage,
           PFN_vkGetPhysicalDeviceImageFormatProperties2 sourceQuery=vkGetPhysicalDeviceImageFormatProperties2,bool systemMemory=true,PFN_vkGetPhysicalDeviceImageFormatProperties2 outputQuery=vkGetPhysicalDeviceImageFormatProperties2){
  source=&s;output=&o;
  for(auto*g:{&s,&o}){
   auto query=g==&s?sourceQuery:outputQuery;
   VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};mod.drmFormatModifier=0;mod.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
   VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};external.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;external.pNext=&mod;
   VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};info.pNext=&external;info.format=format;info.type=VK_IMAGE_TYPE_2D;info.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;info.usage=usage;
   VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};VkImageFormatProperties2 caps{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};caps.pNext=&ep;
   VK(query(g->physical,&info,&caps));
   auto required=g==&s?VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT:VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
   if(!(ep.externalMemoryProperties.externalMemoryFeatures&required)||extent.width>caps.imageFormatProperties.maxExtent.width||extent.height>caps.imageFormatProperties.maxExtent.height)throw std::runtime_error("Shared linear image capability unavailable");
  }
  uint64_t modifier=0;VkImageDrmFormatModifierListCreateInfoEXT list{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};list.drmFormatModifierCount=1;list.pDrmFormatModifiers=&modifier;
  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;external.pNext=&list;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.pNext=&external;info.imageType=VK_IMAGE_TYPE_2D;info.format=format;info.extent={extent.width,extent.height,1};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;info.usage=usage;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  VK(vkCreateImage(s.device,&info,nullptr,&image));
  VkMemoryRequirements req;vkGetImageMemoryRequirements(s.device,image,&req);allocation=req.size;
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=image;
  VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};exportInfo.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;exportInfo.pNext=&dedicated;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.pNext=&exportInfo;alloc.allocationSize=allocation;
  alloc.memoryTypeIndex=s.type(req.memoryTypeBits,systemMemory?(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT):VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,systemMemory);
  std::cout<<"SHARED_IMAGE allocation_type="<<alloc.memoryTypeIndex<<" flags=0x"<<std::hex<<s.memory.memoryTypes[alloc.memoryTypeIndex].propertyFlags<<std::dec<<" system_requested="<<systemMemory<<"\n";
  VK(vkAllocateMemory(s.device,&alloc,nullptr,&memory));VK(vkBindImageMemory(s.device,image,memory,0));
  VkImageDrmFormatModifierPropertiesEXT actual{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};auto getModifier=(PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(s.device,"vkGetImageDrmFormatModifierPropertiesEXT");
  if(!getModifier)throw std::runtime_error("DRM image modifier entry point missing");
  VK(getModifier(s.device,image,&actual));
  if(actual.drmFormatModifier!=0)throw std::runtime_error("Expected linear DRM modifier");
  VkImageSubresource plane{VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,0,0};vkGetImageSubresourceLayout(s.device,image,&plane,&layout);
  VkSubresourceLayout explicitPlane{};explicitPlane.offset=layout.offset;explicitPlane.rowPitch=layout.rowPitch;
  VkImageDrmFormatModifierExplicitCreateInfoEXT explicitMod{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};explicitMod.drmFormatModifier=0;explicitMod.drmFormatModifierPlaneCount=1;explicitMod.pPlaneLayouts=&explicitPlane;
  external.pNext=&explicitMod;VK(vkCreateImage(o.device,&info,nullptr,&alias));
  VkMemoryRequirements targetReq;vkGetImageMemoryRequirements(o.device,alias,&targetReq);if(targetReq.size>allocation)throw std::runtime_error("Shared image import allocation too small");
  VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};get.memory=memory;get.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;int fd=-1;
  auto exportFd=(PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(s.device,"vkGetMemoryFdKHR");VK(exportFd(s.device,&get,&fd));OwnedFd owned(fd);
  VkMemoryFdPropertiesKHR props{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};auto fdProps=(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(o.device,"vkGetMemoryFdPropertiesKHR");VK(fdProps(o.device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,fd,&props));
  VkMemoryDedicatedAllocateInfo importDedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};importDedicated.image=alias;
  VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};importInfo.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;importInfo.fd=fd;importInfo.pNext=&importDedicated;
  alloc.pNext=&importInfo;alloc.memoryTypeIndex=o.type(targetReq.memoryTypeBits&props.memoryTypeBits,0);
  VK(vkAllocateMemory(o.device,&alloc,nullptr,&imported));owned.release();VK(vkBindImageMemory(o.device,alias,imported,0));
  std::cout<<"SHARED_IMAGE modifier=0 row_pitch="<<layout.rowPitch<<" allocation="<<allocation<<"\n";
 }
 ~SharedLinearImage(){if(output&&output->device){if(alias)vkDestroyImage(output->device,alias,nullptr);if(imported)vkFreeMemory(output->device,imported,nullptr);}if(source&&source->device){if(image)vkDestroyImage(source->device,image,nullptr);if(memory)vkFreeMemory(source->device,memory,nullptr);}}
};
static void sharedOwnership(VkCommandBuffer cb,VkImage image,VkImageLayout old,VkImageLayout next,uint32_t from,uint32_t to,VkAccessFlags srcAccess,VkAccessFlags dstAccess,VkPipelineStageFlags srcStage,VkPipelineStageFlags dstStage){
 VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcAccessMask=srcAccess;barrier.dstAccessMask=dstAccess;barrier.oldLayout=old;barrier.newLayout=next;barrier.srcQueueFamilyIndex=from;barrier.dstQueueFamilyIndex=to;barrier.image=image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(cb,srcStage,dstStage,0,0,nullptr,0,nullptr,1,&barrier);
}
