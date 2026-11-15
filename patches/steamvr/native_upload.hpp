#pragma once
// SVR-006: the verified 2.19.3 buffer uploader has no ring-full recovery.
// Intercept before it reserves memory; retain its original copy/barrier logic.
// Only process memory is patched, once before the first renderer is initialized.
#include <atomic>
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
inline bool install(){
 static const bool ready=[](){
#if defined(__x86_64__)
  struct Mapping {uintptr_t entry=0;};Mapping mapping;
  dl_iterate_phdr([](dl_phdr_info*i,size_t,void*raw){
   if(i->dlpi_name&&*i->dlpi_name)return 0;
   auto&out=*static_cast<Mapping*>(raw);uintptr_t entry=i->dlpi_addr+0x42fa40;
   for(unsigned n=0;n<i->dlpi_phnum;++n){auto&p=i->dlpi_phdr[n];if(p.p_type==PT_LOAD&&(p.p_flags&PF_X)&&entry>=i->dlpi_addr+p.p_vaddr&&entry+14<=i->dlpi_addr+p.p_vaddr+p.p_memsz)out.entry=entry;}
   return 1;
  },&mapping);
  // Complete instructions; none is PC-relative. Both entry and ABI are gated by
  // the executable SHA256 in tested_build.hpp, not by a loose byte-pattern search.
  const unsigned char expected[14]={0x55,0x4d,0x89,0xc3,0x48,0x89,0xe5,0x41,0x57,0x45,0x89,0xcf,0x41,0x56};
  if(!mapping.entry||std::memcmp(reinterpret_cast<void*>(mapping.entry),expected,sizeof(expected)))return false;
  long page=sysconf(_SC_PAGESIZE);if(page<=0)return false;
  auto*trampoline=static_cast<unsigned char*>(mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));if(trampoline==MAP_FAILED)return false;
  std::memcpy(trampoline,expected,sizeof(expected));jump(trampoline+14,mapping.entry+14);
  if(mprotect(trampoline,page,PROT_READ|PROT_EXEC)){munmap(trampoline,page);return false;}
  auto first=mapping.entry&~(uintptr_t(page)-1);auto last=(mapping.entry+14+page-1)&~(uintptr_t(page)-1);
  if(mprotect(reinterpret_cast<void*>(first),last-first,PROT_READ|PROT_WRITE)){munmap(trampoline,page);return false;}
  original.store(reinterpret_cast<Upload>(trampoline),std::memory_order_release);
  jump(reinterpret_cast<unsigned char*>(mapping.entry),reinterpret_cast<uintptr_t>(upload));
  __builtin___clear_cache(reinterpret_cast<char*>(mapping.entry),reinterpret_cast<char*>(mapping.entry+14));
  if(mprotect(reinterpret_cast<void*>(first),last-first,PROT_READ|PROT_EXEC)){
   // Do not leave the executable page writable or nonexecutable after failure.
   std::memcpy(reinterpret_cast<void*>(mapping.entry),expected,sizeof(expected));
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
