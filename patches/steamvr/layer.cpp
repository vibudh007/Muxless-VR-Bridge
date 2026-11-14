// Independent SteamVR diagnostic workaround; no dependency on Muxless VR Bridge.
#include <vulkan/vk_layer.h>
#include "../../shim/third_party/picosha2/picosha2.h"
#include "patch_config.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <execinfo.h>
#include <memory>
#include <unwind.h>
#include <link.h>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unistd.h>
#include "tested_build.hpp"
#include "descriptor_arena.hpp"
#include "transfer_ring.hpp"
namespace {
struct Instance {VkInstance handle;PFN_vkGetInstanceProcAddr gipa;};
struct Device {PFN_vkGetDeviceProcAddr gdpa;PFN_vkCreateDescriptorPool createPool;PFN_vkDestroyDevice destroy;std::shared_ptr<DescriptorArena> arena{};std::shared_ptr<TransferRing> ring{};};
struct Pool {uint32_t limit=0;uint64_t live=0,allocations=0,frees=0,resets=0;};
struct Usage {std::mutex lock;std::unordered_map<VkDescriptorPool,Pool>pools;std::unordered_map<VkDescriptorSet,VkDescriptorPool>sets;};
std::unordered_map<void*,std::shared_ptr<Usage>> usage;
std::mutex mutex;
std::unordered_map<void*,Instance> instances;
std::unordered_map<void*,Device> devices;
template<class T> void* key(T h){return *reinterpret_cast<void**>(h);}
const BuildPolicy& buildPolicy(){
 static const BuildPolicy result=[]()->BuildPolicy{
  char exe[4096];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
  if(n<0||n>=ssize_t(sizeof(exe)-1))return {};
  exe[n]=0;
  const char*name=std::strrchr(exe,'/');if(!name||std::strcmp(name+1,"vrcompositor"))return {};
  std::ifstream file("/proc/self/exe",std::ios::binary);
  picosha2::hash256_one_by_one hash;char block[16384];
  while(file){file.read(block,sizeof(block));hash.process(block,block+file.gcount());}
  bool ok=file.eof();hash.finish();auto checksum=picosha2::get_hash_hex_string(hash);
  const char*hex=checksum.c_str();
  auto policy=ok?policyFor(hex):BuildPolicy{};
  std::fprintf(stderr,"STEAMVR_PATCHES build=%s sha256=%s descriptors=%d transfer_ring=%d shader=%d\n",policy.any()?"matched":"unrecognized; inactive",ok?hex:"unavailable",policy.descriptors,policy.transferRing,policy.shader);return policy;
 }();return result;
}
bool observerEnabled(){
 if(!buildPolicy().observer)return false;
 static const bool value=[](){std::string dir=patchConfigDirectory();std::ifstream f(dir+"/descriptor-observer.enabled");std::string v;std::getline(f,v);return v=="1";}();return value;
}
Instance instanceFor(VkPhysicalDevice h){std::lock_guard<std::mutex>lock(mutex);return instances.at(key(h));}
Device deviceFor(VkDevice h){std::lock_guard<std::mutex>lock(mutex);return devices.at(key(h));}
std::shared_ptr<Usage> usageFor(VkDevice d){std::lock_guard<std::mutex>lock(mutex);auto&u=usage[key(d)];if(!u)u=std::make_shared<Usage>();return u;}
#include "shader_adapter.hpp"
VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice d,const VkDescriptorPoolCreateInfo*c,const VkAllocationCallbacks*a,VkDescriptorPool*out){
 auto ds=deviceFor(d);auto result=ds.createPool(d,c,a,out);
 if(result==VK_SUCCESS&&ds.arena){try{ds.arena->track(*out,*c,a);}catch(const std::bad_alloc&){reinterpret_cast<PFN_vkDestroyDescriptorPool>(ds.gdpa(d,"vkDestroyDescriptorPool"))(d,*out,a);*out=VK_NULL_HANDLE;return VK_ERROR_OUT_OF_HOST_MEMORY;}}
 if(!observerEnabled())return result;
 if(result==VK_SUCCESS){auto u=usageFor(d);std::lock_guard<std::mutex>lock(u->lock);u->pools[*out].limit=c->maxSets;
  std::fprintf(stderr,"STEAMVR_OBSERVER pool-created pool=%p max_sets=%u flags=%u pnext=%d\n",(void*)*out,c->maxSets,c->flags,c->pNext!=nullptr);
  for(unsigned i=0;i<c->poolSizeCount;i++)std::fprintf(stderr,"STEAMVR_OBSERVER pool-capacity pool=%p type=%u count=%u\n",(void*)*out,c->pPoolSizes[i].type,c->pPoolSizes[i].descriptorCount);
 }return result;
}
struct CacheOwner {uintptr_t renderer=0,state=0;uint32_t generation=0;uint64_t entries=0;std::array<unsigned char,0x120> bytes{};};
CacheOwner captureOwner(){
 CacheOwner owner;
 static uintptr_t base=[](){uintptr_t b=0;dl_iterate_phdr([](dl_phdr_info*i,size_t,void*out){if(!i->dlpi_name||!i->dlpi_name[0]){*static_cast<uintptr_t*>(out)=i->dlpi_addr;return 1;}return 0;},&b);return b;}();
 struct Context {CacheOwner*owner;uintptr_t base;};Context data{&owner,base};
 _Unwind_Backtrace([](_Unwind_Context*c,void*raw){
  auto&d=*static_cast<Context*>(raw);auto ip=_Unwind_GetIP(c);
  if(ip!=d.base+0x438859)return _URC_NO_REASON;
  auto&r=*d.owner;r.renderer=_Unwind_GetGR(c,14);r.state=_Unwind_GetGR(c,12);
  if(!r.renderer||!r.state)return _URC_END_OF_STACK;
  // Exact executable identity gates all layout-dependent reads.
  std::memcpy(&r.generation,reinterpret_cast<void*>(r.renderer+0xe44),4);
  uintptr_t begin=0,end=0;std::memcpy(&begin,reinterpret_cast<void*>(r.renderer+0x1060),8);std::memcpy(&end,reinterpret_cast<void*>(r.renderer+0x1068),8);
  if(end>=begin&&(end-begin)%0x188==0&&(end-begin)/0x188<10000)r.entries=(end-begin)/0x188;
  std::memcpy(r.bytes.data(),reinterpret_cast<void*>(r.state),r.bytes.size());
  return _URC_END_OF_STACK;
 },&data);return owner;
}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice d,const VkDescriptorSetAllocateInfo*c,VkDescriptorSet*out){
 if(!observerEnabled()){auto ds=deviceFor(d);return ds.arena?ds.arena->allocate(c,out):reinterpret_cast<PFN_vkAllocateDescriptorSets>(ds.gdpa(d,"vkAllocateDescriptorSets"))(d,c,out);}
 auto owner=captureOwner();
 auto u=usageFor(d);uint64_t before=0;{std::lock_guard<std::mutex>lock(u->lock);before=u->pools[c->descriptorPool].live;}
 auto ds=deviceFor(d);auto result=ds.arena?ds.arena->allocate(c,out):reinterpret_cast<PFN_vkAllocateDescriptorSets>(ds.gdpa(d,"vkAllocateDescriptorSets"))(d,c,out);
 {std::lock_guard<std::mutex>lock(u->lock);auto&p=u->pools[c->descriptorPool];
  if(owner.renderer){
   std::fprintf(stderr,"STEAMVR_OWNER renderer=%p state=%p generation=%u entries=%llu key=",(void*)owner.renderer,(void*)owner.state,owner.generation,(unsigned long long)owner.entries);
   for(auto byte:owner.bytes)std::fprintf(stderr,"%02x",byte);
   std::fputc('\n',stderr);
  }
  if(result==VK_SUCCESS){for(unsigned i=0;i<c->descriptorSetCount;i++){u->sets[out[i]]=c->descriptorPool;std::fprintf(stderr,"STEAMVR_SET allocate set=%p pool=%p layout=%p\n",(void*)out[i],(void*)c->descriptorPool,(void*)c->pSetLayouts[i]);}p.live+=c->descriptorSetCount;p.allocations+=c->descriptorSetCount;}
  std::fprintf(stderr,"STEAMVR_OBSERVER allocate pool=%p before=%llu request=%u after=%llu limit=%u total_allocated=%llu total_freed=%llu resets=%llu result=%d\n",(void*)c->descriptorPool,(unsigned long long)before,c->descriptorSetCount,(unsigned long long)p.live,p.limit,(unsigned long long)p.allocations,(unsigned long long)p.frees,(unsigned long long)p.resets,int(result));
 }
 if(result!=VK_SUCCESS){void*frames[24];auto n=backtrace(frames,24);std::fprintf(stderr,"STEAMVR_OBSERVER allocation-failure-backtrace\n");backtrace_symbols_fd(frames,n,2);}
 return result;
}
VKAPI_ATTR VkResult VKAPI_CALL freeSets(VkDevice d,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet*sets){
 if(!observerEnabled()){auto ds=deviceFor(d);return ds.arena?ds.arena->free(pool,count,sets):reinterpret_cast<PFN_vkFreeDescriptorSets>(ds.gdpa(d,"vkFreeDescriptorSets"))(d,pool,count,sets);}
 auto ds=deviceFor(d);auto result=ds.arena?ds.arena->free(pool,count,sets):reinterpret_cast<PFN_vkFreeDescriptorSets>(ds.gdpa(d,"vkFreeDescriptorSets"))(d,pool,count,sets);
 if(result==VK_SUCCESS){auto u=usageFor(d);std::lock_guard<std::mutex>lock(u->lock);auto&p=u->pools[pool];
  for(unsigned i=0;i<count;i++){std::fprintf(stderr,"STEAMVR_SET free set=%p pool=%p\n",(void*)sets[i],(void*)pool);}
  for(unsigned i=0;i<count;i++)if(u->sets.erase(sets[i])){if(p.live)p.live--;p.frees++;}
  std::fprintf(stderr,"STEAMVR_OBSERVER free pool=%p count=%u after=%llu\n",(void*)pool,count,(unsigned long long)p.live);
 }
 if(result==VK_SUCCESS){void*stack[16];auto n=backtrace(stack,16);std::fprintf(stderr,"STEAMVR_OWNER free-backtrace\n");backtrace_symbols_fd(stack,n,2);}
 return result;
}
VKAPI_ATTR VkResult VKAPI_CALL resetPool(VkDevice d,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags){
 if(!observerEnabled()){auto ds=deviceFor(d);return ds.arena?ds.arena->reset(pool,flags):reinterpret_cast<PFN_vkResetDescriptorPool>(ds.gdpa(d,"vkResetDescriptorPool"))(d,pool,flags);}
 auto ds=deviceFor(d);auto result=ds.arena?ds.arena->reset(pool,flags):reinterpret_cast<PFN_vkResetDescriptorPool>(ds.gdpa(d,"vkResetDescriptorPool"))(d,pool,flags);
 if(result==VK_SUCCESS){auto u=usageFor(d);std::lock_guard<std::mutex>lock(u->lock);auto&p=u->pools[pool];p.live=0;p.resets++;
  for(auto i=u->sets.begin();i!=u->sets.end();)if(i->second==pool)i=u->sets.erase(i);else ++i;
  std::fprintf(stderr,"STEAMVR_OBSERVER reset pool=%p\n",(void*)pool);
 }return result;
}
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice d,VkDescriptorPool pool,const VkAllocationCallbacks*a){
 auto ds=deviceFor(d);if(ds.arena)ds.arena->destroy(pool,a);else reinterpret_cast<PFN_vkDestroyDescriptorPool>(ds.gdpa(d,"vkDestroyDescriptorPool"))(d,pool,a);
 if(!observerEnabled())return;
 auto u=usageFor(d);std::lock_guard<std::mutex>lock(u->lock);u->pools.erase(pool);
 for(auto i=u->sets.begin();i!=u->sets.end();)if(i->second==pool)i=u->sets.erase(i);else ++i;
 std::fprintf(stderr,"STEAMVR_OBSERVER destroy-pool pool=%p\n",(void*)pool);
}
VKAPI_ATTR VkResult VKAPI_CALL createView(VkDevice d,const VkImageViewCreateInfo*c,const VkAllocationCallbacks*a,VkImageView*out){
 auto r=reinterpret_cast<PFN_vkCreateImageView>(deviceFor(d).gdpa(d,"vkCreateImageView"))(d,c,a,out);
 if(r==VK_SUCCESS)std::fprintf(stderr,"STEAMVR_RESOURCE view-create view=%p image=%p\n",(void*)*out,(void*)c->image);
 return r;
}
VKAPI_ATTR void VKAPI_CALL destroyView(VkDevice d,VkImageView view,const VkAllocationCallbacks*a){
 std::fprintf(stderr,"STEAMVR_RESOURCE view-destroy view=%p\n",(void*)view);
 reinterpret_cast<PFN_vkDestroyImageView>(deviceFor(d).gdpa(d,"vkDestroyImageView"))(d,view,a);
}
VKAPI_ATTR void VKAPI_CALL updateSets(VkDevice d,uint32_t count,const VkWriteDescriptorSet*w,uint32_t copies,const VkCopyDescriptorSet*c){
 for(unsigned i=0;i<count;i++){
  if(w[i].descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE||w[i].descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE||w[i].descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER){
   for(unsigned j=0;j<w[i].descriptorCount;j++)std::fprintf(stderr,"STEAMVR_RESOURCE set-image set=%p binding=%u view=%p\n",(void*)w[i].dstSet,w[i].dstBinding,(void*)w[i].pImageInfo[j].imageView);
  }
 }
 reinterpret_cast<PFN_vkUpdateDescriptorSets>(deviceFor(d).gdpa(d,"vkUpdateDescriptorSets"))(d,count,w,copies,c);
}
template<class T> Device commandDevice(T h){std::lock_guard<std::mutex>g(mutex);return devices.at(key(h));}
template<class T> T ringNext(const Device&d,VkDevice h,const char*n){return reinterpret_cast<T>(d.gdpa(h,n));}
// Dispatchable queue/command handles share the device dispatch key. Pass their
// actual owning VkDevice to GDPA; never reinterpret a queue as VkDevice.
std::unordered_map<void*,VkDevice> deviceHandles;
VkDevice ringDevice(void*k){std::lock_guard<std::mutex>g(mutex);return deviceHandles.at(k);}
VKAPI_ATTR VkResult VKAPI_CALL ringCreateBuffer(VkDevice h,const VkBufferCreateInfo*c,const VkAllocationCallbacks*a,VkBuffer*out){auto d=deviceFor(h);auto r=ringNext<PFN_vkCreateBuffer>(d,h,"vkCreateBuffer")(h,c,a,out);if(r==VK_SUCCESS&&d.ring)d.ring->created(*out,*c);return r;}
VKAPI_ATTR VkResult VKAPI_CALL ringBindBuffer(VkDevice h,VkBuffer b,VkDeviceMemory m,VkDeviceSize o){auto d=deviceFor(h);auto r=ringNext<PFN_vkBindBufferMemory>(d,h,"vkBindBufferMemory")(h,b,m,o);if(r==VK_SUCCESS&&d.ring)d.ring->bound(b,m,o);return r;}
VKAPI_ATTR VkResult VKAPI_CALL ringMap(VkDevice h,VkDeviceMemory m,VkDeviceSize o,VkDeviceSize z,VkMemoryMapFlags f,void**out){auto d=deviceFor(h);auto r=ringNext<PFN_vkMapMemory>(d,h,"vkMapMemory")(h,m,o,z,f,out);if(r==VK_SUCCESS&&d.ring)d.ring->mapped(m,o,z,out);return r;}
VKAPI_ATTR void VKAPI_CALL ringDestroyBuffer(VkDevice h,VkBuffer b,const VkAllocationCallbacks*a){auto d=deviceFor(h);if(d.ring)d.ring->destroyed(b);ringNext<PFN_vkDestroyBuffer>(d,h,"vkDestroyBuffer")(h,b,a);}
VKAPI_ATTR void VKAPI_CALL ringUnmap(VkDevice h,VkDeviceMemory m){auto d=deviceFor(h);if(d.ring)d.ring->unmapped(m);ringNext<PFN_vkUnmapMemory>(d,h,"vkUnmapMemory")(h,m);}
VKAPI_ATTR VkResult VKAPI_CALL ringBegin(VkCommandBuffer c,const VkCommandBufferBeginInfo*i){auto d=commandDevice(c);auto h=ringDevice(key(c));if(d.ring)d.ring->begin(c);return ringNext<PFN_vkBeginCommandBuffer>(d,h,"vkBeginCommandBuffer")(c,i);}
VKAPI_ATTR VkResult VKAPI_CALL ringEnd(VkCommandBuffer c){auto d=commandDevice(c);auto h=ringDevice(key(c));auto r=ringNext<PFN_vkEndCommandBuffer>(d,h,"vkEndCommandBuffer")(c);if(r==VK_SUCCESS&&d.ring)d.ring->ended(c);return r;}
VKAPI_ATTR void VKAPI_CALL ringCopy(VkCommandBuffer c,VkBuffer src,VkBuffer dst,uint32_t n,const VkBufferCopy*r){auto d=commandDevice(c);auto h=ringDevice(key(c));if(d.ring)for(uint32_t i=0;i<n;i++)d.ring->copy(c,src,r[i].srcOffset>UINT64_MAX-r[i].size?UINT64_MAX:r[i].srcOffset+r[i].size);ringNext<PFN_vkCmdCopyBuffer>(d,h,"vkCmdCopyBuffer")(c,src,dst,n,r);}
VKAPI_ATTR void VKAPI_CALL ringImage(VkCommandBuffer c,VkBuffer src,VkImage dst,VkImageLayout l,uint32_t n,const VkBufferImageCopy*r){auto d=commandDevice(c);auto h=ringDevice(key(c));if(d.ring)d.ring->copy(c,src,UINT64_MAX);ringNext<PFN_vkCmdCopyBufferToImage>(d,h,"vkCmdCopyBufferToImage")(c,src,dst,l,n,r);}
VKAPI_ATTR void VKAPI_CALL ringExecute(VkCommandBuffer c,uint32_t n,const VkCommandBuffer*cmds){auto d=commandDevice(c);auto h=ringDevice(key(c));if(d.ring)d.ring->unsupported();ringNext<PFN_vkCmdExecuteCommands>(d,h,"vkCmdExecuteCommands")(c,n,cmds);}
VKAPI_ATTR void VKAPI_CALL ringFreeMemory(VkDevice h,VkDeviceMemory m,const VkAllocationCallbacks*a){auto d=deviceFor(h);if(d.ring)d.ring->unmapped(m);ringNext<PFN_vkFreeMemory>(d,h,"vkFreeMemory")(h,m,a);}
VKAPI_ATTR VkResult VKAPI_CALL ringSubmit2(VkQueue q,uint32_t n,const VkSubmitInfo2*i,VkFence f){auto d=commandDevice(q);auto h=ringDevice(key(q));if(d.ring)d.ring->unsupported();return ringNext<PFN_vkQueueSubmit2>(d,h,"vkQueueSubmit2")(q,n,i,f);}
VKAPI_ATTR void VKAPI_CALL ringCopy2(VkCommandBuffer c,const VkCopyBufferInfo2*i){auto d=commandDevice(c);auto h=ringDevice(key(c));if(d.ring)d.ring->unsupported();ringNext<PFN_vkCmdCopyBuffer2>(d,h,"vkCmdCopyBuffer2")(c,i);}
VKAPI_ATTR void VKAPI_CALL ringImage2(VkCommandBuffer c,const VkCopyBufferToImageInfo2*i){auto d=commandDevice(c);auto h=ringDevice(key(c));if(d.ring)d.ring->unsupported();ringNext<PFN_vkCmdCopyBufferToImage2>(d,h,"vkCmdCopyBufferToImage2")(c,i);}
VKAPI_ATTR VkResult VKAPI_CALL ringSubmit(VkQueue q,uint32_t n,const VkSubmitInfo*i,VkFence f){
 auto d=commandDevice(q);auto h=ringDevice(key(q));bool touched=d.ring&&d.ring->touches(n,i);
 VkFence completion=VK_NULL_HANDLE;
 if(touched){VkFenceCreateInfo c{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,nullptr,0};if(ringNext<PFN_vkCreateFence>(d,h,"vkCreateFence")(h,&c,nullptr,&completion)!=VK_SUCCESS){d.ring->failed();touched=false;}}
 auto submit=ringNext<PFN_vkQueueSubmit>(d,h,"vkQueueSubmit");auto result=submit(q,n,i,touched&&!f?completion:f);
 if(touched){auto checkpoint=result;if(result==VK_SUCCESS&&f)checkpoint=submit(q,0,nullptr,completion);
  if(checkpoint==VK_SUCCESS)d.ring->submitted(q,n,i,completion);
  else{d.ring->failed();ringNext<PFN_vkDestroyFence>(d,h,"vkDestroyFence")(h,completion,nullptr);}}
 return result;
}
PFN_vkVoidFunction observationHook(const char*n){
 const auto&policy=buildPolicy();
 if(!policy.any())return nullptr;
 if(policy.shader)if(auto shader=ShaderAdapter::hook(n))return shader;
 #define OBS(name,fn) if(!std::strcmp(n,name))return reinterpret_cast<PFN_vkVoidFunction>(fn)
 if(policy.transferRing){
 OBS("vkCreateBuffer",ringCreateBuffer);OBS("vkBindBufferMemory",ringBindBuffer);OBS("vkMapMemory",ringMap);OBS("vkDestroyBuffer",ringDestroyBuffer);OBS("vkUnmapMemory",ringUnmap);
 OBS("vkFreeMemory",ringFreeMemory);OBS("vkQueueSubmit2",ringSubmit2);OBS("vkQueueSubmit2KHR",ringSubmit2);OBS("vkCmdCopyBuffer2",ringCopy2);OBS("vkCmdCopyBuffer2KHR",ringCopy2);OBS("vkCmdCopyBufferToImage2",ringImage2);OBS("vkCmdCopyBufferToImage2KHR",ringImage2);
 OBS("vkBeginCommandBuffer",ringBegin);OBS("vkEndCommandBuffer",ringEnd);OBS("vkCmdCopyBuffer",ringCopy);OBS("vkCmdCopyBufferToImage",ringImage);OBS("vkQueueSubmit",ringSubmit);OBS("vkCmdExecuteCommands",ringExecute);
 }
 if(policy.descriptors){
 if(observerEnabled()){OBS("vkCreateImageView",createView);OBS("vkDestroyImageView",destroyView);OBS("vkUpdateDescriptorSets",updateSets);}
 OBS("vkCreateDescriptorPool",createPool);OBS("vkAllocateDescriptorSets",allocate);OBS("vkFreeDescriptorSets",freeSets);OBS("vkResetDescriptorPool",resetPool);OBS("vkDestroyDescriptorPool",destroyPool);
 }
 #undef OBS
 return nullptr;
}
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice d,const VkAllocationCallbacks*a){auto state=deviceFor(d);auto next=state.destroy;ShaderAdapter::cleanup(d);if(state.ring)state.ring->cleanup();{std::lock_guard<std::mutex>lock(mutex);devices.erase(key(d));usage.erase(key(d));deviceHandles.erase(key(d));}next(d,a);}
VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance h,const VkAllocationCallbacks*a){
 Instance state;{std::lock_guard<std::mutex>lock(mutex);state=instances.at(key(h));instances.erase(key(h));}
 reinterpret_cast<PFN_vkDestroyInstance>(state.gipa(h,"vkDestroyInstance"))(h,a);
}
VKAPI_ATTR VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo*c,const VkAllocationCallbacks*a,VkInstance*out){
 auto*chain=reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(c->pNext));
 while(chain&&(chain->sType!=VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO||chain->function!=VK_LAYER_LINK_INFO))chain=reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(chain->pNext));
 if(!chain)return VK_ERROR_INITIALIZATION_FAILED;
 auto next=chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;chain->u.pLayerInfo=chain->u.pLayerInfo->pNext;
 auto result=reinterpret_cast<PFN_vkCreateInstance>(next(VK_NULL_HANDLE,"vkCreateInstance"))(c,a,out);
 if(result==VK_SUCCESS){std::lock_guard<std::mutex>lock(mutex);instances[key(*out)]={*out,next};}return result;
}
VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice h,const VkDeviceCreateInfo*c,const VkAllocationCallbacks*a,VkDevice*out){
 auto*chain=reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(c->pNext));
 while(chain&&(chain->sType!=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO||chain->function!=VK_LAYER_LINK_INFO))chain=reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(chain->pNext));
 if(!chain)return VK_ERROR_INITIALIZATION_FAILED;
 auto gipa=chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;auto gdpa=chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;chain->u.pLayerInfo=chain->u.pLayerInfo->pNext;
 auto state=instanceFor(h);auto result=reinterpret_cast<PFN_vkCreateDevice>(gipa(state.handle,"vkCreateDevice"))(h,c,a,out);
 if(result==VK_SUCCESS){Device d{gdpa,reinterpret_cast<PFN_vkCreateDescriptorPool>(gdpa(*out,"vkCreateDescriptorPool")),reinterpret_cast<PFN_vkDestroyDevice>(gdpa(*out,"vkDestroyDevice"))};
  if(buildPolicy().descriptors){
   std::string dir=patchConfigDirectory();
   std::ifstream enabled(dir+"/descriptor-arena.enabled");std::string value;std::getline(enabled,value);
   if(value=="1")d.arena=std::make_shared<DescriptorArena>(DescriptorArena::Driver{*out,d.createPool,reinterpret_cast<PFN_vkDestroyDescriptorPool>(gdpa(*out,"vkDestroyDescriptorPool")),reinterpret_cast<PFN_vkAllocateDescriptorSets>(gdpa(*out,"vkAllocateDescriptorSets")),reinterpret_cast<PFN_vkFreeDescriptorSets>(gdpa(*out,"vkFreeDescriptorSets")),reinterpret_cast<PFN_vkResetDescriptorPool>(gdpa(*out,"vkResetDescriptorPool"))});
  }
  if(buildPolicy().transferRing){
   std::string dir=patchConfigDirectory();
   std::ifstream enabled(dir+"/transfer-ring.enabled");std::string value;std::getline(enabled,value);
   if(value=="1")d.ring=std::make_shared<TransferRing>(TransferRing::Driver{*out,gdpa});
  }
  if(buildPolicy().shader&&ShaderAdapter::enabled()){
   try{ShaderAdapter::registerDevice(*out,gdpa);}catch(const std::bad_alloc&){d.destroy(*out,a);*out=VK_NULL_HANDLE;return VK_ERROR_OUT_OF_HOST_MEMORY;}
  }
  std::lock_guard<std::mutex>lock(mutex);devices[key(*out)]=d;deviceHandles[key(*out)]=*out;
 }return result;
}
}
extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL patchGDPA(VkDevice d,const char*n){
 if(!std::strcmp(n,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(patchGDPA);
 if(!std::strcmp(n,"vkDestroyDevice"))return reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
 if(auto hook=observationHook(n))return hook;
 return d?deviceFor(d).gdpa(d,n):nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL patchGIPA(VkInstance h,const char*n){
 if(!std::strcmp(n,"vkGetInstanceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(patchGIPA);
 if(!std::strcmp(n,"vkGetDeviceProcAddr"))return reinterpret_cast<PFN_vkVoidFunction>(patchGDPA);
 if(!std::strcmp(n,"vkDestroyDevice"))return reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
 if(!std::strcmp(n,"vkCreateInstance"))return reinterpret_cast<PFN_vkVoidFunction>(createInstance);
 if(!std::strcmp(n,"vkDestroyInstance"))return reinterpret_cast<PFN_vkVoidFunction>(destroyInstance);
 if(!std::strcmp(n,"vkCreateDevice"))return reinterpret_cast<PFN_vkVoidFunction>(createDevice);
 if(auto hook=observationHook(n))return hook;
 if(!h)return nullptr;
 Instance s;{std::lock_guard<std::mutex>lock(mutex);s=instances.at(key(h));}return s.gipa(h,n);
}
__attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface*v){
 if(v->loaderLayerInterfaceVersion>2)v->loaderLayerInterfaceVersion=2;
 v->pfnGetInstanceProcAddr=patchGIPA;v->pfnGetDeviceProcAddr=patchGDPA;v->pfnGetPhysicalDeviceProcAddr=nullptr;return VK_SUCCESS;
}
}
