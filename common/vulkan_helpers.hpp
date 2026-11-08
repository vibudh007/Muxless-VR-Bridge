#pragma once
#include <iostream>
#include <unistd.h>
struct OwnedFd {
 int fd;explicit OwnedFd(int value):fd(value){}
 OwnedFd(const OwnedFd&)=delete;OwnedFd&operator=(const OwnedFd&)=delete;
 ~OwnedFd(){if(fd>=0)close(fd);}
 void release(){fd=-1;}
};
// Shared Vulkan device, external-buffer and command helpers for the toy and shim.
struct GPU {
 bool ownsDevice=true;VkPhysicalDevice physical=VK_NULL_HANDLE;VkDevice device=VK_NULL_HANDLE;VkQueue queue=VK_NULL_HANDLE;uint32_t family=0;VkCommandPool pool=VK_NULL_HANDLE;
 VkPhysicalDeviceProperties props{};VkPhysicalDeviceMemoryProperties memory{};VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
 void describe(VkPhysicalDevice p){physical=p;VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p2.pNext=&drm;vkGetPhysicalDeviceProperties2(p,&p2);props=p2.properties;vkGetPhysicalDeviceMemoryProperties(p,&memory);}
 void create(bool present){
  uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties>qs(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,qs.data());
  bool found=false;for(uint32_t i=0;i<n;i++)if((qs[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){family=i;found=true;break;}
  if(!found)throw std::runtime_error("No graphics+compute queue");
  const char*exts[]={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  float priority=1;VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};q.queueFamilyIndex=family;q.queueCount=1;q.pQueuePriorities=&priority;
  VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};ci.queueCreateInfoCount=1;ci.pQueueCreateInfos=&q;ci.enabledExtensionCount=present?4:3;ci.ppEnabledExtensionNames=exts;
  VK(vkCreateDevice(physical,&ci,nullptr,&device));vkGetDeviceQueue(device,family,0,&queue);
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VK(vkCreateCommandPool(device,&pi,nullptr,&pool));
 }
 uint32_t findType(uint32_t bits, VkMemoryPropertyFlags flags, bool preferSystem) const {
  uint32_t first = memory.memoryTypeCount;
  for(uint32_t i=0;i<memory.memoryTypeCount;i++){
   if(!(bits&(1u<<i)))continue;
   const auto properties=memory.memoryTypes[i].propertyFlags;
   if((properties&flags)!=flags)continue;
   if(!preferSystem||!(properties&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))return i;
   if(first==memory.memoryTypeCount)first=i;
  }
  return first;
 }
 uint32_t type(uint32_t bits,VkMemoryPropertyFlags flags,bool preferSystem=false){
  const auto index=findType(bits,flags,preferSystem);
  if(index==memory.memoryTypeCount)throw std::runtime_error("No compatible memory type");
  return index;
 }
 VkCommandBuffer commands(){VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;VkCommandBuffer cb;VK(vkAllocateCommandBuffers(device,&ai,&cb));return cb;}
 ~GPU(){if(device){if(ownsDevice||pool)vkDeviceWaitIdle(device);if(pool)vkDestroyCommandPool(device,pool,nullptr);if(ownsDevice)vkDestroyDevice(device,nullptr);}}
};
struct Buffer {GPU*g=nullptr;VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkDeviceSize size=0,allocation=0;void*mapped=nullptr;
 void make(GPU&gpu,VkDeviceSize bytes,VkBufferUsageFlags usage,bool external=false,int importFd=-1,VkDeviceSize importSize=0){
  OwnedFd importedFd(importFd); // Vulkan consumes an import fd only on successful allocation.
  g=&gpu;size=bytes;VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};ext.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=bytes;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.pNext=external?&ext:nullptr;VK(vkCreateBuffer(g->device,&ci,nullptr,&buffer));
  VkMemoryRequirements req;vkGetBufferMemoryRequirements(g->device,buffer,&req);allocation=importFd>=0?importSize:req.size;
  if(allocation<req.size)throw std::runtime_error("Imported allocation smaller than consumer requirements");
  uint32_t bits=req.memoryTypeBits;
  VkImportMemoryFdInfoKHR im{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};im.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;im.fd=importFd;
  if(importFd>=0){VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};auto f=reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(g->device,"vkGetMemoryFdPropertiesKHR"));VK(f(g->device,im.handleType,importFd,&fp));bits&=fp.memoryTypeBits;}
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.buffer=buffer;
  VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};ex.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;ex.pNext=&dedicated;im.pNext=&dedicated;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=allocation;
  // System-visible transport is still written/read by GPUs; the CPU never copies its pixels.
  if(importFd>=0)ai.memoryTypeIndex=g->type(bits,0);
  else {
   VkMemoryPropertyFlags flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   const auto cached = external ? g->memory.memoryTypeCount
                                : g->findType(bits, flags|VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true);
   ai.memoryTypeIndex = cached == g->memory.memoryTypeCount ? g->type(bits,flags,true) : cached;
  }
  ai.pNext=importFd>=0?static_cast<void*>(&im):(external?static_cast<void*>(&ex):nullptr);
  VK(vkAllocateMemory(g->device,&ai,nullptr,&memory));importedFd.release();VK(vkBindBufferMemory(g->device,buffer,memory,0));
  std::cout<<"BUFFER device="<<g->props.deviceName<<" size="<<bytes<<" memory_type="<<ai.memoryTypeIndex<<" flags=0x"<<std::hex<<g->memory.memoryTypes[ai.memoryTypeIndex].propertyFlags<<std::dec<<" imported="<<(importFd>=0)<<"\n";
 }
 int exportFd(){VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};info.memory=memory;info.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;int fd=-1;auto f=reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(g->device,"vkGetMemoryFdKHR"));VK(f(g->device,&info,&fd));return fd;}
 void*map(){if(!mapped)VK(vkMapMemory(g->device,memory,0,VK_WHOLE_SIZE,0,&mapped));return mapped;}
 ~Buffer(){if(g&&g->device){if(mapped)vkUnmapMemory(g->device,memory);if(buffer)vkDestroyBuffer(g->device,buffer,nullptr);if(memory)vkFreeMemory(g->device,memory,nullptr);}}
};
static void begin(VkCommandBuffer cb){VK(vkResetCommandBuffer(cb,0));VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};VK(vkBeginCommandBuffer(cb,&b));}
static void bufferBarrier(VkCommandBuffer cb,VkBuffer b,VkDeviceSize bytes,uint32_t src,uint32_t dst,VkAccessFlags sa,VkAccessFlags da,VkPipelineStageFlags ss,VkPipelineStageFlags ds){VkBufferMemoryBarrier x{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};x.srcAccessMask=sa;x.dstAccessMask=da;x.srcQueueFamilyIndex=src;x.dstQueueFamilyIndex=dst;x.buffer=b;x.size=bytes;vkCmdPipelineBarrier(cb,ss,ds,0,0,nullptr,1,&x,0,nullptr);}
static void imageBarrier(VkCommandBuffer cb,VkImage image,VkImageLayout old,VkImageLayout next,VkAccessFlags sa,VkAccessFlags da,VkPipelineStageFlags ss,VkPipelineStageFlags ds){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=sa;b.dstAccessMask=da;b.oldLayout=old;b.newLayout=next;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};vkCmdPipelineBarrier(cb,ss,ds,0,0,nullptr,0,nullptr,1,&b);}
