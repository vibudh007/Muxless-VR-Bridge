#pragma once
// Methods are included inside ProxySwapchain after its ordinary members.
struct DirectFrame {
 GPU*gpu;VkCommandBuffer acquireCB=VK_NULL_HANDLE,releaseCB=VK_NULL_HANDLE;
 VkSemaphore produced=VK_NULL_HANDLE;VkFence done=VK_NULL_HANDLE;VkQueryPool queries=VK_NULL_HANDLE;bool used=false;
 bool reuseAcquireRecorded=false;
 explicit DirectFrame(GPU*g):gpu(g){}
 ~DirectFrame(){if(queries)vkDestroyQueryPool(gpu->device,queries,nullptr);if(done)vkDestroyFence(gpu->device,done,nullptr);if(produced)vkDestroySemaphore(gpu->device,produced,nullptr);if(acquireCB)vkFreeCommandBuffers(gpu->device,gpu->pool,1,&acquireCB);if(releaseCB)vkFreeCommandBuffers(gpu->device,gpu->pool,1,&releaseCB);}
};
std::unique_ptr<KmsPresenter>kms;
std::vector<std::unique_ptr<SharedLinearImage>>directImages;
std::vector<std::unique_ptr<DirectFrame>>directFrames;
FILE*directLog=nullptr;std::filesystem::path directLogPath;double directRetireMs=0;
void initDirect(const VkSwapchainCreateInfoKHR*ci){ WMR_TIMING_SCOPE("ProxySwapchain::initDirect");
 auto state=dstate(render.device);
 if(!state->modifierImages||!b->modifierImages)throw std::runtime_error("DRM image modifiers not enabled on both Vulkan devices");
 if(ci->flags||ci->imageSharingMode!=VK_SHARING_MODE_EXCLUSIVE||ci->minImageCount>4)throw std::runtime_error("Direct scanout does not support requested swapchain flags/sharing/image count");
 for(auto*p=(const VkBaseInStructure*)ci->pNext;p;p=p->pNext)if(p->sType!=VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT)throw std::runtime_error("Direct scanout swapchain pNext unsupported");
 kms=std::make_unique<KmsPresenter>(b->fd,b->connector,extent.width,extent.height,b->refresh,format);
 const uint32_t count=std::max(3u,ci->minImageCount);
 images.reserve(count);
 directImages.reserve(count);
 directFrames.reserve(count);
 auto query=(PFN_vkGetPhysicalDeviceImageFormatProperties2)state->instance->gipa(state->instance->handle,"vkGetPhysicalDeviceImageFormatProperties2");
 for(uint32_t i=0;i<count;i++){
  auto shared=std::make_unique<SharedLinearImage>();shared->make(render,b->output,format,extent,ci->imageUsage|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,query,true);
  kms->addImage(*shared);images.push_back(shared->image);b->proxyImages.insert(shared->image);directImages.push_back(std::move(shared));
 }
 kms->test();
 bool timestampSupported=false;
 if(perf){uint32_t n=0;auto props=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)state->instance->gipa(state->instance->handle,"vkGetPhysicalDeviceQueueFamilyProperties");props(render.physical,&n,nullptr);std::vector<VkQueueFamilyProperties>families(n);props(render.physical,&n,families.data());timestampSupported=render.family<n&&families[render.family].timestampValidBits==64;}
 for(uint32_t i=0;i<count;i++){
  auto frame=std::make_unique<DirectFrame>(&render);frame->acquireCB=render.commands();frame->releaseCB=render.commands();
  VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};exportInfo.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};sem.pNext=&exportInfo;VK(vkCreateSemaphore(render.device,&sem,nullptr,&frame->produced));VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};fence.flags=VK_FENCE_CREATE_SIGNALED_BIT;VK(vkCreateFence(render.device,&fence,nullptr,&frame->done));
  if(timestampSupported){VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};info.queryType=VK_QUERY_TYPE_TIMESTAMP;info.queryCount=2;VK(vkCreateQueryPool(render.device,&info,nullptr,&frame->queries));}
  recordDirectRelease(*frame, images[i]);
  directFrames.push_back(std::move(frame));
 }
 exportSemaphoreFd=(PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(render.device,"vkGetSemaphoreFdKHR");if(!exportSemaphoreFd)throw std::runtime_error("SYNC_FD export entry point missing");
 log("DIRECT_SCANOUT enabled images=%u size=%ux%u format=%d explicit_gpu_copies=0 memory=shared-system",count,extent.width,extent.height,format);
}
void clearDirect(){ WMR_TIMING_SCOPE("ProxySwapchain::clearDirect");kms.reset();directFrames.clear();for(auto image:images)b->proxyImages.erase(image);images.clear();directImages.clear();}
VkResult acquireDirect(uint64_t timeout,VkSemaphore sem,VkFence fence,uint32_t*index){ WMR_PACING_SCOPE("ProxySwapchain::acquireDirect");
 acquireStart=perf?bridgeNow():0;auto result=kms->acquire(timeout,index);if(result!=VK_SUCCESS)return result;
 auto&frame=*directFrames.at(*index);auto start=perf?bridgeNow():0;
 // A retired scanout image's source work has completed via its IN_FENCE and flip.
 // This explicit fence check also proves command-buffer/query reuse to Vulkan.
 VK(WMR_PACING_CALL("Vulkan::waitDirectRetirement",vkWaitForFences(render.device,1,&frame.done,VK_TRUE,UINT64_MAX)));
 directRetireMs=perf?(bridgeNow()-start)/1e6:0;
 if(frame.used&&frame.queries&&frames<10){uint64_t ticks[2]{};auto r=vkGetQueryPoolResults(render.device,frame.queries,0,2,sizeof(ticks),ticks,8,VK_QUERY_RESULT_64_BIT);if(r==VK_SUCCESS)log("DIRECT_RELEASE_GPU ms=%.6f",(ticks[1]-ticks[0])*double(render.props.limits.timestampPeriod)/1e6);}
 VK(WMR_PACING_CALL("Vulkan::resetDirectFence",vkResetFences(render.device,1,&frame.done)));
 if (!frame.reuseAcquireRecorded) {
  // The first use starts in UNDEFINED. After retirement the same external
  // acquire is valid on every reuse of this image in this queue family.
  begin(frame.acquireCB);
  if(frame.used)sharedOwnership(frame.acquireCB,images[*index],VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_EXTERNAL,render.family,0,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  else imageBarrier(frame.acquireCB,images[*index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  VK(vkEndCommandBuffer(frame.acquireCB));
  frame.reuseAcquireRecorded = frame.used;
 }
 VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
 submit.commandBufferCount=1;
 submit.pCommandBuffers=&frame.acquireCB;
 submit.signalSemaphoreCount=sem?1:0;
 submit.pSignalSemaphores=sem?&sem:nullptr;
 VK(WMR_PACING_CALL("Vulkan::submitDirectAcquire",vkQueueSubmit(render.queue,1,&submit,fence)));
 acquireEnd=perf?bridgeNow():0;return VK_SUCCESS;
}
void recordDirectRelease(DirectFrame& frame, VkImage image){ WMR_TIMING_SCOPE("ProxySwapchain::recordDirectRelease");
 begin(frame.releaseCB);
 if(frame.queries){
  vkCmdResetQueryPool(frame.releaseCB,frame.queries,0,2);
  vkCmdWriteTimestamp(frame.releaseCB,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,frame.queries,0);
 }
 sharedOwnership(frame.releaseCB,image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,render.family,VK_QUEUE_FAMILY_EXTERNAL,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,0,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
 if(frame.queries)vkCmdWriteTimestamp(frame.releaseCB,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,frame.queries,1);
 VK(vkEndCommandBuffer(frame.releaseCB));
}
VkResult presentDirect(VkQueue queue,const VkPresentInfoKHR*pi,uint32_t slot){ WMR_PACING_SCOPE("ProxySwapchain::presentDirect");
 const bool measure=perf||frames<5||(frames+1)%300==0;const auto start=measure?bridgeNow():0;const uint32_t index=pi->pImageIndices[slot];if(index>=images.size())return VK_ERROR_INITIALIZATION_FAILED;
 if(queue!=render.queue){auto state=dstate(queue);if(!state||state->handle!=render.device)throw std::runtime_error("Direct presentation queue belongs to another device");std::lock_guard<std::mutex>lock(state->queueFamiliesMutex);auto it=state->queueFamilies.find(queue);if(it==state->queueFamilies.end()||it->second!=render.family)throw std::runtime_error("Direct scanout queue family changes unsupported");render.queue=queue;}
 auto&frame=*directFrames[index];
 waitStages.resize(pi->waitSemaphoreCount,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=pi->waitSemaphoreCount;submit.pWaitSemaphores=pi->pWaitSemaphores;submit.pWaitDstStageMask=waitStages.data();submit.commandBufferCount=1;submit.pCommandBuffers=&frame.releaseCB;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&frame.produced;VK(WMR_PACING_CALL("Vulkan::submitDirectRelease",vkQueueSubmit(queue,1,&submit,frame.done)));frame.used=true;
 VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};get.semaphore=frame.produced;get.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;int fd=-1;VK(WMR_PACING_CALL("Vulkan::exportDirectSyncFd",exportSemaphoreFd(render.device,&get,&fd)));OwnedFd inFence(fd);
 uint64_t id=frames+1;for(auto*p=(const VkBaseInStructure*)pi->pNext;p;p=p->pNext){if(p->sType==VK_STRUCTURE_TYPE_PRESENT_ID_KHR)id=((const VkPresentIdKHR*)p)->pPresentIds[slot];else throw std::runtime_error("Direct scanout present pNext unsupported");}
 wmrTimingScope.tag=id;
 const auto commitStart=perf?bridgeNow():0;auto result=kms->present(index,fd,id);const auto end=measure?bridgeNow():0;frames++;
 if(perf&&!bridgeRunDirectory.empty()){
  WMR_TIMING_SCOPE("ProxySwapchain::writeDirectTelemetry");
  if(directLogPath.empty())directLogPath=bridgeRunDirectory/("direct-scanout-"+std::to_string(getpid())+"-"+std::to_string((uintptr_t)this)+".csv");
  const auto&path=directLogPath;
  if(directLog&&ftell(directLog)>4*1024*1024){fclose(directLog);directLog=nullptr;std::error_code error;std::filesystem::rename(path,path.string()+".previous",error);}
  if(!directLog){directLog=fopen(path.c_str(),"w");if(directLog){setvbuf(directLog,nullptr,_IOFBF,65536);fprintf(directLog,"frame,index,result,present_start_monotonic_ns,acquire_cpu_ms,source_retire_wait_ms,present_cpu_ms,kms_commit_and_wait_ms,explicit_gpu_copies\n");}}
  if(directLog){fprintf(directLog,"%llu,%u,%d,%lld,%.6f,%.6f,%.6f,%.6f,0\n",(unsigned long long)frames,index,result,(long long)start,(acquireEnd-acquireStart)/1e6,directRetireMs,(end-start)/1e6,(end-commitStart)/1e6);if(frames%90==0)fflush(directLog);}
 }
 if(frames<=5||frames%300==0){log("DIRECT_PRESENT frames=%llu result=%d present_id=%llu cpu_ms=%.6f",(unsigned long long)frames,result,(unsigned long long)id,(end-start)/1e6);bridgeHealth(result>=0?"presenting":"error",frames,result);}return result;
}
