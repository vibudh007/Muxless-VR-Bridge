#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <chrono>
#include <cstdio>
static void check(VkResult result,const char*what){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(what)+" result="+std::to_string(result));}
#define VK(call) check((call),#call)
#include "vulkan_helpers.hpp"
#ifndef WMR_SHARED_IMAGE_HEADER
#define WMR_SHARED_IMAGE_HEADER "shared_image.hpp"
#endif
#include WMR_SHARED_IMAGE_HEADER
struct Bench {
 VkInstance instance=VK_NULL_HANDLE,outputInstance=VK_NULL_HANDLE;GPU source,output;
 std::unique_ptr<SharedLinearImage> shared;Buffer transport,imported,reference,readback;
 VkImage image=VK_NULL_HANDLE,target=VK_NULL_HANDLE;VkDeviceMemory imageMemory=VK_NULL_HANDLE,targetMemory=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;
 VkDescriptorSetLayout setLayout=VK_NULL_HANDLE;VkDescriptorPool descriptorPool=VK_NULL_HANDLE;VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;VkDescriptorSet set=VK_NULL_HANDLE;
 VkSemaphore produced=VK_NULL_HANDLE,received=VK_NULL_HANDLE;VkFence done=VK_NULL_HANDLE;VkQueryPool sourceQueries=VK_NULL_HANDLE,outputQueries=VK_NULL_HANDLE;VkCommandBuffer sourceCB=VK_NULL_HANDLE,outputCB=VK_NULL_HANDLE;
 uint32_t externalFamily=getenv("WMR_BENCH_FOREIGN")&&!strcmp(getenv("WMR_BENCH_FOREIGN"),"1")?VK_QUEUE_FAMILY_FOREIGN_EXT:VK_QUEUE_FAMILY_EXTERNAL;
 bool reverseAllocation=false;uint64_t drmModifier=0;bool direct=false,systemMemory=true;uint32_t width=2880,height=1440;
 void device(GPU&g){
  uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(g.physical,&count,nullptr);std::vector<VkQueueFamilyProperties>families(count);vkGetPhysicalDeviceQueueFamilyProperties(g.physical,&count,families.data());bool found=false;
  for(uint32_t i=0;i<count;i++)if((families[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)&&families[i].timestampValidBits==64){g.family=i;found=true;break;}
  if(!found)throw std::runtime_error("Benchmark requires graphics/compute queue and 64-bit timestamps");
  std::vector<const char*>exts={VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME};
  if(externalFamily==VK_QUEUE_FAMILY_FOREIGN_EXT)exts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
  float priority=1;VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};q.queueFamilyIndex=g.family;q.queueCount=1;q.pQueuePriorities=&priority;
  VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};ci.queueCreateInfoCount=1;ci.pQueueCreateInfos=&q;ci.enabledExtensionCount=exts.size();ci.ppEnabledExtensionNames=exts.data();VK(vkCreateDevice(g.physical,&ci,nullptr,&g.device));vkGetDeviceQueue(g.device,g.family,0,&g.queue);
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=g.family;pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VK(vkCreateCommandPool(g.device,&pool,nullptr,&g.pool));
 }
 void localImage(GPU&g,VkImageUsageFlags usage,VkImage&out,VkDeviceMemory&mem){
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;ci.extent={width,height,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=usage;
  VK(vkCreateImage(g.device,&ci,nullptr,&out));VkMemoryRequirements req;vkGetImageMemoryRequirements(g.device,out,&req);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=g.type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VK(vkAllocateMemory(g.device,&ai,nullptr,&mem));VK(vkBindImageMemory(g.device,out,mem,0));
 }
 void init(){
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;VK(vkCreateInstance(&ci,nullptr,&instance));
  uint32_t n=0;VK(vkEnumeratePhysicalDevices(instance,&n,nullptr));std::vector<VkPhysicalDevice>gpus(n);VK(vkEnumeratePhysicalDevices(instance,&n,gpus.data()));
  for(auto p:gpus){VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(p,&props);if(props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU&&!source.physical)source.describe(p);if(props.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU&&!output.physical)output.describe(p);}
  if(!source.physical||!output.physical)throw std::runtime_error("Benchmark needs dGPU and iGPU");
  // Optional diagnostic isolation: RADV names RGP output by process/submit,
  // so two traced devices can overwrite one another. Trace the producer's
  // instance only. This single-threaded toy never changes another process env.
  if(getenv("WMR_BENCH_PROFILE_SOURCE")){
   const char*value=getenv("MESA_VK_TRACE");bool had=value;std::string saved=value?value:"";
   unsetenv("MESA_VK_TRACE");auto result=vkCreateInstance(&ci,nullptr,&outputInstance);
   if(had)setenv("MESA_VK_TRACE",saved.c_str(),1);VK(result);
   uint32_t m=0;VK(vkEnumeratePhysicalDevices(outputInstance,&m,nullptr));std::vector<VkPhysicalDevice>others(m);VK(vkEnumeratePhysicalDevices(outputInstance,&m,others.data()));
   bool found=false;for(auto physical:others){VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical,&props);if(props.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU&&props.vendorID==output.props.vendorID&&props.deviceID==output.props.deviceID){output.describe(physical);found=true;break;}}
   if(!found)throw std::runtime_error("Profile isolation could not rediscover display GPU");
   puts("PROFILE_SOURCE_INSTANCE_ONLY=1");
  }
  device(source);device(output);
  printf("OWNERSHIP=%s\n",externalFamily==VK_QUEUE_FAMILY_FOREIGN_EXT?"foreign":"external-legacy");
  printf("MODE=%s source=%s output=%s extent=%ux%u\n",direct?"shared-image":"two-copy",source.props.deviceName,output.props.deviceName,width,height);
  VkDeviceSize bytes=VkDeviceSize(width)*height*4;
  if(direct){shared=std::make_unique<SharedLinearImage>();shared->make(source,output,VK_FORMAT_R8G8B8A8_UNORM,{width,height},VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,vkGetPhysicalDeviceImageFormatProperties2,systemMemory,vkGetPhysicalDeviceImageFormatProperties2,drmModifier,reverseAllocation);image=shared->image;}
  else{localImage(source,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,image,imageMemory);transport.make(source,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);imported.make(output,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,transport.exportFd(),transport.allocation);}
  localImage(output,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,target,targetMemory);reference.make(source,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);readback.make(output,bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=VK_FORMAT_R8G8B8A8_UNORM;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VK(vkCreateImageView(source.device,&vi,nullptr,&view));
  VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=1;sl.pBindings=&binding;VK(vkCreateDescriptorSetLayout(source.device,&sl,nullptr,&setLayout));
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1};VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=1;dp.poolSizeCount=1;dp.pPoolSizes=&size;VK(vkCreateDescriptorPool(source.device,&dp,nullptr,&descriptorPool));VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=descriptorPool;da.descriptorSetCount=1;da.pSetLayouts=&setLayout;VK(vkAllocateDescriptorSets(source.device,&da,&set));
  VkDescriptorImageInfo descriptor{VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&descriptor;vkUpdateDescriptorSets(source.device,1,&write,0,nullptr);
  VkPushConstantRange pc{VK_SHADER_STAGE_COMPUTE_BIT,0,16};VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&setLayout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&pc;VK(vkCreatePipelineLayout(source.device,&pl,nullptr,&pipelineLayout));
  std::ifstream file("prototype/build/cube.spv",std::ios::binary|std::ios::ate);auto length=file.tellg();if(length<=0||length%4)throw std::runtime_error("Build toy shader first");std::vector<uint32_t>code(size_t(length)/4);file.seekg(0);file.read((char*)code.data(),length);
  VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=length;sm.pCode=code.data();VkShaderModule module;VK(vkCreateShaderModule(source.device,&sm,nullptr,&module));VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipelineInfo.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};pipelineInfo.layout=pipelineLayout;auto r=vkCreateComputePipelines(source.device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&pipeline);vkDestroyShaderModule(source.device,module,nullptr);VK(r);
  VkExportSemaphoreCreateInfo ex{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};ex.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};si.pNext=&ex;VK(vkCreateSemaphore(source.device,&si,nullptr,&produced));si.pNext=nullptr;VK(vkCreateSemaphore(output.device,&si,nullptr,&received));VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK(vkCreateFence(output.device,&fi,nullptr,&done));
  VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qp.queryType=VK_QUERY_TYPE_TIMESTAMP;qp.queryCount=4;VK(vkCreateQueryPool(source.device,&qp,nullptr,&sourceQueries));qp.queryCount=2;VK(vkCreateQueryPool(output.device,&qp,nullptr,&outputQueries));sourceCB=source.commands();outputCB=output.commands();
 }
 void frame(unsigned number,FILE*csv){
  bool verify=number%30==0;VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
  auto start=std::chrono::steady_clock::now();begin(sourceCB);vkCmdResetQueryPool(sourceCB,sourceQueries,0,4);
  if(direct&&number)sharedOwnership(sourceCB,image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,externalFamily,source.family,0,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  else if(!direct&&number)bufferBarrier(sourceCB,transport.buffer,transport.size,externalFamily,source.family,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  if(!direct||!number)imageBarrier(sourceCB,image,number?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,number?VK_ACCESS_TRANSFER_READ_BIT:0,VK_ACCESS_SHADER_WRITE_BIT,number?VK_PIPELINE_STAGE_TRANSFER_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  vkCmdWriteTimestamp(sourceCB,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,sourceQueries,0);
  struct Params{uint32_t w,h;float time;uint32_t swap;}params{width,height,number/90.f,0};vkCmdBindPipeline(sourceCB,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(sourceCB,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);vkCmdPushConstants(sourceCB,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,16,&params);vkCmdDispatch(sourceCB,(width+15)/16,(height+15)/16,1);
  imageBarrier(sourceCB,image,VK_IMAGE_LAYOUT_GENERAL,direct?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);vkCmdWriteTimestamp(sourceCB,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,sourceQueries,1);vkCmdWriteTimestamp(sourceCB,VK_PIPELINE_STAGE_TRANSFER_BIT,sourceQueries,2);
  if(!direct)vkCmdCopyImageToBuffer(sourceCB,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,transport.buffer,1,&copy);
  if(verify){vkCmdCopyImageToBuffer(sourceCB,image,direct?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,reference.buffer,1,&copy);bufferBarrier(sourceCB,reference.buffer,reference.size,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);}
  if(direct)sharedOwnership(sourceCB,image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,source.family,externalFamily,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,0,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  else bufferBarrier(sourceCB,transport.buffer,transport.size,source.family,externalFamily,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  vkCmdWriteTimestamp(sourceCB,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,sourceQueries,3);VK(vkEndCommandBuffer(sourceCB));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&sourceCB;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&produced;VK(vkQueueSubmit(source.queue,1,&submit,VK_NULL_HANDLE));
  VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};get.semaphore=produced;get.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;int fd;auto exp=(PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(source.device,"vkGetSemaphoreFdKHR");VK(exp(source.device,&get,&fd));OwnedFd owned(fd);VkImportSemaphoreFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};imp.semaphore=received;imp.flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;imp.handleType=get.handleType;imp.fd=fd;auto import=(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(output.device,"vkImportSemaphoreFdKHR");VK(import(output.device,&imp));owned.release();
  begin(outputCB);vkCmdResetQueryPool(outputCB,outputQueries,0,2);
  if(direct)sharedOwnership(outputCB,shared->alias,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,externalFamily,output.family,0,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  else bufferBarrier(outputCB,imported.buffer,imported.size,externalFamily,output.family,0,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  imageBarrier(outputCB,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);vkCmdWriteTimestamp(outputCB,VK_PIPELINE_STAGE_TRANSFER_BIT,outputQueries,0);
  if(direct){VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.extent={width,height,1};vkCmdCopyImage(outputCB,shared->alias,VK_IMAGE_LAYOUT_GENERAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);}
  else vkCmdCopyBufferToImage(outputCB,imported.buffer,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
  if(verify){imageBarrier(outputCB,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);vkCmdCopyImageToBuffer(outputCB,target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&copy);bufferBarrier(outputCB,readback.buffer,readback.size,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);}
  if(direct)sharedOwnership(outputCB,shared->alias,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,output.family,externalFamily,VK_ACCESS_TRANSFER_READ_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  else bufferBarrier(outputCB,imported.buffer,imported.size,output.family,externalFamily,VK_ACCESS_TRANSFER_READ_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  vkCmdWriteTimestamp(outputCB,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,outputQueries,1);VK(vkEndCommandBuffer(outputCB));
  VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&received;submit.pWaitDstStageMask=&stage;submit.commandBufferCount=1;submit.pCommandBuffers=&outputCB;VK(vkQueueSubmit(output.queue,1,&submit,done));VK(vkWaitForFences(output.device,1,&done,VK_TRUE,5000000000ull));VK(vkResetFences(output.device,1,&done));VK(vkQueueWaitIdle(source.queue));
  uint64_t s[4],o[2];VK(vkGetQueryPoolResults(source.device,sourceQueries,0,4,sizeof(s),s,8,VK_QUERY_RESULT_64_BIT));VK(vkGetQueryPoolResults(output.device,outputQueries,0,2,sizeof(o),o,8,VK_QUERY_RESULT_64_BIT));auto ms=[&](uint64_t a,uint64_t b){return (b-a)*double(source.props.limits.timestampPeriod)/1e6;};
  double host=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();fprintf(csv,"%u,%d,%.6f,%.6f,%.6f,%.6f,%.6f\n",number,verify,ms(s[0],s[1]),ms(s[2],s[3]),ms(s[0],s[3]),(o[1]-o[0])*double(output.props.limits.timestampPeriod)/1e6,host);
  if(verify){auto*a=(unsigned char*)reference.map();auto*b=(unsigned char*)readback.map();size_t mismatches=0;bool varied=false;for(size_t i=0;i<readback.size;i++){mismatches+=a[i]!=b[i];if(i>3&&b[i]!=b[i%4])varied=true;}printf("VERIFY frame=%u bytes=%llu mismatches=%zu nonuniform=%d\n",number,(unsigned long long)readback.size,mismatches,varied);if(mismatches||!varied)throw std::runtime_error("Final output image byte comparison failed");}
 }
 ~Bench(){
  if(source.device)vkDeviceWaitIdle(source.device);if(output.device)vkDeviceWaitIdle(output.device);
  if(source.device){if(pipeline)vkDestroyPipeline(source.device,pipeline,nullptr);if(pipelineLayout)vkDestroyPipelineLayout(source.device,pipelineLayout,nullptr);if(descriptorPool)vkDestroyDescriptorPool(source.device,descriptorPool,nullptr);if(setLayout)vkDestroyDescriptorSetLayout(source.device,setLayout,nullptr);if(view)vkDestroyImageView(source.device,view,nullptr);if(!direct&&image)vkDestroyImage(source.device,image,nullptr);if(imageMemory)vkFreeMemory(source.device,imageMemory,nullptr);if(produced)vkDestroySemaphore(source.device,produced,nullptr);if(sourceQueries)vkDestroyQueryPool(source.device,sourceQueries,nullptr);}
  if(output.device){if(target)vkDestroyImage(output.device,target,nullptr);if(targetMemory)vkFreeMemory(output.device,targetMemory,nullptr);if(received)vkDestroySemaphore(output.device,received,nullptr);if(done)vkDestroyFence(output.device,done,nullptr);if(outputQueries)vkDestroyQueryPool(output.device,outputQueries,nullptr);}
  // Instances live until process exit, after member-owned devices/resources die.
 }
};
int main(int argc,char**argv){try{if(argc<3){fprintf(stderr,"Usage: shared-image-bench two-copy|shared|shared-local OUTPUT.csv [frames=180]\n");return 2;}Bench bench;bench.direct=!strcmp(argv[1],"shared")||!strcmp(argv[1],"shared-local");bench.systemMemory=strcmp(argv[1],"shared-local");if(!bench.direct&&strcmp(argv[1],"two-copy"))return 2;unsigned count=argc>3?std::stoul(argv[3]):180;if(!count||count>1800)return 2;if(const char*modifier=getenv("WMR_BENCH_MODIFIER")){char*end;bench.drmModifier=strtoull(modifier,&end,0);if(!*modifier||*end)throw std::runtime_error("Invalid modifier");}bench.reverseAllocation=getenv("WMR_BENCH_REVERSE")!=nullptr;bench.init();FILE*f=fopen(argv[2],"w");if(!f)throw std::runtime_error("CSV open failed");fprintf(f,"frame,verification,producer_render_ms,producer_transfer_ms,producer_total_ms,consumer_ms,host_ms\n");for(unsigned i=0;i<count;i++)bench.frame(i,f);fclose(f);puts("SHARED_IMAGE_BENCH_PASS");return 0;}catch(const std::exception&e){fprintf(stderr,"BENCH_ERROR %s\n",e.what());return 1;}}
