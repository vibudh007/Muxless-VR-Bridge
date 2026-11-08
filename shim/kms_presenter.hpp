#pragma once
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <poll.h>
#include <cmath>
#include <cerrno>
#include <limits>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include "../common/shared_image.hpp"
#include "atomic_request.hpp"
#include "function_timing.hpp"

// Experimental private output backend. One pending flip, no presentation thread,
// no GPU copy, and only lease-owned CRTC/plane/connector objects are touched.
class KmsPresenter {
 struct Image {uint32_t gem=0,fb=0;enum State{Available,Acquired,Queued,Scanning}state=Available;};
 bool monotonicEvents_=false;
 OwnedFd timingFence_{-1};
 int fd_=-1;uint32_t connector_=0,crtc_=0,plane_=0,blob_=0,width_=0,height_=0,fourcc_=0;
 struct Property {uint32_t object,type,id;const char*name;};
 AtomicRequestBuffer requestBuffer_;
 std::vector<Image>images_;
 // Names are internal string literals. Populate once during TEST_ONLY; no
 // temporary strings/tree nodes or cache growth on steady-state commits.
 std::vector<Property>properties_;
 std::recursive_mutex mutex_;bool active_=false,pending_=false;int queued_=-1,scanning_=-1;uint64_t queuedId_=0,completedId_=0,baseSequence_=0;bool sequenceBase_=false;uint64_t waitCalls_=0,waitTimeouts_=0;
 uint32_t property(uint32_t id,uint32_t type,const char*name){ WMR_TIMING_SCOPE("KmsPresenter::property");
  for(const auto&entry:properties_)if(entry.object==id&&entry.type==type&&!strcmp(entry.name,name))return entry.id;
  auto*list=drmModeObjectGetProperties(fd_,id,type);if(!list)throw std::runtime_error("KMS object unavailable");uint32_t result=0;
  for(uint32_t i=0;i<list->count_props;i++){auto*p=drmModeGetProperty(fd_,list->props[i]);if(p){if(!strcmp(name,p->name))result=p->prop_id;drmModeFreeProperty(p);}}
  drmModeFreeObjectProperties(list);if(!result)throw std::runtime_error(std::string("KMS property unavailable: ")+name);properties_.push_back({id,type,result,name});return result;
 }
 void add(drmModeAtomicReq*req,uint32_t id,uint32_t type,const char*name,uint64_t value){if(drmModeAtomicAddProperty(req,id,property(id,type,name),value)<0)throw std::runtime_error("KMS property add failed");}
 drmModeAtomicReq*request(uint32_t image,int inFence,bool mode){ WMR_TIMING_SCOPE("KmsPresenter::request");
  auto*req=requestBuffer_.begin();
  try{
   if(mode){add(req,connector_,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",crtc_);add(req,crtc_,DRM_MODE_OBJECT_CRTC,"MODE_ID",blob_);add(req,crtc_,DRM_MODE_OBJECT_CRTC,"ACTIVE",1);}
   add(req,plane_,DRM_MODE_OBJECT_PLANE,"FB_ID",images_.at(image).fb);add(req,plane_,DRM_MODE_OBJECT_PLANE,"CRTC_ID",crtc_);
   for(const char*name:{"SRC_X","SRC_Y","CRTC_X","CRTC_Y"})add(req,plane_,DRM_MODE_OBJECT_PLANE,name,0);
   add(req,plane_,DRM_MODE_OBJECT_PLANE,"SRC_W",uint64_t(width_)<<16);add(req,plane_,DRM_MODE_OBJECT_PLANE,"SRC_H",uint64_t(height_)<<16);add(req,plane_,DRM_MODE_OBJECT_PLANE,"CRTC_W",width_);add(req,plane_,DRM_MODE_OBJECT_PLANE,"CRTC_H",height_);
   if(inFence>=0)add(req,plane_,DRM_MODE_OBJECT_PLANE,"IN_FENCE_FD",uint64_t(inFence));
   return req;
  }catch(...){drmModeAtomicSetCursor(req,0);throw;}
 }
 static void flipped(int,unsigned sequence,unsigned sec,unsigned usec,void*data){ WMR_PACING_SCOPE("KmsPresenter::flipped");
  auto*self=(KmsPresenter*)data;
  if(self->scanning_>=0)self->images_[self->scanning_].state=Image::Available;
  self->scanning_=self->queued_;self->images_[self->scanning_].state=Image::Scanning;
  self->completedId_=std::max(self->completedId_,self->queuedId_);self->pending_=false;self->queued_=-1;wmrTimingScope.tag=self->queuedId_;
  if(wmrTimingScope.stream&&self->monotonicEvents_&&(sec||usec)){
   wmrTimingScope.event("DRM::flipTimestamp",(uint64_t(sec)*1000000+usec)*1000,self->queuedId_);
   if(self->timingFence_.fd>=0){
    WMR_PACING_SCOPE("DRM::readReleaseFence");
    sync_fence_info fences[8]{};
    sync_file_info info{};
    info.num_fences=8;
    info.sync_fence_info=reinterpret_cast<uintptr_t>(fences);
    if(ioctl(self->timingFence_.fd,SYNC_IOC_FILE_INFO,&info)==0&&info.status==1&&info.num_fences<=8){
     uint64_t ready=0;
     for(unsigned i=0;i<info.num_fences;i++)if(fences[i].status==1)ready=std::max(ready,uint64_t(fences[i].timestamp_ns));
     if(ready)wmrTimingScope.event("DRM::releaseFenceReady",ready,self->queuedId_);
    }
   }
  }
  if(self->timingFence_.fd>=0){close(self->timingFence_.fd);self->timingFence_.release();}
  if(!self->sequenceBase_&&(sec||usec)){self->baseSequence_=sequence;self->sequenceBase_=true;}
  if(self->completedId_<=5||self->completedId_%300==0)log("KMS_FLIP present_id=%llu sequence=%u event_us=%llu",(unsigned long long)self->completedId_,sequence,(unsigned long long)(uint64_t(sec)*1000000+usec));
 }
 VkResult pump(uint64_t timeout){
  WMR_TIMING_SCOPE("KmsPresenter::pump");
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  uint64_t remaining = timeout;
  for (;;) {
   // ppoll accepts nanoseconds. Rounding up to a millisecond can wait past
   // the caller's deadline, especially after an interrupted poll.
   const auto slice = std::min<uint64_t>(remaining, 50000000);
   const timespec wait{0, static_cast<long>(slice)};
   pollfd descriptor{fd_, POLLIN, 0};
   const int ready = WMR_TIMING_CALL("DRM::pollLease", ppoll(&descriptor, 1, &wait, nullptr));
   if (ready > 0) {
    if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) return VK_ERROR_SURFACE_LOST_KHR;
    drmEventContext context{};
    context.version = 2;
    context.page_flip_handler = flipped;
    if (WMR_TIMING_CALL("DRM::handleEvent", drmHandleEvent(fd_, &context))) return VK_ERROR_SURFACE_LOST_KHR;
    return VK_SUCCESS;
   }
   if (ready < 0 && errno != EINTR) return VK_ERROR_SURFACE_LOST_KHR;
   if (timeout == 0) return VK_NOT_READY;
   if (timeout != UINT64_MAX) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    if (uint64_t(elapsed) >= timeout) return VK_TIMEOUT;
    remaining = timeout - uint64_t(elapsed);
   }
  }
 }

public:
 KmsPresenter(int fd,uint32_t connector,uint32_t width,uint32_t height,double refresh,VkFormat format):connector_(connector),width_(width),height_(height){ WMR_TIMING_SCOPE("KmsPresenter::KmsPresenter");
  fd_=dup(fd);if(fd_<0)throw std::runtime_error("KMS lease duplication failed");
  try{
   if(bridgeTiming::enabled.load(std::memory_order_acquire)&&bridgeTiming::pacingOnly){uint64_t monotonic=0;monotonicEvents_=!drmGetCap(fd_,DRM_CAP_TIMESTAMP_MONOTONIC,&monotonic)&&monotonic;}
   if(drmSetClientCap(fd_,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1)||drmSetClientCap(fd_,DRM_CLIENT_CAP_ATOMIC,1))throw std::runtime_error("Atomic KMS unavailable");
   auto*resources=drmModeGetResources(fd_);if(!resources||!resources->count_crtcs){if(resources)drmModeFreeResources(resources);throw std::runtime_error("Lease has no CRTC");}crtc_=resources->crtcs[0];drmModeFreeResources(resources);
   auto*con=drmModeGetConnector(fd_,connector_);if(!con)throw std::runtime_error("Lease connector unavailable");drmModeModeInfo mode{};
   for(int i=0;i<con->count_modes;i++){auto&m=con->modes[i];double hz=double(m.clock)*1000/m.htotal/m.vtotal;if(m.hdisplay==width&&m.vdisplay==height&&std::abs(hz-refresh)<.1){mode=m;break;}}
   drmModeFreeConnector(con);if(!mode.clock)throw std::runtime_error("KMS requested mode unavailable");if(drmModeCreatePropertyBlob(fd_,&mode,sizeof(mode),&blob_))throw std::runtime_error("KMS mode blob failed");
   bool rgba=format==VK_FORMAT_R8G8B8A8_UNORM||format==VK_FORMAT_R8G8B8A8_SRGB;fourcc_=rgba?DRM_FORMAT_ABGR8888:DRM_FORMAT_ARGB8888;
   auto*planes=drmModeGetPlaneResources(fd_);if(!planes)throw std::runtime_error("Lease planes unavailable");
   for(uint32_t i=0;i<planes->count_planes&&!plane_;i++){auto*p=drmModeGetPlane(fd_,planes->planes[i]);if(!p)continue;auto*list=drmModeObjectGetProperties(fd_,p->plane_id,DRM_MODE_OBJECT_PLANE);bool primary=false;
    if(list){for(uint32_t j=0;j<list->count_props;j++){auto*v=drmModeGetProperty(fd_,list->props[j]);if(v){if(!strcmp(v->name,"type")&&list->prop_values[j]==DRM_PLANE_TYPE_PRIMARY)primary=true;drmModeFreeProperty(v);}}drmModeFreeObjectProperties(list);}
    if(primary&&std::find(p->formats,p->formats+p->count_formats,fourcc_)!=p->formats+p->count_formats)plane_=p->plane_id;
    drmModeFreePlane(p);
   }
   drmModeFreePlaneResources(planes);if(!plane_)throw std::runtime_error("No lease-owned primary plane supports format");
   property(plane_,DRM_MODE_OBJECT_PLANE,"IN_FENCE_FD");
  }catch(...){if(blob_)drmModeDestroyPropertyBlob(fd_,blob_);close(fd_);fd_=-1;throw;}
 }
 void addImage(SharedLinearImage&image){ WMR_TIMING_SCOPE("KmsPresenter::addImage");
  Image entry;VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};get.memory=image.memory;get.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;int prime=-1;auto exportFd=(PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(image.source->device,"vkGetMemoryFdKHR");VK(exportFd(image.source->device,&get,&prime));OwnedFd owned(prime);
  if(drmPrimeFDToHandle(fd_,prime,&entry.gem))throw std::runtime_error("KMS PRIME image import failed");
  if(image.layout.rowPitch>UINT32_MAX||image.layout.offset>UINT32_MAX){drm_gem_close closeRequest{};closeRequest.handle=entry.gem;drmIoctl(fd_,DRM_IOCTL_GEM_CLOSE,&closeRequest);throw std::runtime_error("KMS image pitch/offset exceeds32bits");}
  uint32_t handles[4]={entry.gem},pitches[4]={uint32_t(image.layout.rowPitch)},offsets[4]={uint32_t(image.layout.offset)};uint64_t mods[4]={};
  if(drmModeAddFB2WithModifiers(fd_,width_,height_,fourcc_,handles,pitches,offsets,mods,&entry.fb,DRM_MODE_FB_MODIFIERS)){drm_gem_close closeRequest{};closeRequest.handle=entry.gem;drmIoctl(fd_,DRM_IOCTL_GEM_CLOSE,&closeRequest);throw std::runtime_error(std::string("KMS framebuffer rejected: ")+strerror(errno));}images_.push_back(entry);
 }
 void test(){ WMR_TIMING_SCOPE("KmsPresenter::test");auto*req=request(0,-1,true);int r=drmModeAtomicCommit(fd_,req,DRM_MODE_ATOMIC_TEST_ONLY|DRM_MODE_ATOMIC_ALLOW_MODESET,nullptr);int error=errno;if(r)throw std::runtime_error(std::string("KMS test failed: ")+strerror(error));}
 VkResult acquire(uint64_t timeout,uint32_t*index){ WMR_TIMING_SCOPE("KmsPresenter::acquire");
  std::lock_guard<std::recursive_mutex>lock(mutex_);auto start=std::chrono::steady_clock::now();
  for(;;){if(pending_){auto result=pump(0);if(result<0)return result;}
   for(uint32_t i=0;i<images_.size();i++)if(images_[i].state==Image::Available){images_[i].state=Image::Acquired;*index=i;return VK_SUCCESS;}
   uint64_t remaining=timeout;if(timeout!=UINT64_MAX){auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());remaining=elapsed<timeout?timeout-elapsed:0;}
   auto result=pump(remaining);if(result!=VK_SUCCESS)return timeout>0&&result==VK_NOT_READY?VK_TIMEOUT:result;
  }
 }
 VkResult present(uint32_t index,int inFence,uint64_t id){ WMR_PACING_SCOPE("KmsPresenter::present");wmrTimingScope.tag=id;
  std::lock_guard<std::recursive_mutex>lock(mutex_);if(index>=images_.size()||images_[index].state!=Image::Acquired)return VK_ERROR_INITIALIZATION_FAILED;
  while(pending_){auto result=pump(3000000000ull);if(result!=VK_SUCCESS)return result;}
  auto*req=request(index,inFence,!active_);int result=WMR_PACING_CALL("DRM::atomicCommit",drmModeAtomicCommit(fd_,req,DRM_MODE_ATOMIC_NONBLOCK|DRM_MODE_PAGE_FLIP_EVENT|(!active_?DRM_MODE_ATOMIC_ALLOW_MODESET:0),this));int error=errno;
  if(result){log("KMS_COMMIT_ERROR errno=%d reason=%s",error,strerror(error));return VK_ERROR_SURFACE_LOST_KHR;}
  if(wmrTimingScope.stream&&monotonicEvents_&&inFence>=0)timingFence_.fd=fcntl(inFence,F_DUPFD_CLOEXEC,0);
  images_[index].state=Image::Queued;queued_=index;queuedId_=id;pending_=true;active_=true;return VK_SUCCESS;
 }
 VkResult waitPresent(uint64_t id,uint64_t timeout){ WMR_PACING_SCOPE("KmsPresenter::waitPresent");wmrTimingScope.tag=id;
  std::lock_guard<std::recursive_mutex>lock(mutex_);auto start=std::chrono::steady_clock::now();
  ++waitCalls_;
  VkResult result=VK_SUCCESS;
  while(completedId_<id){
   uint64_t remaining=timeout;
   if(timeout!=UINT64_MAX){auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());remaining=elapsed<timeout?timeout-elapsed:0;}
   result=pump(remaining);
   if(result!=VK_SUCCESS){if(result==VK_NOT_READY)result=VK_TIMEOUT;break;}
  }
  if(result==VK_TIMEOUT)++waitTimeouts_;
  // Keep real timeout semantics: queued submission is not proof of scanout.
  // Bounded diagnostics distinguish short caller deadlines from a stalled flip.
  if(waitCalls_<=5||(result==VK_TIMEOUT&&(waitTimeouts_<=5||waitTimeouts_%300==0))||result<0){
   double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
   log("KMS_WAIT present_id=%llu timeout_ns=%llu completed_id=%llu queued_id=%llu pending=%d result=%d elapsed_ms=%.6f calls=%llu timeouts=%llu",(unsigned long long)id,(unsigned long long)timeout,(unsigned long long)completedId_,(unsigned long long)queuedId_,int(pending_),result,elapsed,(unsigned long long)waitCalls_,(unsigned long long)waitTimeouts_);
  }
  return result;
 }
 VkResult counter(uint64_t*out){ WMR_TIMING_SCOPE("KmsPresenter::counter");std::lock_guard<std::recursive_mutex>lock(mutex_);if(!active_){*out=0;return VK_SUCCESS;}uint64_t sequence=0,ns=0;if(drmCrtcGetSequence(fd_,crtc_,&sequence,&ns)){if(!sequenceBase_){*out=0;return VK_SUCCESS;}return VK_ERROR_SURFACE_LOST_KHR;}if(!ns){*out=0;return VK_SUCCESS;}if(!sequenceBase_){baseSequence_=sequence;sequenceBase_=true;}*out=sequence-baseSequence_;return VK_SUCCESS;}
 ~KmsPresenter(){ WMR_TIMING_SCOPE("KmsPresenter::~KmsPresenter");
  if(fd_<0)return;
  if(active_){std::lock_guard<std::recursive_mutex>lock(mutex_);if(pending_)pump(3000000000ull);auto*req=drmModeAtomicAlloc();if(req){try{add(req,plane_,DRM_MODE_OBJECT_PLANE,"FB_ID",0);add(req,plane_,DRM_MODE_OBJECT_PLANE,"CRTC_ID",0);add(req,connector_,DRM_MODE_OBJECT_CONNECTOR,"CRTC_ID",0);add(req,crtc_,DRM_MODE_OBJECT_CRTC,"ACTIVE",0);add(req,crtc_,DRM_MODE_OBJECT_CRTC,"MODE_ID",0);if(drmModeAtomicCommit(fd_,req,DRM_MODE_ATOMIC_ALLOW_MODESET,nullptr))log("KMS_DETACH_ERROR errno=%d",errno);}catch(...){log("KMS_DETACH_ERROR exception");}drmModeAtomicFree(req);}}
  for(auto&image:images_){drmModeRmFB(fd_,image.fb);drm_gem_close closeRequest{};closeRequest.handle=image.gem;drmIoctl(fd_,DRM_IOCTL_GEM_CLOSE,&closeRequest);}if(blob_)drmModeDestroyPropertyBlob(fd_,blob_);close(fd_);
 }
};
