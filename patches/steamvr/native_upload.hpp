#pragma once
// SVR-006: recognized native buffer uploaders lack ring-full recovery.
// Intercept before it reserves memory; retain its original copy/barrier logic.
// Only process memory is patched, once before the first renderer is initialized.
#include <algorithm>
#include <atomic>
#include <string_view>
#include <cstring>
#include <link.h>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <unordered_map>
#include <unistd.h>
#include "transfer_ring.hpp"

namespace nativeUpload {
using Upload=void(*)(void*,VkBuffer,uint64_t,uint64_t,const void*,uint32_t);
inline std::atomic<Upload> original{nullptr};
inline std::mutex mutex;
inline std::unordered_map<uintptr_t,std::weak_ptr<TransferRing>> renderers;
inline void upload(void*renderer,VkBuffer dst,uint64_t offset,uint64_t size,const void*data,uint32_t usage){
 auto next=original.load(std::memory_order_acquire);
 // Ordinary uploads keep their original path with only a cursor/bounds check.
 // No registry lookup, allocation, syscall or mutex on the nonoverflow path.
 if(!size||(size<=65535&&size%4==0)){next(renderer,dst,offset,size,data,usage);return;}
 uint64_t used;std::memcpy(&used,static_cast<unsigned char*>(renderer)+0x460,sizeof(used));
 if(used<TransferRing::capacity&&size<TransferRing::capacity-used){next(renderer,dst,offset,size,data,usage);return;}
 std::shared_ptr<TransferRing>ring;
 {std::lock_guard<std::mutex>g(mutex);auto i=renderers.find(reinterpret_cast<uintptr_t>(renderer));if(i!=renderers.end())ring=i->second.lock();}
 if(!ring){next(renderer,dst,offset,size,data,usage);return;}
 // Scope restores the allocator before returning. Pending primary-ring reads
 // are never overwritten, rewound or waited upon here.
 // The native allocator rejects an individual reservation >= its capacity.
 // Split only such oversized uploads; preserve byte offsets and alignment.
 auto*source=static_cast<const unsigned char*>(data);
 while(size){
  auto part=std::min(size,TransferRing::capacity-4);
  {auto scope=ring->spill(reinterpret_cast<uintptr_t>(renderer)+0x448,part);
   next(renderer,dst,offset,part,source,usage);}
  size-=part;offset+=part;source+=part;
 }
}
inline void jump(unsigned char*out,uintptr_t target){
 const unsigned char instruction[6]={0xff,0x25,0,0,0,0};std::memcpy(out,instruction,6);std::memcpy(out+6,&target,8);
}
inline bool installed(){return original.load(std::memory_order_acquire)!=nullptr;}

inline bool install(){
 static const bool ready=[](){
#if defined(__x86_64__)
  static constexpr std::string_view entryBytes{
   "\x55\x4d\x89\xc3\x48\x89\xe5\x41\x57\x45\x89\xcf\x41\x56",14};
  // Match the ABI, cursor and primary allocation layout before detouring.
  static constexpr std::string_view cursorBound{
   "\x48\x8b\xbb\x60\x04\x00\x00\x4a\x8d\x04\x27\x48\x3d\xff\xff\xff\x0f",17};
  static constexpr std::string_view mappedAddress{"\x48\x03\xbb\x48\x04\x00\x00",7};
  static constexpr std::string_view stagingBuffer{"\x48\x8b\xb3\x50\x04\x00\x00",7};
  static constexpr std::string_view cursorAdvance{"\x4c\x01\xa3\x60\x04\x00\x00",7};

  uintptr_t entry=0;
  dl_iterate_phdr([](dl_phdr_info*info,size_t,void*raw){
   if(info->dlpi_name&&*info->dlpi_name)return 0;
   auto&result=*static_cast<uintptr_t*>(raw);
   for(unsigned i=0;i<info->dlpi_phnum;++i){
    const auto&segment=info->dlpi_phdr[i];
    if(segment.p_type!=PT_LOAD||(segment.p_flags&(PF_X|PF_R))!=(PF_X|PF_R))continue;
    constexpr size_t inspectedBytes=0x1a0;
    const auto*start=reinterpret_cast<const char*>(info->dlpi_addr+segment.p_vaddr);
    if(segment.p_filesz<inspectedBytes)continue;
    const auto*end=start+segment.p_filesz;
    const char*match=start;
    while((match=std::search(match,end,entryBytes.begin(),entryBytes.end()))!=end){
     if(size_t(end-match)<inspectedBytes)break;
     const std::string_view body(match,inspectedBytes);
     auto cursor=body.find(cursorBound);
     auto address=body.find(mappedAddress);
     auto buffer=body.find(stagingBuffer);
     auto advance=body.find(cursorAdvance);
     if(cursor!=body.npos&&address!=body.npos&&buffer!=body.npos&&advance!=body.npos&&
        cursor<address&&address<buffer&&buffer<advance){
      if(result){result=0;return 1;}
      result=reinterpret_cast<uintptr_t>(match);
     }
     ++match;
    }
   }
   return 1;
  },&entry);
  if(!entry)return false;
 long page=sysconf(_SC_PAGESIZE);if(page<=0)return false;
  auto*trampoline=static_cast<unsigned char*>(mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));if(trampoline==MAP_FAILED)return false;
  std::memcpy(trampoline,entryBytes.data(),entryBytes.size());jump(trampoline+14,entry+14);
  if(mprotect(trampoline,page,PROT_READ|PROT_EXEC)){munmap(trampoline,page);return false;}
  auto first=entry&~(uintptr_t(page)-1);auto last=(entry+14+page-1)&~(uintptr_t(page)-1);
  if(mprotect(reinterpret_cast<void*>(first),last-first,PROT_READ|PROT_WRITE)){munmap(trampoline,page);return false;}
  original.store(reinterpret_cast<Upload>(trampoline),std::memory_order_release);
  jump(reinterpret_cast<unsigned char*>(entry),reinterpret_cast<uintptr_t>(upload));
  __builtin___clear_cache(reinterpret_cast<char*>(entry),reinterpret_cast<char*>(entry+14));
  if(mprotect(reinterpret_cast<void*>(first),last-first,PROT_READ|PROT_EXEC)){
   // Do not leave the executable page writable or nonexecutable after failure.
   std::memcpy(reinterpret_cast<void*>(entry),entryBytes.data(),entryBytes.size());
   if(mprotect(reinterpret_cast<void*>(first),last-first,PROT_READ|PROT_EXEC))_exit(127);
   original.store(nullptr,std::memory_order_release);munmap(trampoline,page);return false;
  }
  std::fprintf(stderr,"STEAMVR_RING native overflow spill installed; ring capacity unchanged\n");return true;
#else
  return false;
#endif
 }();return ready;
}
inline void track(uintptr_t slot,const std::shared_ptr<TransferRing>&ring){std::lock_guard<std::mutex>g(mutex);renderers[slot-0x448]=ring;}
inline void forget(const std::shared_ptr<TransferRing>&ring){std::lock_guard<std::mutex>g(mutex);for(auto i=renderers.begin();i!=renderers.end();)if(i->second.expired()||i->second.lock()==ring)i=renderers.erase(i);else ++i;}
}
