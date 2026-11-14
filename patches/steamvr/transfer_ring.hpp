#pragma once
// SVR-006: exact-build SteamVR staging-ring retirement. No game or bridge changes.
#include <vulkan/vulkan.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>

class TransferRing {
public:
 static constexpr uint64_t capacity=256ULL*1024*1024;
 struct Fields {uintptr_t mapped;VkBuffer buffer;VkDeviceMemory memory;uint64_t used;};
 struct Buffer {VkDeviceSize size;VkBufferUsageFlags usage;VkDeviceMemory memory=VK_NULL_HANDLE;VkDeviceSize offset=0;};
 struct Command {bool ended=false,submitted=false;uint64_t serial=0;};
 struct Ring {uintptr_t slot;Fields identity;pid_t owner=0;bool disabled=false;uint64_t lastEnd=UINT64_MAX,resets=0,lastSerial=0;std::unordered_map<VkCommandBuffer,Command> commands;};
 struct Completion {VkFence fence;uint64_t serial;};
 struct Driver {VkDevice device;PFN_vkGetDeviceProcAddr gdpa;};
 explicit TransferRing(Driver d):driver(d){}
 template<class T>T next(const char*n){return reinterpret_cast<T>(driver.gdpa(driver.device,n));}
 static bool read(uintptr_t address,Fields&out){iovec a{&out,sizeof(out)},b{reinterpret_cast<void*>(address),sizeof(out)};return process_vm_readv(getpid(),&a,1,&b,1,0)==ssize_t(sizeof(out));}
 static bool writeCursor(uintptr_t address){uint64_t zero=0;iovec a{&zero,8},b{reinterpret_cast<void*>(address+offsetof(Fields,used)),8};return process_vm_writev(getpid(),&a,1,&b,1,0)==8;}
 void created(VkBuffer b,const VkBufferCreateInfo&c){if(c.size==capacity&&c.usage==VK_BUFFER_USAGE_TRANSFER_SRC_BIT){std::lock_guard<std::mutex>g(lock);buffers[b]={c.size,c.usage};}}
 void bound(VkBuffer b,VkDeviceMemory m,VkDeviceSize offset){std::lock_guard<std::mutex>g(lock);auto i=buffers.find(b);if(i!=buffers.end()){i->second.memory=m;i->second.offset=offset;}}
 void mapped(VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,void**out){
  if(offset||(size!=VK_WHOLE_SIZE&&size<capacity))return;
  std::lock_guard<std::mutex>g(lock);
  for(auto&entry:buffers){auto&b=entry.second;if(b.memory!=memory||b.offset)continue;
   Fields f{};auto slot=reinterpret_cast<uintptr_t>(out);
   if(!read(slot,f)||f.mapped!=reinterpret_cast<uintptr_t>(*out)||f.buffer!=entry.first||f.memory!=memory||f.used>capacity)continue;
   if(rings.count(entry.first)){disable(rings.at(entry.first),"ring remapped");continue;}
   rings.emplace(entry.first,Ring{slot,f,0,false,UINT64_MAX,0,0,{}});
   std::fprintf(stderr,"STEAMVR_RING validated capacity=%llu buffer=%p cursor=%llu\n",(unsigned long long)capacity,(void*)entry.first,(unsigned long long)f.used);
  }
 }
 void destroyed(VkBuffer b){std::lock_guard<std::mutex>g(lock);rings.erase(b);buffers.erase(b);}
 void unmapped(VkDeviceMemory m){std::lock_guard<std::mutex>g(lock);for(auto&entry:rings)if(entry.second.identity.memory==m)disable(entry.second,"mapping released");}
 void copy(VkCommandBuffer c,VkBuffer b,uint64_t end){
  std::lock_guard<std::mutex>g(lock);auto i=rings.find(b);if(i==rings.end())return;auto&r=i->second;
  if(r.disabled||!sameThread(r))return;
  auto&command=r.commands[c];if(command.ended||command.submitted){disable(r,"copy outside fresh recording");return;}
  if(end!=UINT64_MAX&&end>capacity){disable(r,"copy exceeds allocation");return;}r.lastEnd=end;
 }
 void ended(VkCommandBuffer c){std::lock_guard<std::mutex>g(lock);for(auto&entry:rings){auto i=entry.second.commands.find(c);if(i!=entry.second.commands.end())i->second.ended=true;}}
 void begin(VkCommandBuffer c){std::lock_guard<std::mutex>g(lock);recycle();for(auto&entry:rings){auto&r=entry.second;auto i=r.commands.find(c);if(i!=r.commands.end()){if(!i->second.submitted)disable(r,"recording discarded before submit");r.commands.erase(i);}}}
 // Called before downstream submit, never changes caller's fence or semaphore lists.
 bool touches(uint32_t count,const VkSubmitInfo*infos){std::lock_guard<std::mutex>g(lock);recycle();bool touched=false;for(auto&entry:rings){auto&r=entry.second;if(r.disabled)continue;for(uint32_t i=0;i<count;i++)for(uint32_t j=0;j<infos[i].commandBufferCount;j++){auto c=r.commands.find(infos[i].pCommandBuffers[j]);if(c!=r.commands.end()){if(!c->second.ended||c->second.submitted)disable(r,"upload resubmission or incomplete recording");else touched=true;}}}return touched;}
 void submitted(VkQueue q,uint32_t count,const VkSubmitInfo*infos,VkFence completion){
  std::lock_guard<std::mutex>g(lock);uint64_t serial=++submittedSerial;
  completions.push_back({completion,serial});
  for(auto&entry:rings){auto&r=entry.second;if(r.disabled)continue;for(uint32_t i=0;i<count;i++)for(uint32_t j=0;j<infos[i].commandBufferCount;j++){
   auto c=r.commands.find(infos[i].pCommandBuffers[j]);if(c==r.commands.end())continue;
   if(!sameThread(r))break;
   if(queue&&queue!=q){disable(r,"upload queue changed");break;}
   queue=q;r.owner=tid();c->second.submitted=true;c->second.serial=serial;r.lastSerial=serial;
  }}
 }
 void failed(){std::lock_guard<std::mutex>g(lock);for(auto&entry:rings)disable(entry.second,"submission or completion fence failed");}
 void unsupported(){std::lock_guard<std::mutex>g(lock);for(auto&entry:rings)disable(entry.second,"unsupported command submission or secondary recording");}
 // DestroyDevice implies the application has completed all device work.
 void cleanup(){std::lock_guard<std::mutex>g(lock);for(auto&c:completions)next<PFN_vkDestroyFence>("vkDestroyFence")(driver.device,c.fence,nullptr);completions.clear();}
private:
 Driver driver;std::mutex lock;std::unordered_map<VkBuffer,Buffer>buffers;std::unordered_map<VkBuffer,Ring>rings;std::vector<Completion>completions;uint64_t submittedSerial=0,completedSerial=0;VkQueue queue=VK_NULL_HANDLE;
 static pid_t tid(){return static_cast<pid_t>(syscall(SYS_gettid));}
 static void disable(Ring&r,const char*why){if(!r.disabled)std::fprintf(stderr,"STEAMVR_RING inactive reason=%s\n",why);r.disabled=true;}
 static bool sameThread(Ring&r){if(r.owner&&r.owner!=tid()){disable(r,"upload author changed");return false;}return true;}
 void recycle(){
  // Poll only. No QueueWaitIdle/DeviceWaitIdle, no waits on future CPU work.
  size_t done=0;for(auto&c:completions){auto result=next<PFN_vkGetFenceStatus>("vkGetFenceStatus")(driver.device,c.fence);if(result==VK_NOT_READY)break;if(result!=VK_SUCCESS){for(auto&entry:rings)disable(entry.second,"completion status failed");break;}completedSerial=c.serial;next<PFN_vkDestroyFence>("vkDestroyFence")(driver.device,c.fence,nullptr);done++;}
  if(done)completions.erase(completions.begin(),completions.begin()+done);
  for(auto&entry:rings){auto&r=entry.second;if(r.disabled||!r.owner||r.commands.empty()||r.owner!=tid()||r.lastSerial>completedSerial)continue;
   bool complete=std::all_of(r.commands.begin(),r.commands.end(),[&](const auto&e){return e.second.submitted&&e.second.serial<=completedSerial;});if(!complete)continue;
   Fields f{};if(!read(r.slot,f)||f.mapped!=r.identity.mapped||f.buffer!=r.identity.buffer||f.memory!=r.identity.memory||f.used>capacity){disable(r,"allocator identity changed");continue;}
   // Unknown image-layout sizes and partially reserved uploads never authorize reuse.
   if(!f.used||f.used!=r.lastEnd)continue;
   if(!writeCursor(r.slot)){disable(r,"cursor write failed");continue;}
   r.lastEnd=UINT64_MAX;r.resets++;
   if(r.resets<=3||r.resets%128==0)std::fprintf(stderr,"STEAMVR_RING retired bytes=%llu cycles=%llu\n",(unsigned long long)f.used,(unsigned long long)r.resets);
  }
 }
};
static_assert(sizeof(TransferRing::Fields)==32&&offsetof(TransferRing::Fields,used)==24);
