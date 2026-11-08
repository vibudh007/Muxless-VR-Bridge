#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include "drm-lease-client.h"
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static volatile sig_atomic_t stop=0;
static void interrupted(int){stop=1;}
static void check(VkResult r,const char *what){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(what)+": VkResult="+std::to_string(r));}
#define VK(call) check((call),#call)
#include "wayland_lease.hpp"
#include "vulkan_helpers.hpp"
struct InstanceOwner { VkInstance handle=VK_NULL_HANDLE; ~InstanceOwner(){if(handle)vkDestroyInstance(handle,nullptr);} };
struct App {
 // Declared in lifetime order: buffers die before GPUs, Vulkan display before Wayland fd.
 Wayland wayland;InstanceOwner instanceOwner;VkInstance instance=VK_NULL_HANDLE;GPU render,present;Connector*connector=nullptr;
 VkDisplayKHR display=VK_NULL_HANDLE;VkSurfaceKHR surface=VK_NULL_HANDLE;VkSwapchainKHR swapchain=VK_NULL_HANDLE;
 std::vector<VkImage> swapImages;VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;uint32_t width=640,height=360;bool swapRB=false;
 Buffer transport,imported,readback,reference;
 VkImage image=VK_NULL_HANDLE;VkDeviceMemory imageMemory=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;
 VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;VkDescriptorPool descriptorPool=VK_NULL_HANDLE;VkDescriptorSet set=VK_NULL_HANDLE;
 VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
 VkCommandBuffer producerCB=VK_NULL_HANDLE,consumerCB=VK_NULL_HANDLE;
 VkSemaphore produced=VK_NULL_HANDLE,received=VK_NULL_HANDLE,available=VK_NULL_HANDLE;
 std::vector<VkSemaphore>ready;
 VkFence consumerFence=VK_NULL_HANDLE;
 bool verifyOnly=false;uint32_t verifyEvery=0;std::string connectorName,snapshot;std::vector<double> steadyMs;
 void discover(){
  connector=wayland.select(connectorName);
  const char*exts[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_DISPLAY_EXTENSION_NAME,VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME,VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME};
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="wmr-cross-gpu-toy";app.apiVersion=VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;ci.enabledExtensionCount=4;ci.ppEnabledExtensionNames=exts;VK(vkCreateInstance(&ci,nullptr,&instance));instanceOwner.handle=instance;
  uint32_t n=0;VK(vkEnumeratePhysicalDevices(instance,&n,nullptr));std::vector<VkPhysicalDevice>gpus(n);VK(vkEnumeratePhysicalDevices(instance,&n,gpus.data()));
  struct stat st{};if(fstat(connector->owner->fd,&st))throw std::runtime_error("Cannot identify connector owner");
  for(auto p:gpus){GPU probe;probe.describe(p);std::cout<<"GPU "<<probe.props.deviceName<<" drm="<<probe.drm.primaryMajor<<":"<<probe.drm.primaryMinor<<"\n";
   if(probe.drm.hasPrimary&&probe.drm.primaryMajor==major(st.st_rdev)&&probe.drm.primaryMinor==minor(st.st_rdev))present.describe(p);
   if(probe.props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU&&render.physical==VK_NULL_HANDLE)render.describe(p);
  }
  if(!render.physical||!present.physical||render.physical==present.physical)throw std::runtime_error("Need distinct discrete render GPU and connector-owner GPU");
  std::cout<<"RENDER "<<render.props.deviceName<<"\nPRESENT "<<present.props.deviceName<<"\n";
 }
 void output(){
  wayland.acquire(connector);
  auto get=reinterpret_cast<PFN_vkGetDrmDisplayEXT>(vkGetInstanceProcAddr(instance,"vkGetDrmDisplayEXT"));auto acquire=reinterpret_cast<PFN_vkAcquireDrmDisplayEXT>(vkGetInstanceProcAddr(instance,"vkAcquireDrmDisplayEXT"));
  VK(get(present.physical,wayland.leaseFd,connector->id,&display));VK(acquire(present.physical,wayland.leaseFd,display));
  uint32_t n=0;VK(vkGetDisplayModePropertiesKHR(present.physical,display,&n,nullptr));std::vector<VkDisplayModePropertiesKHR>modes(n);VK(vkGetDisplayModePropertiesKHR(present.physical,display,&n,modes.data()));
  VkDisplayModeKHR mode=VK_NULL_HANDLE;uint64_t bestArea=0;
  for(auto&m:modes){auto v=m.parameters;std::cout<<"MODE "<<v.visibleRegion.width<<"x"<<v.visibleRegion.height<<" "<<v.refreshRate/1000.0<<" Hz\n";
   uint64_t area=uint64_t(v.visibleRegion.width)*v.visibleRegion.height;
   if(std::abs(int(v.refreshRate)-90000)<=1000&&area>bestArea){mode=m.displayMode;bestArea=area;width=v.visibleRegion.width;height=v.visibleRegion.height;}}
  if(!mode)throw std::runtime_error("No 90 Hz display mode");
  uint32_t count=0;VK(vkGetPhysicalDeviceDisplayPlanePropertiesKHR(present.physical,&count,nullptr));std::vector<VkDisplayPlanePropertiesKHR>planes(count);VK(vkGetPhysicalDeviceDisplayPlanePropertiesKHR(present.physical,&count,planes.data()));
  bool found=false;uint32_t plane=0;VkDisplayPlaneAlphaFlagBitsKHR alpha=VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
  for(uint32_t i=0;i<count&&!found;i++){
   uint32_t k=0;VK(vkGetDisplayPlaneSupportedDisplaysKHR(present.physical,i,&k,nullptr));std::vector<VkDisplayKHR>ds(k);VK(vkGetDisplayPlaneSupportedDisplaysKHR(present.physical,i,&k,ds.data()));
   if(std::find(ds.begin(),ds.end(),display)==ds.end())continue;
   VkDisplayPlaneCapabilitiesKHR caps{};VK(vkGetDisplayPlaneCapabilitiesKHR(present.physical,mode,i,&caps));
   if(caps.supportedAlpha&VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR){plane=i;found=true;}
  }
  if(!found)throw std::runtime_error("No opaque display plane");
  VkDisplaySurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};si.displayMode=mode;si.planeIndex=plane;si.planeStackIndex=planes[plane].currentStackIndex;si.transform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;si.globalAlpha=1;si.alphaMode=alpha;si.imageExtent={width,height};VK(vkCreateDisplayPlaneSurfaceKHR(instance,&si,nullptr,&surface));
 }
 void createDevices(){
  render.create(false);present.create(!verifyOnly);
  if(verifyOnly)return;
  VkBool32 supported=VK_FALSE;VK(vkGetPhysicalDeviceSurfaceSupportKHR(present.physical,present.family,surface,&supported));if(!supported)throw std::runtime_error("Selected output queue cannot present");
  uint32_t n=0;VK(vkGetPhysicalDeviceSurfaceFormatsKHR(present.physical,surface,&n,nullptr));std::vector<VkSurfaceFormatKHR>formats(n);VK(vkGetPhysicalDeviceSurfaceFormatsKHR(present.physical,surface,&n,formats.data()));
  VkSurfaceFormatKHR chosen{};bool found=false;
  for(auto wanted:{VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_B8G8R8A8_SRGB})for(auto f:formats)if(!found&&(f.format==wanted||f.format==VK_FORMAT_UNDEFINED)){chosen=f;chosen.format=wanted;found=true;}
  if(!found)throw std::runtime_error("No supported 8-bit RGBA/BGRA output format");
  format=chosen.format;swapRB=format==VK_FORMAT_B8G8R8A8_UNORM||format==VK_FORMAT_B8G8R8A8_SRGB;
  VkSurfaceCapabilitiesKHR caps{};VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(present.physical,surface,&caps));if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))throw std::runtime_error("Display swapchain cannot accept transfer copies");
  VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};sc.surface=surface;sc.minImageCount=std::max(2u,caps.minImageCount);if(caps.maxImageCount)sc.minImageCount=std::min(sc.minImageCount,caps.maxImageCount);sc.imageFormat=format;sc.imageColorSpace=chosen.colorSpace;sc.imageExtent={width,height};sc.imageArrayLayers=1;sc.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;sc.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;sc.preTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;sc.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;sc.presentMode=VK_PRESENT_MODE_FIFO_KHR;sc.clipped=VK_TRUE;VK(vkCreateSwapchainKHR(present.device,&sc,nullptr,&swapchain));
  VK(vkGetSwapchainImagesKHR(present.device,swapchain,&n,nullptr));swapImages.resize(n);VK(vkGetSwapchainImagesKHR(present.device,swapchain,&n,swapImages.data()));
  std::cout<<"OUTPUT_READY "<<width<<"x"<<height<<" at 90 Hz format="<<format<<" buffers="<<n<<"\n";
 }
 void resources(const std::string&shader){
  VkDeviceSize bytes=VkDeviceSize(width)*height*4;
  // Negotiate the exact external-buffer usages before attempting export/import.
  for(auto*g:{&render,&present}){VkPhysicalDeviceExternalBufferInfo bi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;VkExternalBufferProperties bp{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};vkGetPhysicalDeviceExternalBufferProperties(g->physical,&bi,&bp);auto required=g==&render?VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT:VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;if(!(bp.externalMemoryProperties.externalMemoryFeatures&required))throw std::runtime_error("DMA-BUF transfer-buffer capability unavailable");}
  transport.make(render,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
  int fd=transport.exportFd();imported.make(present,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,fd,transport.allocation);
  reference.make(render,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);readback.make(present,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};im.imageType=VK_IMAGE_TYPE_2D;im.format=VK_FORMAT_R8G8B8A8_UNORM;im.extent={width,height,1};im.mipLevels=im.arrayLayers=1;im.samples=VK_SAMPLE_COUNT_1_BIT;im.tiling=VK_IMAGE_TILING_OPTIMAL;im.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;VK(vkCreateImage(render.device,&im,nullptr,&image));
  VkMemoryRequirements req;vkGetImageMemoryRequirements(render.device,image,&req);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=render.type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VK(vkAllocateMemory(render.device,&ai,nullptr,&imageMemory));VK(vkBindImageMemory(render.device,image,imageMemory,0));
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=im.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VK(vkCreateImageView(render.device,&vi,nullptr,&view));
  VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};li.bindingCount=1;li.pBindings=&binding;VK(vkCreateDescriptorSetLayout(render.device,&li,nullptr,&setLayout));
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&ps;VK(vkCreateDescriptorPool(render.device,&dp,nullptr,&descriptorPool));
  VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=descriptorPool;da.descriptorSetCount=1;da.pSetLayouts=&setLayout;VK(vkAllocateDescriptorSets(render.device,&da,&set));
  VkDescriptorImageInfo di{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&di;vkUpdateDescriptorSets(render.device,1,&write,0,nullptr);
  VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,16};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&setLayout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;VK(vkCreatePipelineLayout(render.device,&pl,nullptr,&pipelineLayout));
  std::ifstream in(shader,std::ios::binary|std::ios::ate);if(!in)throw std::runtime_error("Cannot read shader: "+shader);auto length=in.tellg();if(length<=0||length%4)throw std::runtime_error("Invalid shader length");std::vector<uint32_t>code(size_t(length)/4);in.seekg(0);in.read(reinterpret_cast<char*>(code.data()),length);
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=size_t(length);sm.pCode=code.data();VkShaderModule module;VK(vkCreateShaderModule(render.device,&sm,nullptr,&module));
  VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};cp.layout=pipelineLayout;VkResult r=vkCreateComputePipelines(render.device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline);vkDestroyShaderModule(render.device,module,nullptr);VK(r);
  producerCB=render.commands();consumerCB=present.commands();
  for(auto*g:{&render,&present}){VkPhysicalDeviceExternalSemaphoreInfo si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};si.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};vkGetPhysicalDeviceExternalSemaphoreProperties(g->physical,&si,&sp);auto required=g==&render?VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT:VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;if(!(sp.externalSemaphoreFeatures&required))throw std::runtime_error("SYNC_FD semaphore capability unavailable");}
  VkExportSemaphoreCreateInfo es{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};es.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};sem.pNext=&es;VK(vkCreateSemaphore(render.device,&sem,nullptr,&produced));sem.pNext=nullptr;VK(vkCreateSemaphore(present.device,&sem,nullptr,&received));
  if(!verifyOnly){VK(vkCreateSemaphore(present.device,&sem,nullptr,&available));ready.resize(swapImages.size());for(auto&s:ready)VK(vkCreateSemaphore(present.device,&sem,nullptr,&s));}
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK(vkCreateFence(present.device,&fi,nullptr,&consumerFence));
 }
 void frame(uint32_t frame,float time,bool verify){
  const auto tick=std::chrono::steady_clock::now();
  auto ms=[&](){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-tick).count();};
  uint32_t index=0;if(!verifyOnly)VK(vkAcquireNextImageKHR(present.device,swapchain,5000000000ull,available,VK_NULL_HANDLE,&index));
  double acquiredAt=ms();
  auto bytes=transport.size;
  begin(producerCB);
  if(frame)bufferBarrier(producerCB,transport.buffer,bytes,VK_QUEUE_FAMILY_EXTERNAL,render.family,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  imageBarrier(producerCB,image,frame?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,frame?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_SHADER_WRITE_BIT,frame?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  struct Params{uint32_t width,height;float time;uint32_t swapRB;}params{width,height,time,swapRB?1u:0u};
  vkCmdBindPipeline(producerCB,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(producerCB,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);vkCmdPushConstants(producerCB,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),&params);vkCmdDispatch(producerCB,(width+15)/16,(height+15)/16,1);
  imageBarrier(producerCB,image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};vkCmdCopyImageToBuffer(producerCB,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,transport.buffer,1,&copy);
  if(verify){vkCmdCopyImageToBuffer(producerCB,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,reference.buffer,1,&copy);bufferBarrier(producerCB,reference.buffer,bytes,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);}
  bufferBarrier(producerCB,transport.buffer,bytes,render.family,VK_QUEUE_FAMILY_EXTERNAL,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);VK(vkEndCommandBuffer(producerCB));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&producerCB;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&produced;VK(vkQueueSubmit(render.queue,1,&submit,VK_NULL_HANDLE));
  VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};get.semaphore=produced;get.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;int fd=-1;
  auto exportSem=reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(vkGetDeviceProcAddr(render.device,"vkGetSemaphoreFdKHR"));VK(exportSem(render.device,&get,&fd));
  VkImportSemaphoreFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};imp.semaphore=received;imp.flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;imp.handleType=get.handleType;imp.fd=fd;auto importSem=reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(vkGetDeviceProcAddr(present.device,"vkImportSemaphoreFdKHR"));VK(importSem(present.device,&imp));
  double submittedAt=ms();
  begin(consumerCB);bufferBarrier(consumerCB,imported.buffer,bytes,VK_QUEUE_FAMILY_EXTERNAL,present.family,0,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  if(verify){VkBufferCopy bc{0,0,bytes};vkCmdCopyBuffer(consumerCB,imported.buffer,readback.buffer,1,&bc);bufferBarrier(consumerCB,readback.buffer,bytes,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);}
  if(!verifyOnly){imageBarrier(consumerCB,swapImages[index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);vkCmdCopyBufferToImage(consumerCB,imported.buffer,swapImages[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);imageBarrier(consumerCB,swapImages[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);}
  bufferBarrier(consumerCB,imported.buffer,bytes,present.family,VK_QUEUE_FAMILY_EXTERNAL,VK_ACCESS_TRANSFER_READ_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);VK(vkEndCommandBuffer(consumerCB));
  VkSemaphore waits[]={received,available};VkPipelineStageFlags stages[]={VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT};submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=verifyOnly?1:2;submit.pWaitSemaphores=waits;submit.pWaitDstStageMask=stages;submit.commandBufferCount=1;submit.pCommandBuffers=&consumerCB;submit.signalSemaphoreCount=verifyOnly?0:1;submit.pSignalSemaphores=verifyOnly?nullptr:&ready[index];VK(vkQueueSubmit(present.queue,1,&submit,consumerFence));
  if(!verifyOnly){VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&ready[index];pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&index;VK(vkQueuePresentKHR(present.queue,&pi));}
  VK(vkWaitForFences(present.device,1,&consumerFence,VK_TRUE,5000000000ull));VK(vkResetFences(present.device,1,&consumerFence));
  // First prototype intentionally serializes reuse. No pixel CPU copies occur in transport.
  VK(vkQueueWaitIdle(render.queue));
  double completedAt=ms();
  if(verify){auto*a=static_cast<uint8_t*>(reference.map());auto*b=static_cast<uint8_t*>(readback.map());size_t mismatches=0;uint64_t hash=1469598103934665603ull;bool varied=false;
   for(size_t i=0;i<bytes;i++){mismatches+=a[i]!=b[i];hash=(hash^b[i])*1099511628211ull;if(i>3&&b[i]!=b[i%4])varied=true;}
   std::cout<<"VERIFY frame="<<frame<<" bytes="<<bytes<<" mismatches="<<mismatches<<" fnv1a="<<std::hex<<hash<<std::dec<<" nonuniform="<<varied<<"\n";
   if(mismatches||!varied)throw std::runtime_error("Pixel verification failed");
   if(frame==0&&!snapshot.empty()){
    std::vector<char>rgb(size_t(width)*height*3);for(size_t i=0,j=0;i<bytes;i+=4,j+=3){rgb[j]=char(b[i+(swapRB?2:0)]);rgb[j+1]=char(b[i+1]);rgb[j+2]=char(b[i+(swapRB?0:2)]);}
    std::ofstream ppm(snapshot,std::ios::binary);if(!ppm)throw std::runtime_error("Cannot write snapshot");ppm<<"P6\n"<<width<<" "<<height<<"\n255\n";ppm.write(rgb.data(),rgb.size());if(!ppm)throw std::runtime_error("Snapshot write failed");
   }
  }
  if(frame>5&&!verify)steadyMs.push_back(ms());
  if(frame<5||frame%90==0)std::cout<<"TIMING frame="<<frame<<" acquire_ms="<<acquiredAt<<" producer_submit_ms="<<submittedAt-acquiredAt<<" consumer_complete_ms="<<completedAt-submittedAt<<" verification_ms="<<ms()-completedAt<<" total_ms="<<ms()<<"\n";
 }
 void destroy(){
  if(render.device)vkDeviceWaitIdle(render.device);
  if(present.device)vkDeviceWaitIdle(present.device);
  if(render.device){if(produced)vkDestroySemaphore(render.device,produced,nullptr);if(pipeline)vkDestroyPipeline(render.device,pipeline,nullptr);if(pipelineLayout)vkDestroyPipelineLayout(render.device,pipelineLayout,nullptr);if(descriptorPool)vkDestroyDescriptorPool(render.device,descriptorPool,nullptr);if(setLayout)vkDestroyDescriptorSetLayout(render.device,setLayout,nullptr);if(view)vkDestroyImageView(render.device,view,nullptr);if(image)vkDestroyImage(render.device,image,nullptr);if(imageMemory)vkFreeMemory(render.device,imageMemory,nullptr);}
  if(present.device){for(auto s:ready)vkDestroySemaphore(present.device,s,nullptr);if(available)vkDestroySemaphore(present.device,available,nullptr);if(received)vkDestroySemaphore(present.device,received,nullptr);if(consumerFence)vkDestroyFence(present.device,consumerFence,nullptr);if(swapchain)vkDestroySwapchainKHR(present.device,swapchain,nullptr);}
  if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);
  if(display){auto release=reinterpret_cast<PFN_vkReleaseDisplayEXT>(vkGetInstanceProcAddr(instance,"vkReleaseDisplayEXT"));if(release)release(present.physical,display);}
 }
 ~App(){destroy();}
};
int main(int argc,char**argv){
 std::cout.setf(std::ios::unitbuf);signal(SIGINT,interrupted);signal(SIGTERM,interrupted);
 bool verify=false,list=false;uint32_t verifyEvery=0;double seconds=12;std::string name,snapshot,shader="prototype/build/cube.spv";
 try{
  for(int i=1;i<argc;i++){std::string a=argv[i];if(a=="--verify-only")verify=true;else if(a=="--list")list=true;else if(a=="--seconds"&&i+1<argc)seconds=std::stod(argv[++i]);else if(a=="--connector"&&i+1<argc)name=argv[++i];else if(a=="--shader"&&i+1<argc)shader=argv[++i];else if(a=="--snapshot"&&i+1<argc)snapshot=argv[++i];else if(a=="--verify-every"&&i+1<argc)verifyEvery=std::stoul(argv[++i]);else throw std::runtime_error("Usage: bridge-toy [--list] [--verify-only] [--seconds N] [--connector NAME] [--shader FILE] [--snapshot FILE.ppm] [--verify-every N]");}
  if(!std::isfinite(seconds)||seconds<=0||seconds>300)throw std::runtime_error("Duration must be 0 < seconds <= 300");
  {App app;app.verifyOnly=verify;app.verifyEvery=verifyEvery;app.connectorName=name;app.snapshot=snapshot;app.discover();if(!list){if(!verify)app.output();app.createDevices();app.resources(shader);
   auto start=std::chrono::steady_clock::now();uint32_t frames=0;
   while(!stop&&!app.wayland.finished){
    wl_display_dispatch_pending(app.wayland.display);pollfd event{wl_display_get_fd(app.wayland.display),POLLIN,0};if(poll(&event,1,0)>0&&wl_display_dispatch(app.wayland.display)<0)throw std::runtime_error("Wayland connection lost");
    if(app.wayland.finished)break;
    float elapsed=std::chrono::duration<float>(std::chrono::steady_clock::now()-start).count();if((verify&&frames>=16)||(!verify&&elapsed>=seconds))break;app.frame(frames,elapsed,verify||frames==0||(app.verifyEvery&&frames%app.verifyEvery==0));frames++;}
   double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();std::cout<<"RESULT frames="<<frames<<" seconds="<<elapsed<<" submission_rate="<<frames/elapsed<<" mode="<<(verify?"transfer-verification":"leased-headset")<<" cpu_pixel_copies=0\n";
   if(!app.steadyMs.empty()){std::sort(app.steadyMs.begin(),app.steadyMs.end());size_t n=app.steadyMs.size();std::cout<<"STEADY nonverification_frames="<<n<<" p50_frame_ms="<<app.steadyMs[n/2]<<" p95_frame_ms="<<app.steadyMs[std::min(n-1,size_t(n*0.95))]<<" max_frame_ms="<<app.steadyMs.back()<<"\n";}
   if(app.wayland.finished)throw std::runtime_error("Display lease revoked during run");
  }}
  return 0;
 }catch(const std::exception&e){std::cerr<<"ERROR "<<e.what()<<"\n";return 1;}
}
