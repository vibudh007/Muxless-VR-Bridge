#pragma once
#include "acquire_state.hpp"
#include "kms_presenter.hpp"
// Alpha: a private Vulkan display device plus dGPU proxy swapchains.
// Render/composition stays in the calling runtime; only completed images cross devices.
// Diagnostic state: identify the precise wait behind compositor watchdogs.
static std::atomic<const char*> bridgeOperation{"idle"};
static std::atomic<int64_t> bridgeOperationStart{0};
static int64_t bridgeNow(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct BridgeOperation {
 const char* previous;int64_t previousStart;
 BridgeOperation(const char*name):previous(bridgeOperation.load()),previousStart(bridgeOperationStart.load()){bridgeOperationStart.store(bridgeNow());bridgeOperation.store(name);}
 ~BridgeOperation(){bridgeOperationStart.store(previousStart);bridgeOperation.store(previous);}
};
static void startBridgeMonitor(){if(!getenv("WMR_BRIDGE_DIAGNOSTICS")&&!bridgeConfigBool("diagnostics"))return;static std::once_flag once;std::call_once(once,[]{std::thread([]{for(;;){std::this_thread::sleep_for(std::chrono::milliseconds(250));auto name=bridgeOperation.load();auto elapsed=bridgeNow()-bridgeOperationStart.load();if(strcmp(name,"idle")&&elapsed>250000000)log("BRIDGE_STALL operation=%s elapsed_ms=%.1f",name,elapsed/1e6);}}).detach();});}
struct ProxySwapchain;
struct Bridge {
 VkInstance instance=VK_NULL_HANDLE;GPU output;VkPhysicalDevice publicPhysical=VK_NULL_HANDLE;VkDisplayKHR display=VK_NULL_HANDLE;VkSurfaceKHR surface=VK_NULL_HANDLE;int fd=-1;uint32_t connector=0;double refresh=90;bool timing=false,modifierImages=false;
 std::unordered_map<VkSwapchainKHR,std::unique_ptr<ProxySwapchain>>chains;
 std::set<VkImage>proxyImages;std::set<VkFence>outputFences;std::mutex fencesMutex;
 std::recursive_mutex mutex;
 void init(VkPhysicalDevice renderPhysical,int drmFd){ WMR_TIMING_SCOPE("Bridge::init");
  publicPhysical=renderPhysical;startBridgeMonitor();
  const char*exts[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_DISPLAY_EXTENSION_NAME,VK_EXT_DIRECT_MODE_DISPLAY_EXTENSION_NAME,VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME,VK_EXT_DISPLAY_SURFACE_COUNTER_EXTENSION_NAME};
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="WMR private presenter";app.apiVersion=VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;ci.enabledExtensionCount=5;ci.ppEnabledExtensionNames=exts;{BridgePrivateInstanceScope privateInstance;VK(vkCreateInstance(&ci,nullptr,&instance));}
  uint32_t n=0;VK(vkEnumeratePhysicalDevices(instance,&n,nullptr));std::vector<VkPhysicalDevice>ps(n);VK(vkEnumeratePhysicalDevices(instance,&n,ps.data()));struct stat st{};fstat(drmFd,&st);
  for(auto p:ps){GPU g;g.describe(p);if(g.drm.hasPrimary&&g.drm.primaryMajor==major(st.st_rdev)&&g.drm.primaryMinor==minor(st.st_rdev)){output.describe(p);break;}}
  if(!output.physical)throw std::runtime_error("Cannot find display-owner Vulkan GPU");
  log("BRIDGE private presenter=%s public_render=%p DRM=%u:%u",output.props.deviceName,(void*)publicPhysical,major(st.st_rdev),minor(st.st_rdev));
 }
 void device(){ WMR_TIMING_SCOPE("Bridge::device");
  if(output.device)return;
  if(output.props.apiVersion<VK_API_VERSION_1_1)throw std::runtime_error("Output GPU requires Vulkan 1.1 or newer");
  uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(output.physical,&n,nullptr);std::vector<VkQueueFamilyProperties>qs(n);vkGetPhysicalDeviceQueueFamilyProperties(output.physical,&n,qs.data());bool found=false;
  for(uint32_t i=0;i<n;i++){VkBool32 support=false;VK(vkGetPhysicalDeviceSurfaceSupportKHR(output.physical,i,surface,&support));if(support&&(qs[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)){output.family=i;found=true;break;}}
  if(!found)throw std::runtime_error("No output queue supports leased surface");
  uint32_t extensionCount=0;VK(vkEnumerateDeviceExtensionProperties(output.physical,nullptr,&extensionCount,nullptr));
  std::vector<VkExtensionProperties> extensions(extensionCount);VK(vkEnumerateDeviceExtensionProperties(output.physical,nullptr,&extensionCount,extensions.data()));
  std::set<std::string> availableExtensions;for(uint32_t i=0;i<extensionCount;i++)availableExtensions.insert(extensions[i].extensionName);
  const char*exts[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME,VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,VK_EXT_DISPLAY_CONTROL_EXTENSION_NAME,VK_KHR_PRESENT_ID_EXTENSION_NAME,VK_KHR_PRESENT_WAIT_EXTENSION_NAME};
  for(unsigned i=0;i<5;i++)if(!availableExtensions.count(exts[i]))throw std::runtime_error(std::string("Output GPU missing required extension: ")+exts[i]);
  VkPhysicalDevicePresentIdFeaturesKHR id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR};VkPhysicalDevicePresentWaitFeaturesKHR wait{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
  if(availableExtensions.count(exts[5])&&availableExtensions.count(exts[6])){id.pNext=&wait;VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&id;vkGetPhysicalDeviceFeatures2(output.physical,&features);timing=id.presentId&&wait.presentWait;}

  std::vector<const char*>enabledExtensions(exts,exts+5);if(timing){enabledExtensions.push_back(exts[5]);enabledExtensions.push_back(exts[6]);}
  modifierImages=bridgeConfigBool("direct_scanout")&&availableExtensions.count(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME)&&availableExtensions.count(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
  if(modifierImages){enabledExtensions.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);enabledExtensions.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);}
  float priority=1;VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};q.queueFamilyIndex=output.family;q.queueCount=1;q.pQueuePriorities=&priority;VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};ci.queueCreateInfoCount=1;ci.pQueueCreateInfos=&q;ci.enabledExtensionCount=enabledExtensions.size();ci.ppEnabledExtensionNames=enabledExtensions.data();ci.pNext=timing?&id:nullptr;VK(vkCreateDevice(output.physical,&ci,nullptr,&output.device));vkGetDeviceQueue(output.device,output.family,0,&output.queue);VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool.queueFamilyIndex=output.family;VK(vkCreateCommandPool(output.device,&pool,nullptr,&output.pool));log("BRIDGE output_device=%p timing=%d",(void*)output.device,timing);
 }
};
static Bridge*bridge=nullptr;
static bool enabled(){const char*v=getenv("WMR_BRIDGE_MODE");return v&&!strcmp(v,"alpha");}
static PFN_vkVoidFunction bridgeHook(const char*name);
static bool bridgedSurface(VkSurfaceKHR s){return bridge&&bridge->surface==s;}
static VkResult failure(const char*name,const std::exception&e){log("BRIDGE_ERROR %s: %s",name,e.what());bridgeHealth("error",0,VK_ERROR_INITIALIZATION_FAILED);return VK_ERROR_INITIALIZATION_FAILED;}
struct ProxySwapchain {
 GPU render;Bridge*b;VkSwapchainKHR real=VK_NULL_HANDLE;VkSwapchainKHR handle=VK_NULL_HANDLE;VkFormat format;VkExtent2D extent;
 std::vector<VkImage>images,realImages;std::vector<VkDeviceMemory>memory;Buffer transfer,imported;
 VkCommandBuffer sourceCB=VK_NULL_HANDLE,outputCB=VK_NULL_HANDLE;VkSemaphore produced=VK_NULL_HANDLE,received=VK_NULL_HANDLE;std::vector<VkSemaphore>ready;
 // Reused across frames; external-memory ownership and GPU waits are unchanged.
 std::vector<VkPipelineStageFlags> waitStages;
 PFN_vkGetSemaphoreFdKHR exportSemaphoreFd=nullptr;
 PFN_vkImportSemaphoreFdKHR importSemaphoreFd=nullptr;
 AcquireState<> acquireState;
 VkFence available=VK_NULL_HANDLE,done=VK_NULL_HANDLE,sourceDone=VK_NULL_HANDLE;uint64_t frames=0;
 bool queriesAttempted=false;VkQueryPool sourceQueries=VK_NULL_HANDLE,outputQueries=VK_NULL_HANDLE;uint32_t sourceTimestampBits=0,outputTimestampBits=0;
 void initQueries(){ WMR_TIMING_SCOPE("ProxySwapchain::initQueries");
  if(!perf||queriesAttempted)return;
  queriesAttempted=true;
  auto make=[&](GPU&gpu,VkQueryPool&pool,uint32_t&bits){auto properties=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)vkGetInstanceProcAddr(b->instance,"vkGetPhysicalDeviceQueueFamilyProperties");if(gpu.device==render.device){auto state=dstate(render.device);properties=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)state->instance->gipa(state->instance->handle,"vkGetPhysicalDeviceQueueFamilyProperties");}uint32_t count=0;properties(gpu.physical,&count,nullptr);std::vector<VkQueueFamilyProperties>qs(count);properties(gpu.physical,&count,qs.data());bits=qs.at(gpu.family).timestampValidBits;if(!bits)return;VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=2;VkResult r=vkCreateQueryPool(gpu.device,&info,nullptr,&pool);if(r!=VK_SUCCESS){pool=VK_NULL_HANDLE;log("PERF_QUERY unavailable result=%d",r);}};
  make(render,sourceQueries,sourceTimestampBits);make(b->output,outputQueries,outputTimestampBits);
 }
 double querySpan(GPU&gpu,VkQueryPool pool,uint32_t bits){ WMR_TIMING_SCOPE("ProxySwapchain::querySpan");if(!pool)return -1;uint64_t ticks[2]{};VkResult r=vkGetQueryPoolResults(gpu.device,pool,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);if(r!=VK_SUCCESS)return -1;uint64_t mask=bits>=64?~uint64_t(0):((uint64_t(1)<<bits)-1);return ((ticks[1]-ticks[0])&mask)*double(gpu.props.limits.timestampPeriod)/1e6;}
 bool perf=bridgeConfigBool("performance_logging");FILE*perfFile=nullptr;int64_t acquireStart=0,acquireEnd=0,previousPerfStart=0;
 void performance(uint32_t index,VkResult result,int64_t start,int64_t submitted,int64_t presented,int64_t copied,int64_t idle){ WMR_TIMING_SCOPE("ProxySwapchain::performance");
  if(!perf)return;
  if(!perfFile&&!bridgeRunDirectory.empty()){
   auto path=bridgeRunDirectory/("performance-"+std::to_string(getpid())+"-"+std::to_string((uintptr_t)this)+".csv");
   perfFile=fopen(path.c_str(),"w");if(perfFile){setvbuf(perfFile,nullptr,_IOFBF,65536);fprintf(perfFile,"frame,index,result,present_start_monotonic_ns,acquire_ms,prepare_submit_ms,output_present_ms,copy_fence_wait_ms,source_fence_wait_ms,total_present_ms,start_interval_ms,source_gpu_transfer_span_ms,output_gpu_transfer_span_ms\n");}
  }
  if(perfFile&&ftell(perfFile)>=4*1024*1024){
   fclose(perfFile);auto path=bridgeRunDirectory/("performance-"+std::to_string(getpid())+"-"+std::to_string((uintptr_t)this)+".csv");std::error_code ec;std::filesystem::rename(path,path.string()+".previous",ec);perfFile=fopen(path.c_str(),"w");if(perfFile){setvbuf(perfFile,nullptr,_IOFBF,65536);fprintf(perfFile,"frame,index,result,present_start_monotonic_ns,acquire_ms,prepare_submit_ms,output_present_ms,copy_fence_wait_ms,source_fence_wait_ms,total_present_ms,start_interval_ms,source_gpu_transfer_span_ms,output_gpu_transfer_span_ms\n");}
  }
  if(perfFile){fprintf(perfFile,"%llu,%u,%d,%lld,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",(unsigned long long)frames,index,result,(long long)start,(acquireEnd-acquireStart)/1e6,(submitted-start)/1e6,(presented-submitted)/1e6,(copied-presented)/1e6,(idle-copied)/1e6,(idle-start)/1e6,previousPerfStart?(start-previousPerfStart)/1e6:0.0,querySpan(render,sourceQueries,sourceTimestampBits),querySpan(b->output,outputQueries,outputTimestampBits));if(frames%90==0)fflush(perfFile);}previousPerfStart=start;
 }
 #include "direct_scanout.hpp"
 ProxySwapchain(Bridge*owner):b(owner){}
 void init(VkDevice dev,const VkSwapchainCreateInfoKHR*ci){ WMR_TIMING_SCOPE("ProxySwapchain::init");
  if(ci->imageArrayLayers!=1)throw std::runtime_error("Alpha expects one-layer headset swapchain");
  if(ci->imageFormat!=VK_FORMAT_R8G8B8A8_UNORM&&ci->imageFormat!=VK_FORMAT_B8G8R8A8_UNORM&&ci->imageFormat!=VK_FORMAT_R8G8B8A8_SRGB&&ci->imageFormat!=VK_FORMAT_B8G8R8A8_SRGB)throw std::runtime_error("Alpha expects RGBA8/BGRA8");
  auto d=dstate(dev);if(!d||!d->externalTransfer)throw std::runtime_error("Render device lacks enabled DMA-BUF or SYNC_FD support");render.physical=d->physical;render.device=dev;render.ownsDevice=false;
  auto props=(PFN_vkGetPhysicalDeviceProperties)d->instance->gipa(d->instance->handle,"vkGetPhysicalDeviceProperties");props(render.physical,&render.props);
  if(render.props.apiVersion<VK_API_VERSION_1_1)throw std::runtime_error("Render GPU requires Vulkan 1.1 or newer");
  auto mem=(PFN_vkGetPhysicalDeviceMemoryProperties)d->instance->gipa(d->instance->handle,"vkGetPhysicalDeviceMemoryProperties");mem(render.physical,&render.memory);
  auto queueProps=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)d->instance->gipa(d->instance->handle,"vkGetPhysicalDeviceQueueFamilyProperties");uint32_t n=0;queueProps(render.physical,&n,nullptr);std::vector<VkQueueFamilyProperties>qs(n);queueProps(render.physical,&n,qs.data());
  // SteamVR's graphics queue is discovered in its actual device-create request below.
  render.family=0;for(uint32_t i=0;i<n;i++)if(qs[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){render.family=i;break;}
  vkGetDeviceQueue(dev,render.family,0,&render.queue);
  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool.queueFamilyIndex=render.family;VK(vkCreateCommandPool(dev,&pool,nullptr,&render.pool));
  b->device();format=ci->imageFormat;extent=ci->imageExtent;
  if(bridgeConfigBool("direct_scanout")){try{initDirect(ci);return;}catch(const std::exception&e){log("DIRECT_SCANOUT fallback reason=%s",e.what());clearDirect();}}

  VkSwapchainCreateInfoKHR info=*ci;info.pNext=nullptr;info.oldSwapchain=VK_NULL_HANDLE;
  if(ci->oldSwapchain){auto previous=b->chains.find(ci->oldSwapchain);if(previous==b->chains.end()||previous->second->render.device!=dev)throw std::runtime_error("Old swapchain does not belong to this bridged device");info.oldSwapchain=previous->second->real;log("BRIDGE_RECREATE old_proxy=%p old_output=%p",(void*)ci->oldSwapchain,(void*)info.oldSwapchain);}
  info.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;info.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;info.queueFamilyIndexCount=0;info.pQueueFamilyIndices=nullptr;info.flags=0;
  VkSwapchainCounterCreateInfoEXT counters{VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT};for(auto*base=(const VkBaseInStructure*)ci->pNext;base;base=base->pNext)if(base->sType==VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT){counters.surfaceCounters=((const VkSwapchainCounterCreateInfoEXT*)base)->surfaceCounters;info.pNext=&counters;}
  VK(vkCreateSwapchainKHR(b->output.device,&info,nullptr,&real));
  VK(vkGetSwapchainImagesKHR(b->output.device,real,&n,nullptr));realImages.resize(n);VK(vkGetSwapchainImagesKHR(b->output.device,real,&n,realImages.data()));
  images.reserve(n);
  memory.reserve(n);
  for(uint32_t i=0;i<n;i++){
   VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};im.imageType=VK_IMAGE_TYPE_2D;im.format=format;im.extent={extent.width,extent.height,1};im.mipLevels=1;im.arrayLayers=ci->imageArrayLayers;im.samples=VK_SAMPLE_COUNT_1_BIT;im.tiling=VK_IMAGE_TILING_OPTIMAL;im.usage=ci->imageUsage|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;im.sharingMode=ci->imageSharingMode;im.queueFamilyIndexCount=ci->queueFamilyIndexCount;im.pQueueFamilyIndices=ci->pQueueFamilyIndices;if(ci->flags&VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)im.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
   VkImage image;VK(vkCreateImage(dev,&im,nullptr,&image));images.push_back(image);VkMemoryRequirements req;vkGetImageMemoryRequirements(dev,image,&req);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=render.type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);VkDeviceMemory mem;VK(vkAllocateMemory(dev,&ai,nullptr,&mem));memory.push_back(mem);VK(vkBindImageMemory(dev,image,mem,0));b->proxyImages.insert(image);
  }

  VkDeviceSize bytes=VkDeviceSize(extent.width)*extent.height*4;transfer.make(render,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);int fd=transfer.exportFd();imported.make(b->output,bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,fd,transfer.allocation);
  exportSemaphoreFd=(PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(render.device,"vkGetSemaphoreFdKHR");
  importSemaphoreFd=(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(b->output.device,"vkImportSemaphoreFdKHR");
  if(!exportSemaphoreFd||!importSemaphoreFd)throw std::runtime_error("External semaphore entry points unavailable");
  sourceCB=render.commands();outputCB=b->output.commands();VkExportSemaphoreCreateInfo ex{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};ex.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};si.pNext=&ex;VK(vkCreateSemaphore(dev,&si,nullptr,&produced));si.pNext=nullptr;VK(vkCreateSemaphore(b->output.device,&si,nullptr,&received));ready.resize(n);for(auto&s:ready)VK(vkCreateSemaphore(b->output.device,&si,nullptr,&s));VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VK(vkCreateFence(b->output.device,&fi,nullptr,&available));VK(vkCreateFence(b->output.device,&fi,nullptr,&done));VK(vkCreateFence(render.device,&fi,nullptr,&sourceDone));
  log("BRIDGE_SWAPCHAIN present_mode=%d requested_images=%u",ci->presentMode,ci->minImageCount);
  log("BRIDGE_SWAPCHAIN public_device=%p real=%p size=%ux%u format=%d images=%u source_usage=0x%x",(void*)dev,(void*)real,extent.width,extent.height,format,n,ci->imageUsage);
 }
 VkResult acquire(uint64_t timeout,VkSemaphore sem,VkFence fence,uint32_t*index){ WMR_TIMING_SCOPE("ProxySwapchain::acquire");
  if(kms)return acquireDirect(timeout,sem,fence,index);
  acquireStart=perf?bridgeNow():0;BridgeOperation operation("acquire");
  VkResult r=acquireState.next(timeout,index,
   [&](uint64_t budget,uint32_t*out){return vkAcquireNextImageKHR(b->output.device,real,budget,VK_NULL_HANDLE,available,out);},
   [&](uint64_t budget){return vkWaitForFences(b->output.device,1,&available,VK_TRUE,budget);},
   [&](){return vkResetFences(b->output.device,1,&available);});
  if(r!=VK_SUCCESS&&r!=VK_SUBOPTIMAL_KHR)return r;
  acquireEnd=perf?bridgeNow():0;
  if(sem||fence){VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.signalSemaphoreCount=sem?1:0;s.pSignalSemaphores=sem?&sem:nullptr;VK(vkQueueSubmit(render.queue,1,&s,fence));}
  if(frames<4)log("BRIDGE_ACQUIRE index=%u",*index);
  return r;
 }
 VkResult present(VkQueue queue,const VkPresentInfoKHR*pi,uint32_t slot){ WMR_TIMING_SCOPE("ProxySwapchain::present");
  if(kms)return presentDirect(queue,pi,slot);
  int64_t perfStart=perf?bridgeNow():0;BridgeOperation presentOperation("present-prepare");uint32_t index=pi->pImageIndices[slot];if(index>=images.size())return VK_ERROR_INITIALIZATION_FAILED;
  if(queue!=render.queue){
   auto d=dstate(queue);if(!d||d->handle!=render.device)throw std::runtime_error("Presentation queue belongs to a different logical device");uint32_t queueFamily;{std::lock_guard<std::mutex>lock(d->queueFamiliesMutex);auto qf=d->queueFamilies.find(queue);if(qf==d->queueFamilies.end())throw std::runtime_error("Presentation queue family not observed");queueFamily=qf->second;}
   if(queueFamily!=render.family){if(frames)throw std::runtime_error("Queue family changed after presentation began");vkDestroyCommandPool(render.device,render.pool,nullptr);render.family=queueFamily;VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=render.family;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;VK(vkCreateCommandPool(render.device,&pi,nullptr,&render.pool));sourceCB=render.commands();}
   log("BRIDGE_QUEUE adopt=%p previous=%p family=%u",(void*)queue,(void*)render.queue,render.family);render.queue=queue;
  }
  if(perf&&!sourceQueries&&!outputQueries)initQueries();
  begin(sourceCB);if(sourceQueries)vkCmdResetQueryPool(sourceCB,sourceQueries,0,2);if(frames)bufferBarrier(sourceCB,transfer.buffer,transfer.size,VK_QUEUE_FAMILY_EXTERNAL,render.family,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  imageBarrier(sourceCB,images[index],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
  if(sourceQueries)vkCmdWriteTimestamp(sourceCB,VK_PIPELINE_STAGE_TRANSFER_BIT,sourceQueries,0);
  VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};vkCmdCopyImageToBuffer(sourceCB,images[index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,transfer.buffer,1,&copy);
  imageBarrier(sourceCB,images[index],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  bufferBarrier(sourceCB,transfer.buffer,transfer.size,render.family,VK_QUEUE_FAMILY_EXTERNAL,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);if(sourceQueries)vkCmdWriteTimestamp(sourceCB,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,sourceQueries,1);VK(vkEndCommandBuffer(sourceCB));
  waitStages.resize(pi->waitSemaphoreCount,VK_PIPELINE_STAGE_TRANSFER_BIT);VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=pi->waitSemaphoreCount;submit.pWaitSemaphores=pi->pWaitSemaphores;submit.pWaitDstStageMask=waitStages.data();submit.commandBufferCount=1;submit.pCommandBuffers=&sourceCB;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&produced;VK(vkQueueSubmit(queue,1,&submit,sourceDone));
  VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};get.semaphore=produced;get.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;int fd=-1;VK(exportSemaphoreFd(render.device,&get,&fd));OwnedFd semaphoreFd(fd);VkImportSemaphoreFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};imp.semaphore=received;imp.flags=VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;imp.handleType=get.handleType;imp.fd=fd;VK(importSemaphoreFd(b->output.device,&imp));semaphoreFd.release();
  begin(outputCB);if(outputQueries)vkCmdResetQueryPool(outputCB,outputQueries,0,2);bufferBarrier(outputCB,imported.buffer,imported.size,VK_QUEUE_FAMILY_EXTERNAL,b->output.family,0,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);imageBarrier(outputCB,realImages[index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);if(outputQueries)vkCmdWriteTimestamp(outputCB,VK_PIPELINE_STAGE_TRANSFER_BIT,outputQueries,0);vkCmdCopyBufferToImage(outputCB,imported.buffer,realImages[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);imageBarrier(outputCB,realImages[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);bufferBarrier(outputCB,imported.buffer,imported.size,b->output.family,VK_QUEUE_FAMILY_EXTERNAL,VK_ACCESS_TRANSFER_READ_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);if(outputQueries)vkCmdWriteTimestamp(outputCB,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,outputQueries,1);VK(vkEndCommandBuffer(outputCB));
  VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;submit={VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&received;submit.pWaitDstStageMask=&stage;submit.commandBufferCount=1;submit.pCommandBuffers=&outputCB;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&ready[index];VK(vkQueueSubmit(b->output.queue,1,&submit,done));
  VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};presentInfo.waitSemaphoreCount=1;presentInfo.pWaitSemaphores=&ready[index];presentInfo.swapchainCount=1;presentInfo.pSwapchains=&real;presentInfo.pImageIndices=&index;
  VkPresentIdKHR id{VK_STRUCTURE_TYPE_PRESENT_ID_KHR};uint64_t idValue=0;for(auto*base=(const VkBaseInStructure*)pi->pNext;base;base=base->pNext){if(base->sType==VK_STRUCTURE_TYPE_PRESENT_ID_KHR){idValue=((const VkPresentIdKHR*)base)->pPresentIds[slot];id.swapchainCount=1;id.pPresentIds=&idValue;if(b->timing)presentInfo.pNext=&id;}else if(frames<2)log("BRIDGE_PRESENT unhandled_pnext=%d",base->sType);}
  int64_t perfSubmitted=perf?bridgeNow():0;VkResult result;{BridgeOperation operation("output-present");result=WMR_TIMING_CALL("Vulkan::presentOutput",vkQueuePresentKHR(b->output.queue,&presentInfo));}
  int64_t perfPresented=perf?bridgeNow():0;{BridgeOperation operation("output-copy-fence");VK(WMR_TIMING_CALL("Vulkan::waitOutputCopy",vkWaitForFences(b->output.device,1,&done,VK_TRUE,5000000000ull)));}VK(vkResetFences(b->output.device,1,&done));int64_t perfCopied=perf?bridgeNow():0;{BridgeOperation operation("source-copy-fence");VK(WMR_TIMING_CALL("Vulkan::waitSourceCopy",vkWaitForFences(render.device,1,&sourceDone,VK_TRUE,5000000000ull)));}VK(vkResetFences(render.device,1,&sourceDone));
  frames++;performance(index,result,perfStart,perfSubmitted,perfPresented,perfCopied,perf?bridgeNow():0);if(frames<=5||frames%300==0)bridgeHealth(result>=0?"presenting":"error",frames,result);if(frames<=5||frames%300==0)log("BRIDGE_PRESENT frames=%llu index=%u result=%d present_id=%llu",(unsigned long long)frames,index,result,(unsigned long long)idValue);return result;
 }
 ~ProxySwapchain(){ WMR_TIMING_SCOPE("ProxySwapchain::~ProxySwapchain");
  if(perfFile)fclose(perfFile);
  if(directLog)fclose(directLog);
  // An internal acquire may outlive a public timeout. Retire its WSI fence
  // before destruction, just as submitted GPU work is retired below.
  if(acquireState.pending()&&b->output.device&&available)vkWaitForFences(b->output.device,1,&available,VK_TRUE,UINT64_MAX);
  if(render.device)vkDeviceWaitIdle(render.device);
  if(b->output.device)vkDeviceWaitIdle(b->output.device);
  if(kms)clearDirect();
  if(sourceQueries)vkDestroyQueryPool(render.device,sourceQueries,nullptr);
  if(outputQueries)vkDestroyQueryPool(b->output.device,outputQueries,nullptr);
  if(real)vkDestroySwapchainKHR(b->output.device,real,nullptr);
  for(auto s:ready)vkDestroySemaphore(b->output.device,s,nullptr);
  if(received)vkDestroySemaphore(b->output.device,received,nullptr);
  if(available)vkDestroyFence(b->output.device,available,nullptr);
  if(done)vkDestroyFence(b->output.device,done,nullptr);
  if(produced)vkDestroySemaphore(render.device,produced,nullptr);
  if(sourceDone)vkDestroyFence(render.device,sourceDone,nullptr);
  for(auto image:images){b->proxyImages.erase(image);vkDestroyImage(render.device,image,nullptr);}for(auto mem:memory)vkFreeMemory(render.device,mem,nullptr);
  // Buffer member destructors need the public device alive; GPU must not own it.
  if(render.pool){vkDestroyCommandPool(render.device,render.pool,nullptr);render.pool=VK_NULL_HANDLE;}
 }
};
// Proxy GPU wraps an application-owned device; suppress its ownership in its destructor via cleanup below.
static ProxySwapchain*findChain(VkSwapchainKHR c){ WMR_TIMING_SCOPE("findChain");if(!bridge)return nullptr;auto it=bridge->chains.find(c);return it==bridge->chains.end()?nullptr:it->second.get();}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeGetDrm(VkPhysicalDevice p,int fd,uint32_t id,VkDisplayKHR*out){ WMR_TIMING_SCOPE("bridgeGetDrm");
 auto s=istate(p);auto next=(PFN_vkGetDrmDisplayEXT)s->gipa(s->handle,"vkGetDrmDisplayEXT");if(!enabled())return next(p,fd,id,out);
 VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};props.pNext=&drm;auto getProps=(PFN_vkGetPhysicalDeviceProperties2)s->gipa(s->handle,"vkGetPhysicalDeviceProperties2");getProps(p,&props);struct stat st{};fstat(fd,&st);if(drm.primaryMajor==major(st.st_rdev)&&drm.primaryMinor==minor(st.st_rdev))return next(p,fd,id,out);
 try{if(!bridge){bridge=new Bridge;bridge->init(p,fd);}bridge->connector=id;auto fn=(PFN_vkGetDrmDisplayEXT)vkGetInstanceProcAddr(bridge->instance,"vkGetDrmDisplayEXT");VkResult r=fn(bridge->output.physical,fd,id,&bridge->display);*out=bridge->display;log("BRIDGE_DRM connector=%u render=%s presenter=%s result=%d",id,props.properties.deviceName,bridge->output.props.deviceName,r);return r;}catch(const std::exception&e){return failure("GetDrmDisplay",e);}
}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeAcquireDrm(VkPhysicalDevice p,int fd,VkDisplayKHR d){ WMR_TIMING_SCOPE("bridgeAcquireDrm");if(bridge&&d==bridge->display){if(bridge->fd>=0)close(bridge->fd);bridge->fd=dup(fd);auto fn=(PFN_vkAcquireDrmDisplayEXT)vkGetInstanceProcAddr(bridge->instance,"vkAcquireDrmDisplayEXT");VkResult r=fn(bridge->output.physical,bridge->fd,d);log("BRIDGE_ACQUIRE_DISPLAY result=%d",r);return r;}auto s=istate(p);return ((PFN_vkAcquireDrmDisplayEXT)s->gipa(s->handle,"vkAcquireDrmDisplayEXT"))(p,fd,d);}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeSurface(VkInstance i,const VkDisplaySurfaceCreateInfoKHR*ci,const VkAllocationCallbacks*a,VkSurfaceKHR*out){ WMR_TIMING_SCOPE("bridgeSurface");if(bridge){uint32_t count=0;vkGetDisplayModePropertiesKHR(bridge->output.physical,bridge->display,&count,nullptr);std::vector<VkDisplayModePropertiesKHR>modes(count);vkGetDisplayModePropertiesKHR(bridge->output.physical,bridge->display,&count,modes.data());for(auto&m:modes)if(m.displayMode==ci->displayMode)bridge->refresh=m.parameters.refreshRate/1000.0;VkResult r=vkCreateDisplayPlaneSurfaceKHR(bridge->instance,ci,a,out);if(r==VK_SUCCESS)bridge->surface=*out;log("BRIDGE_SURFACE extent=%ux%u result=%d",ci->imageExtent.width,ci->imageExtent.height,r);return r;}auto s=istate(i);return ((PFN_vkCreateDisplayPlaneSurfaceKHR)s->gipa(i,"vkCreateDisplayPlaneSurfaceKHR"))(i,ci,a,out);}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeCreateSwapchain(VkDevice d,const VkSwapchainCreateInfoKHR*ci,const VkAllocationCallbacks*a,VkSwapchainKHR*out){ WMR_TIMING_SCOPE("bridgeCreateSwapchain");if(!bridgedSurface(ci->surface)){auto s=dstate(d);return ((PFN_vkCreateSwapchainKHR)s->gdpa(d,"vkCreateSwapchainKHR"))(d,ci,a,out);}try{std::lock_guard<std::recursive_mutex>lock(bridge->mutex);auto c=std::make_unique<ProxySwapchain>(bridge);c->init(d,ci);c->handle=(VkSwapchainKHR)c.get();*out=c->handle;bridge->chains[*out]=std::move(c);return VK_SUCCESS;}catch(const std::exception&e){return failure("CreateSwapchain",e);}}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeImages(VkDevice d,VkSwapchainKHR sc,uint32_t*n,VkImage*out){ WMR_TIMING_SCOPE("bridgeImages");if(auto*c=findChain(sc)){if(!out){*n=c->images.size();return VK_SUCCESS;}uint32_t count=std::min(*n,(uint32_t)c->images.size());std::copy_n(c->images.begin(),count,out);*n=count;return count<c->images.size()?VK_INCOMPLETE:VK_SUCCESS;}auto s=dstate(d);return ((PFN_vkGetSwapchainImagesKHR)s->gdpa(d,"vkGetSwapchainImagesKHR"))(d,sc,n,out);}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeAcquire(VkDevice d,VkSwapchainKHR sc,uint64_t timeout,VkSemaphore sem,VkFence fence,uint32_t*index){ WMR_TIMING_SCOPE("bridgeAcquire");if(auto*c=findChain(sc)){try{std::lock_guard<std::recursive_mutex>lock(bridge->mutex);return c->acquire(timeout,sem,fence,index);}catch(const std::exception&e){return failure("Acquire",e);}}auto s=dstate(d);return ((PFN_vkAcquireNextImageKHR)s->gdpa(d,"vkAcquireNextImageKHR"))(d,sc,timeout,sem,fence,index);}
static VKAPI_ATTR VkResult VKAPI_CALL bridgePresent(VkQueue q,const VkPresentInfoKHR*pi){ WMR_TIMING_SCOPE("bridgePresent");bool ours=false;for(uint32_t i=0;i<pi->swapchainCount;i++)ours|=findChain(pi->pSwapchains[i])!=nullptr;if(!ours){auto s=dstate(q);return ((PFN_vkQueuePresentKHR)s->gdpa(s->handle,"vkQueuePresentKHR"))(q,pi);}if(pi->swapchainCount!=1){log("BRIDGE_ERROR mixed/multiple presentation unsupported in alpha");return VK_ERROR_INITIALIZATION_FAILED;}try{std::lock_guard<std::recursive_mutex>lock(bridge->mutex);VkResult r=findChain(pi->pSwapchains[0])->present(q,pi,0);if(pi->pResults)pi->pResults[0]=r;return r;}catch(const std::exception&e){return failure("Present",e);}}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeWaitPresent(VkDevice d,VkSwapchainKHR sc,uint64_t id,uint64_t timeout){ WMR_TIMING_SCOPE("bridgeWaitPresent");if(auto*c=findChain(sc)){if(c->kms)return c->kms->waitPresent(id,timeout);if(!bridge->timing)return VK_ERROR_FEATURE_NOT_PRESENT;auto fn=(PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(bridge->output.device,"vkWaitForPresentKHR");return fn(bridge->output.device,c->real,id,timeout);}auto s=dstate(d);return ((PFN_vkWaitForPresentKHR)s->gdpa(d,"vkWaitForPresentKHR"))(d,sc,id,timeout);}
// Translate PRESENT layouts only for the dGPU proxy images, which are ordinary VkImages.
// Most barriers do not need rewriting. Do not allocate or copy them.
static VKAPI_ATTR void VKAPI_CALL bridgeBarrier(VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags dep,uint32_t mc,const VkMemoryBarrier*ms,uint32_t bc,const VkBufferMemoryBarrier*bs,uint32_t ic,const VkImageMemoryBarrier*is){ WMR_TIMING_SCOPE("bridgeBarrier");
 auto s=dstate(cb);auto fn=s->pipelineBarrier?s->pipelineBarrier:(PFN_vkCmdPipelineBarrier)s->gdpa(s->handle,"vkCmdPipelineBarrier");
 bool rewrite=false;
 if(bridge)for(uint32_t i=0;i<ic;i++)if((is[i].oldLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR||is[i].newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)&&bridge->proxyImages.count(is[i].image)){rewrite=true;break;}
 if(!rewrite){fn(cb,src,dst,dep,mc,ms,bc,bs,ic,is);return;}
 // Downstream dispatch does not re-enter this wrapper; scratch is per thread.
 thread_local std::vector<VkImageMemoryBarrier> scratch;
 scratch.assign(is,is+ic);
 for(auto&image:scratch)if(bridge->proxyImages.count(image.image)){
  if(image.oldLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)image.oldLayout=VK_IMAGE_LAYOUT_GENERAL;
  if(image.newLayout==VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)image.newLayout=VK_IMAGE_LAYOUT_GENERAL;
 }
 fn(cb,src,dst,dep,mc,ms,bc,bs,ic,scratch.data());
}
#include "bridge-generated.h"
static void bridgeDeviceCleanup(VkDevice d){if(!bridge)return;for(auto it=bridge->chains.begin();it!=bridge->chains.end();){if(it->second->render.device==d)it=bridge->chains.erase(it);else ++it;}}
static VKAPI_ATTR void VKAPI_CALL bridgeDestroySwapchain(VkDevice d,VkSwapchainKHR sc,const VkAllocationCallbacks*a){ WMR_TIMING_SCOPE("bridgeDestroySwapchain");if(findChain(sc)){bridge->chains.erase(sc);return;}auto s=dstate(d);((PFN_vkDestroySwapchainKHR)s->gdpa(d,"vkDestroySwapchainKHR"))(d,sc,a);}
static VKAPI_ATTR void VKAPI_CALL bridgeDestroySurface(VkInstance i,VkSurfaceKHR surf,const VkAllocationCallbacks*a){ WMR_TIMING_SCOPE("bridgeDestroySurface");if(bridgedSurface(surf)){vkDestroySurfaceKHR(bridge->instance,surf,a);bridge->surface=VK_NULL_HANDLE;return;}auto s=istate(i);((PFN_vkDestroySurfaceKHR)s->gipa(i,"vkDestroySurfaceKHR"))(i,surf,a);}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeAcquire2(VkDevice d,const VkAcquireNextImageInfoKHR*info,uint32_t*index){ WMR_TIMING_SCOPE("bridgeAcquire2");if(findChain(info->swapchain)){if(info->deviceMask!=1||info->pNext)return VK_ERROR_FEATURE_NOT_PRESENT;return bridgeAcquire(d,info->swapchain,info->timeout,info->semaphore,info->fence,index);}auto s=dstate(d);return ((PFN_vkAcquireNextImage2KHR)s->gdpa(d,"vkAcquireNextImage2KHR"))(d,info,index);}
static bool isOutputFence(VkFence fence){if(!bridge)return false;std::lock_guard<std::mutex>lock(bridge->fencesMutex);return bridge->outputFences.count(fence);}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeRegisterDisplay(VkDevice d,VkDisplayKHR display,const VkDisplayEventInfoEXT*info,const VkAllocationCallbacks*a,VkFence*out){ WMR_TIMING_SCOPE("bridgeRegisterDisplay");
 if(bridge&&display==bridge->display){for(auto&entry:bridge->chains)if(entry.second->kms)return VK_ERROR_FEATURE_NOT_PRESENT;try{bridge->device();auto fn=(PFN_vkRegisterDisplayEventEXT)vkGetDeviceProcAddr(bridge->output.device,"vkRegisterDisplayEventEXT");VkResult r=fn(bridge->output.device,display,info,a,out);if(r==VK_SUCCESS){std::lock_guard<std::mutex>lock(bridge->fencesMutex);bridge->outputFences.insert(*out);}static std::atomic<unsigned>calls{0};if(calls.fetch_add(1)<4)log("BRIDGE_DISPLAY_EVENT result=%d",r);return r;}catch(const std::exception&e){return failure("DisplayEvent",e);}}
 auto s=dstate(d);return ((PFN_vkRegisterDisplayEventEXT)s->gdpa(d,"vkRegisterDisplayEventEXT"))(d,display,info,a,out);
}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeCounter(VkDevice d,VkSwapchainKHR sc,VkSurfaceCounterFlagBitsEXT counter,uint64_t*out){ WMR_TIMING_SCOPE("bridgeCounter");if(auto*c=findChain(sc)){if(c->kms)return c->kms->counter(out);auto fn=(PFN_vkGetSwapchainCounterEXT)vkGetDeviceProcAddr(bridge->output.device,"vkGetSwapchainCounterEXT");VkResult r=fn(bridge->output.device,c->real,counter,out);static std::atomic<unsigned>calls{0};if(calls.fetch_add(1)<4)log("BRIDGE_COUNTER result=%d value=%llu",r,(unsigned long long)(r==VK_SUCCESS?*out:0));return r;}auto s=dstate(d);return ((PFN_vkGetSwapchainCounterEXT)s->gdpa(d,"vkGetSwapchainCounterEXT"))(d,sc,counter,out);}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeFenceStatus(VkDevice d,VkFence fence){ WMR_TIMING_SCOPE("bridgeFenceStatus");if(isOutputFence(fence))return vkGetFenceStatus(bridge->output.device,fence);auto s=dstate(d);return (s->fenceStatus?s->fenceStatus:(PFN_vkGetFenceStatus)s->gdpa(d,"vkGetFenceStatus"))(d,fence);}
static VKAPI_ATTR void VKAPI_CALL bridgeDestroyFence(VkDevice d,VkFence fence,const VkAllocationCallbacks*a){if(isOutputFence(fence)){vkDestroyFence(bridge->output.device,fence,a);std::lock_guard<std::mutex>lock(bridge->fencesMutex);bridge->outputFences.erase(fence);return;}auto s=dstate(d);((PFN_vkDestroyFence)s->gdpa(d,"vkDestroyFence"))(d,fence,a);}
enum class FenceRoute {Local,Remote,Mixed};
static FenceRoute classifyFences(uint32_t count,const VkFence*fences){
 if(!bridge)return FenceRoute::Local;
 std::lock_guard<std::mutex>lock(bridge->fencesMutex);bool local=false,remote=false;
 for(uint32_t i=0;i<count;i++){if(bridge->outputFences.count(fences[i]))remote=true;else local=true;if(local&&remote)return FenceRoute::Mixed;}
 return remote?FenceRoute::Remote:FenceRoute::Local;
}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeWaitFences(VkDevice d,uint32_t count,const VkFence*fences,VkBool32 all,uint64_t timeout){ WMR_TIMING_SCOPE("bridgeWaitFences");
 auto s=dstate(d);auto fn=s->waitFences?s->waitFences:(PFN_vkWaitForFences)s->gdpa(d,"vkWaitForFences");
 const auto route=classifyFences(count,fences);
 if(route==FenceRoute::Local)return fn(d,count,fences,all,timeout);
 if(route==FenceRoute::Remote)return vkWaitForFences(bridge->output.device,count,fences,all,timeout);
 auto start=std::chrono::steady_clock::now();do{bool every=true,any=false;for(uint32_t i=0;i<count;i++){auto r=bridgeFenceStatus(d,fences[i]);if(r!=VK_SUCCESS&&r!=VK_NOT_READY)return r;every&=r==VK_SUCCESS;any|=r==VK_SUCCESS;}if(all?every:any)return VK_SUCCESS;if(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count()>=(int64_t)std::min(timeout,uint64_t(INT64_MAX)))return VK_TIMEOUT;std::this_thread::sleep_for(std::chrono::microseconds(50));}while(true);
}
static VKAPI_ATTR VkResult VKAPI_CALL bridgeResetFences(VkDevice d,uint32_t count,const VkFence*fences){ WMR_TIMING_SCOPE("bridgeResetFences");
 auto s=dstate(d);auto fn=s->resetFences?s->resetFences:(PFN_vkResetFences)s->gdpa(d,"vkResetFences");
 const auto route=classifyFences(count,fences);
 if(route==FenceRoute::Local)return fn(d,count,fences);
 if(route==FenceRoute::Remote)return vkResetFences(bridge->output.device,count,fences);
 thread_local std::vector<VkFence>local,remote;local.clear();remote.clear();
 {std::lock_guard<std::mutex>lock(bridge->fencesMutex);for(uint32_t i=0;i<count;i++)(bridge->outputFences.count(fences[i])?remote:local).push_back(fences[i]);}
 auto result=fn(d,local.size(),local.data());if(result!=VK_SUCCESS)return result;
 return vkResetFences(bridge->output.device,remote.size(),remote.data());
}
#include "descriptor_diagnostic.hpp"
static void descriptorDeviceCleanup(VkDevice d){descriptorDiagnostic::cleanup(d);}
static PFN_vkVoidFunction bridgeHook(const char*n){if(!enabled())return nullptr;
 if(auto hook=descriptorDiagnostic::hook(n))return hook;
#define HOOK(name,fn) if(!strcmp(n,name))return (PFN_vkVoidFunction)fn
 HOOK("vkRegisterDisplayEventEXT",bridgeRegisterDisplay);HOOK("vkGetSwapchainCounterEXT",bridgeCounter);HOOK("vkWaitForFences",bridgeWaitFences);HOOK("vkGetFenceStatus",bridgeFenceStatus);HOOK("vkDestroyFence",bridgeDestroyFence);HOOK("vkResetFences",bridgeResetFences);HOOK("vkDestroySwapchainKHR",bridgeDestroySwapchain);HOOK("vkDestroySurfaceKHR",bridgeDestroySurface);HOOK("vkAcquireNextImage2KHR",bridgeAcquire2);HOOK("vkGetDrmDisplayEXT",bridgeGetDrm);HOOK("vkAcquireDrmDisplayEXT",bridgeAcquireDrm);HOOK("vkCreateDisplayPlaneSurfaceKHR",bridgeSurface);HOOK("vkCreateSwapchainKHR",bridgeCreateSwapchain);HOOK("vkGetSwapchainImagesKHR",bridgeImages);HOOK("vkAcquireNextImageKHR",bridgeAcquire);HOOK("vkQueuePresentKHR",bridgePresent);HOOK("vkWaitForPresentKHR",bridgeWaitPresent);HOOK("vkCmdPipelineBarrier",bridgeBarrier);
 auto generated=bridgeGeneratedHook(n);if(generated)return generated;
#undef HOOK
 return nullptr;
}
