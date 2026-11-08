// Standalone experiment: direct scanout of a shared dGPU image, no iGPU copy.
#define main bench_program_main
#include "shared_image_bench.cpp"
#undef main
#include "wayland_lease.hpp"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <map>
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#include <cstring>
#include <csignal>
#include <cmath>
static volatile sig_atomic_t interrupted=0;
static void interruptToy(int){interrupted=1;}
struct KmsToy {
 Wayland lease;Bench render;
 int fd=-1;uint32_t connector=0,crtc=0,plane=0,modeBlob=0,fourcc=0;bool swapRB=false,active=false;
 std::map<std::pair<uint32_t,std::string>,uint32_t>props;
 struct Slot {std::unique_ptr<SharedLinearImage>extra;SharedLinearImage*image=nullptr;VkImageView view=VK_NULL_HANDLE;uint32_t gem=0,fb=0;};
 Slot slots[2];bool flipped=false;uint32_t lastSequence=0;uint64_t lastEventUs=0;
 uint32_t property(uint32_t id,uint32_t type,const char*name){
  auto key=std::make_pair(id,std::string(name));auto it=props.find(key);if(it!=props.end())return it->second;
  auto*object=drmModeObjectGetProperties(fd,id,type);if(!object)throw std::runtime_error("KMS object properties unavailable");uint32_t found=0;
  for(uint32_t i=0;i<object->count_props;i++){auto*p=drmModeGetProperty(fd,object->props[i]);if(p){if(!strcmp(p->name,name))found=p->prop_id;drmModeFreeProperty(p);}}
  drmModeFreeObjectProperties(object);if(!found)throw std::runtime_error(std::string("KMS property missing: ")+name);props[key]=found;return found;
 }
 void add(drmModeAtomicReq*req,uint32_t id,uint32_t type,const char*name,uint64_t value){if(drmModeAtomicAddProperty(req,id,property(id,type,name),value)<0)throw std::runtime_error("Atomic property add failed");}
 static void event(int,unsigned sequence,unsigned sec,unsigned usec,void*data){auto*self=(KmsToy*)data;self->flipped=true;self->lastSequence=sequence;self->lastEventUs=uint64_t(sec)*1000000+usec;}
 void init(){
  auto*c=lease.select("");connector=c->id;lease.acquire(c);fd=lease.leaseFd;
  if(drmSetClientCap(fd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1)||drmSetClientCap(fd,DRM_CLIENT_CAP_ATOMIC,1))throw std::runtime_error("Atomic KMS unavailable on lease");
  auto*resources=drmModeGetResources(fd);if(!resources||!resources->count_crtcs)throw std::runtime_error("Lease has no CRTC");crtc=resources->crtcs[0];drmModeFreeResources(resources);
  auto*con=drmModeGetConnector(fd,connector);if(!con)throw std::runtime_error("Leased connector unavailable");drmModeModeInfo mode{};
  for(int i=0;i<con->count_modes;i++){auto&m=con->modes[i];double hz=double(m.clock)*1000/m.htotal/m.vtotal;if(m.hdisplay==2880&&m.vdisplay==1440&&std::abs(hz-90)<.1){mode=m;break;}}
  drmModeFreeConnector(con);if(!mode.clock)throw std::runtime_error("Toy expects connected2880x1440@90Hz headset");if(drmModeCreatePropertyBlob(fd,&mode,sizeof(mode),&modeBlob))throw std::runtime_error("Mode blob failed");
  auto*planes=drmModeGetPlaneResources(fd);if(!planes)throw std::runtime_error("Lease planes unavailable");
  for(uint32_t i=0;i<planes->count_planes&&!plane;i++){
   auto*p=drmModeGetPlane(fd,planes->planes[i]);if(!p)continue;
   auto*op=drmModeObjectGetProperties(fd,p->plane_id,DRM_MODE_OBJECT_PLANE);bool primary=false;
   if(op){for(uint32_t j=0;j<op->count_props;j++){auto*v=drmModeGetProperty(fd,op->props[j]);if(v){if(!strcmp(v->name,"type")&&op->prop_values[j]==DRM_PLANE_TYPE_PRIMARY)primary=true;drmModeFreeProperty(v);}}drmModeFreeObjectProperties(op);}
   if(primary)for(uint32_t wanted:{DRM_FORMAT_ABGR8888,DRM_FORMAT_XBGR8888,DRM_FORMAT_ARGB8888,DRM_FORMAT_XRGB8888})if(!plane&&std::find(p->formats,p->formats+p->count_formats,wanted)!=p->formats+p->count_formats){plane=p->plane_id;fourcc=wanted;swapRB=wanted==DRM_FORMAT_ARGB8888||wanted==DRM_FORMAT_XRGB8888;}
   drmModeFreePlane(p);
  }
  drmModeFreePlaneResources(planes);if(!plane)throw std::runtime_error("No compatible leased primary plane");
  if(const char*modifier=getenv("WMR_BENCH_MODIFIER"))render.drmModifier=strtoull(modifier,nullptr,0);
  render.reverseAllocation=getenv("WMR_BENCH_REVERSE")!=nullptr;render.direct=true;render.systemMemory=getenv("WMR_KMS_SYSTEM")!=nullptr;render.init();
  // Match the discovered lease owner, not an assumed card/render-node number.
  struct stat st{};fstat(fd,&st);if(render.output.drm.primaryMajor!=major(st.st_rdev)||render.output.drm.primaryMinor!=minor(st.st_rdev))throw std::runtime_error("Benchmark iGPU does not own leased output");
  slots[0].image=render.shared.get();
  slots[1].extra=std::make_unique<SharedLinearImage>();slots[1].extra->make(render.source,render.output,VK_FORMAT_R8G8B8A8_UNORM,{render.width,render.height},VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,vkGetPhysicalDeviceImageFormatProperties2,render.systemMemory,vkGetPhysicalDeviceImageFormatProperties2,render.drmModifier,render.reverseAllocation);slots[1].image=slots[1].extra.get();
  for(auto&slot:slots){
   VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=slot.image->image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=VK_FORMAT_R8G8B8A8_UNORM;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VK(vkCreateImageView(render.source.device,&vi,nullptr,&slot.view));
   VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};get.memory=slot.image->outputOwnsAllocation?slot.image->imported:slot.image->memory;get.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;int primeFd=-1;auto exportFd=(PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(slot.image->outputOwnsAllocation?render.output.device:render.source.device,"vkGetMemoryFdKHR");VK(exportFd(slot.image->outputOwnsAllocation?render.output.device:render.source.device,&get,&primeFd));OwnedFd owned(primeFd);
   if(drmPrimeFDToHandle(fd,primeFd,&slot.gem))throw std::runtime_error(std::string("KMS PRIME import failed: ")+strerror(errno));
   if(slot.image->layout.rowPitch>UINT32_MAX||slot.image->layout.offset>UINT32_MAX)throw std::runtime_error("KMS layout exceeds32bit fields");
   uint32_t handles[4]={slot.gem},pitches[4]={uint32_t(slot.image->layout.rowPitch)},offsets[4]={uint32_t(slot.image->layout.offset)};uint64_t mods[4]={slot.image->drmModifier};
   if(drmModeAddFB2WithModifiers(fd,render.width,render.height,fourcc,handles,pitches,offsets,mods,&slot.fb,DRM_MODE_FB_MODIFIERS))throw std::runtime_error(std::string("KMS framebuffer import failed: ")+strerror(errno));
  }
  printf("KMS_IMPORT_OK connector=%u crtc=%u plane=%u framebuffer=%u format=0x%x\n",connector,crtc,plane,slots[0].fb,fourcc);
 }
 drmModeAtomicReq* request(Slot&slot,int inFence,bool mode){
  auto*req=drmModeAtomicAlloc();if(!req)throw std::runtime_error("Atomic alloc failed");
  try{
   if(mode){add(req,connector,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",crtc);add(req,crtc,DRM_MODE_OBJECT_CRTC,"MODE_ID",modeBlob);add(req,crtc,DRM_MODE_OBJECT_CRTC,"ACTIVE",1);}
   add(req,plane,DRM_MODE_OBJECT_PLANE,"FB_ID",slot.fb);add(req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_ID",crtc);
   add(req,plane,DRM_MODE_OBJECT_PLANE,"SRC_X",0);add(req,plane,DRM_MODE_OBJECT_PLANE,"SRC_Y",0);add(req,plane,DRM_MODE_OBJECT_PLANE,"SRC_W",uint64_t(render.width)<<16);add(req,plane,DRM_MODE_OBJECT_PLANE,"SRC_H",uint64_t(render.height)<<16);
   add(req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_X",0);add(req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_Y",0);add(req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_W",render.width);add(req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_H",render.height);
   if(inFence>=0)add(req,plane,DRM_MODE_OBJECT_PLANE,"IN_FENCE_FD",uint64_t(inFence));return req;
  }catch(...){drmModeAtomicFree(req);throw;}
 }
 void test(){auto*req=request(slots[0],-1,true);int r=drmModeAtomicCommit(fd,req,DRM_MODE_ATOMIC_TEST_ONLY|DRM_MODE_ATOMIC_ALLOW_MODESET,nullptr);int error=errno;drmModeAtomicFree(req);if(r)throw std::runtime_error(std::string("KMS test commit rejected: ")+strerror(error));puts("KMS_TEST_ONLY_PASS");}
 void frame(unsigned number,FILE*csv){
  auto&slot=slots[number%2];auto start=std::chrono::steady_clock::now();
  VkDescriptorImageInfo descriptor{VK_NULL_HANDLE,slot.view,VK_IMAGE_LAYOUT_GENERAL};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=render.set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pImageInfo=&descriptor;vkUpdateDescriptorSets(render.source.device,1,&write,0,nullptr);
  begin(render.sourceCB);vkCmdResetQueryPool(render.sourceCB,render.sourceQueries,0,4);
  if(number>=2)sharedOwnership(render.sourceCB,slot.image->image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_EXTERNAL,render.source.family,0,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  else imageBarrier(render.sourceCB,slot.image->image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  vkCmdWriteTimestamp(render.sourceCB,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,render.sourceQueries,0);
  struct Params{uint32_t w,h;float time;uint32_t swap;}params{render.width,render.height,number/90.f,swapRB?1u:0u};vkCmdBindPipeline(render.sourceCB,VK_PIPELINE_BIND_POINT_COMPUTE,render.pipeline);vkCmdBindDescriptorSets(render.sourceCB,VK_PIPELINE_BIND_POINT_COMPUTE,render.pipelineLayout,0,1,&render.set,0,nullptr);vkCmdPushConstants(render.sourceCB,render.pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT,0,16,&params);vkCmdDispatch(render.sourceCB,(render.width+15)/16,(render.height+15)/16,1);
  sharedOwnership(render.sourceCB,slot.image->image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,render.source.family,VK_QUEUE_FAMILY_EXTERNAL,VK_ACCESS_SHADER_WRITE_BIT,0,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);vkCmdWriteTimestamp(render.sourceCB,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,render.sourceQueries,1);VK(vkEndCommandBuffer(render.sourceCB));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&render.sourceCB;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&render.produced;VK(vkQueueSubmit(render.source.queue,1,&submit,VK_NULL_HANDLE));
  VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};get.semaphore=render.produced;get.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;int inFence=-1;auto exp=(PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(render.source.device,"vkGetSemaphoreFdKHR");VK(exp(render.source.device,&get,&inFence));OwnedFd fence(inFence);
  auto*req=request(slot,inFence,number==0);flipped=false;auto commitStart=std::chrono::steady_clock::now();int r=drmModeAtomicCommit(fd,req,DRM_MODE_ATOMIC_NONBLOCK|DRM_MODE_PAGE_FLIP_EVENT|(number==0?DRM_MODE_ATOMIC_ALLOW_MODESET:0),this);int error=errno;auto commitEnd=std::chrono::steady_clock::now();drmModeAtomicFree(req);if(r)throw std::runtime_error(std::string("KMS commit failed: ")+strerror(error));active=true;
  drmEventContext events{};events.version=2;events.page_flip_handler=event;
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while(!flipped&&!interrupted){if(std::chrono::steady_clock::now()>=deadline)throw std::runtime_error("KMS page flip timed out");pollfd descriptor{fd,POLLIN,0};int ready=poll(&descriptor,1,50);if(ready<0&&errno!=EINTR)throw std::runtime_error("KMS event poll failed");if(ready>0&&drmHandleEvent(fd,&events))throw std::runtime_error("KMS event dispatch failed");}
  VK(vkQueueWaitIdle(render.source.queue));uint64_t ticks[2];VK(vkGetQueryPoolResults(render.source.device,render.sourceQueries,0,2,sizeof(ticks),ticks,8,VK_QUERY_RESULT_64_BIT));
  auto ms=[](auto duration){return std::chrono::duration<double,std::milli>(duration).count();};fprintf(csv,"%u,%u,%llu,%.6f,%.6f,%.6f\n",number,lastSequence,(unsigned long long)lastEventUs,(ticks[1]-ticks[0])*double(render.source.props.limits.timestampPeriod)/1e6,ms(commitEnd-commitStart),ms(std::chrono::steady_clock::now()-start));fflush(csv);
 }
 ~KmsToy(){
  if(active){auto*req=drmModeAtomicAlloc();if(req){try{add(req,plane,DRM_MODE_OBJECT_PLANE,"FB_ID",0);add(req,plane,DRM_MODE_OBJECT_PLANE,"CRTC_ID",0);add(req,connector,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",0);add(req,crtc,DRM_MODE_OBJECT_CRTC,"ACTIVE",0);add(req,crtc,DRM_MODE_OBJECT_CRTC,"MODE_ID",0);if(drmModeAtomicCommit(fd,req,DRM_MODE_ATOMIC_ALLOW_MODESET,nullptr))fprintf(stderr,"KMS detach failed: %s\n",strerror(errno));}catch(...){}drmModeAtomicFree(req);}}
  if(render.source.device)vkDeviceWaitIdle(render.source.device);
  for(auto&slot:slots){if(slot.fb)drmModeRmFB(fd,slot.fb);if(slot.gem){drm_gem_close closeRequest{};closeRequest.handle=slot.gem;drmIoctl(fd,DRM_IOCTL_GEM_CLOSE,&closeRequest);}if(slot.view)vkDestroyImageView(render.source.device,slot.view,nullptr);}
  if(modeBlob)drmModeDestroyPropertyBlob(fd,modeBlob);
 }
};
int main(int argc,char**argv){try{signal(SIGINT,interruptToy);signal(SIGTERM,interruptToy);bool display=argc>1&&!strcmp(argv[1],"--display");unsigned frames=argc>3?std::stoul(argv[3]):1350;if(frames>5400)throw std::runtime_error("Frame limit too high");KmsToy toy;toy.init();toy.test();if(!display)return 0;if(argc<3)throw std::runtime_error("Usage: kms-shared-toy --display OUTPUT.csv [frames]");FILE*f=fopen(argv[2],"w");if(!f)throw std::runtime_error("CSV open failed");fprintf(f,"frame,sequence,event_monotonic_us,producer_gpu_ms,atomic_commit_cpu_ms,frame_host_ms\n");for(unsigned i=0;i<frames&&!interrupted;i++)toy.frame(i,f);fclose(f);puts("KMS_DISPLAY_PASS");return 0;}catch(const std::exception&e){fprintf(stderr,"KMS_ERROR %s\n",e.what());return 1;}}
