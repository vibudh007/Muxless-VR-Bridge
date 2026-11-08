#pragma once
#include "function_timing.hpp"
#include <array>
// Opt-in observation only: original arguments/results always reach the driver.
// Fixed recent-event ring + bounded metadata, dumped before an invalid write.
namespace descriptorDiagnostic {
struct Event {
 uint64_t serial=0,ns=0;const char*op="";VkResult result=VK_SUCCESS;
 VkDevice device=VK_NULL_HANDLE;VkDescriptorPool pool=VK_NULL_HANDLE;
 VkDescriptorSet set=VK_NULL_HANDLE;uint32_t count=0,binding=0;
};
struct Set {VkDescriptorPool pool;bool live;};
struct State {
 std::mutex mutex;std::array<Event,128>recent{};uint64_t serial=0;
 unsigned dumps=0,failures=0;std::unordered_map<VkDescriptorSet,Set>sets;
};
static std::mutex statesMutex;
static std::unordered_map<VkDevice,std::shared_ptr<State>>states;
static bool enabled(){static const bool value=bridgeConfigBool("descriptor_diagnostics");return value;}
static std::shared_ptr<State>state(VkDevice device){
 std::lock_guard<std::mutex>lock(statesMutex);auto&value=states[device];if(!value)value=std::make_shared<State>();return value;
}
static void event(State&s,const char*op,VkDevice d,VkDescriptorPool pool,VkDescriptorSet set,uint32_t count,VkResult result=VK_SUCCESS,uint32_t binding=0){
 Event e;e.serial=++s.serial;e.ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();e.op=op;e.device=d;e.pool=pool;e.set=set;e.count=count;e.result=result;e.binding=binding;s.recent[(e.serial-1)%s.recent.size()]=e;
}
static void dump(State&s,const char*reason){
 if(s.dumps++>=4)return;
 log("DESCRIPTOR_DUMP reason=%s serial=%llu tracked_sets=%zu",reason,(unsigned long long)s.serial,s.sets.size());
 uint64_t begin=s.serial>s.recent.size()?s.serial-s.recent.size()+1:1;
 for(uint64_t i=begin;i<=s.serial;i++){
  const auto&e=s.recent[(i-1)%s.recent.size()];
  log("DESCRIPTOR_EVENT serial=%llu ns=%llu op=%s device=%p pool=%p set=%p count=%u binding=%u result=%d",(unsigned long long)e.serial,(unsigned long long)e.ns,e.op,(void*)e.device,(void*)e.pool,(void*)e.set,e.count,e.binding,e.result);
 }
}
static VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice d,const VkDescriptorPoolCreateInfo*ci,const VkAllocationCallbacks*a,VkDescriptorPool*out){ WMR_TIMING_SCOPE("descriptor::createPool");
 auto ds=dstate(d);auto fn=(PFN_vkCreateDescriptorPool)ds->gdpa(d,"vkCreateDescriptorPool");
 auto r=fn(d,ci,a,out);
 auto s=state(d);std::lock_guard<std::mutex>lock(s->mutex);event(*s,"create-pool",d,r==VK_SUCCESS?*out:VK_NULL_HANDLE,VK_NULL_HANDLE,ci->maxSets,r);
 // Setup/error only: capacity per type makes exhaustion evidence interpretable.
 if(s->serial<=128||r!=VK_SUCCESS){log("DESCRIPTOR_POOL result=%d pool=%p max_sets=%u flags=0x%x",r,(void*)(r==VK_SUCCESS?*out:VK_NULL_HANDLE),ci->maxSets,ci->flags);for(uint32_t i=0;i<std::min(ci->poolSizeCount,32u);i++)log("DESCRIPTOR_CAPACITY type=%d count=%u",ci->pPoolSizes[i].type,ci->pPoolSizes[i].descriptorCount);}
 return r;
}
static VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice d,VkDescriptorPool pool,const VkAllocationCallbacks*a){ WMR_TIMING_SCOPE("descriptor::destroyPool");
 auto s=state(d);{std::lock_guard<std::mutex>lock(s->mutex);event(*s,"destroy-pool",d,pool,VK_NULL_HANDLE,0);for(auto&entry:s->sets)if(entry.second.pool==pool)entry.second.live=false;}
 auto ds=dstate(d);((PFN_vkDestroyDescriptorPool)ds->gdpa(d,"vkDestroyDescriptorPool"))(d,pool,a);
}
static VKAPI_ATTR VkResult VKAPI_CALL resetPool(VkDevice d,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags){ WMR_TIMING_SCOPE("descriptor::resetPool");
 auto ds=dstate(d);auto r=((PFN_vkResetDescriptorPool)ds->gdpa(d,"vkResetDescriptorPool"))(d,pool,flags);
 auto s=state(d);std::lock_guard<std::mutex>lock(s->mutex);event(*s,"reset-pool",d,pool,VK_NULL_HANDLE,0,r);if(r==VK_SUCCESS)for(auto&entry:s->sets)if(entry.second.pool==pool)entry.second.live=false;return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice d,const VkDescriptorSetAllocateInfo*ci,VkDescriptorSet*out){ WMR_TIMING_SCOPE("descriptor::allocate");
 auto ds=dstate(d);auto r=((PFN_vkAllocateDescriptorSets)ds->gdpa(d,"vkAllocateDescriptorSets"))(d,ci,out);
 auto s=state(d);std::lock_guard<std::mutex>lock(s->mutex);event(*s,"allocate",d,ci->descriptorPool,VK_NULL_HANDLE,ci->descriptorSetCount,r);
 if(r==VK_SUCCESS){for(uint32_t i=0;i<ci->descriptorSetCount;i++){if(s->sets.size()>=4096&&!s->sets.count(out[i])){s->sets.clear();log("DESCRIPTOR_METADATA reset capacity=4096; unknown handles are not proof of invalidity");}s->sets[out[i]]={ci->descriptorPool,true};event(*s,"allocated-set",d,ci->descriptorPool,out[i],1);}}
 else if(s->failures++<16){log("DESCRIPTOR_ALLOC_FAILED device=%p pool=%p count=%u result=%d",(void*)d,(void*)ci->descriptorPool,ci->descriptorSetCount,r);dump(*s,"allocation-failure");}
 return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL freeSets(VkDevice d,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet*sets){ WMR_TIMING_SCOPE("descriptor::freeSets");
 auto ds=dstate(d);auto r=((PFN_vkFreeDescriptorSets)ds->gdpa(d,"vkFreeDescriptorSets"))(d,pool,count,sets);
 auto s=state(d);std::lock_guard<std::mutex>lock(s->mutex);for(uint32_t i=0;i<count;i++){event(*s,"free-set",d,pool,sets[i],1,r);auto it=s->sets.find(sets[i]);if(r==VK_SUCCESS&&it!=s->sets.end())it->second.live=false;}return r;
}
static VKAPI_ATTR void VKAPI_CALL update(VkDevice d,uint32_t count,const VkWriteDescriptorSet*writes,uint32_t copies,const VkCopyDescriptorSet*copy){ WMR_TIMING_SCOPE("descriptor::update");
 auto s=state(d);{std::lock_guard<std::mutex>lock(s->mutex);
  for(uint32_t i=0;i<count;i++){
   auto it=s->sets.find(writes[i].dstSet);event(*s,"write",d,it==s->sets.end()?VK_NULL_HANDLE:it->second.pool,writes[i].dstSet,writes[i].descriptorCount,VK_SUCCESS,writes[i].dstBinding);
   if(!writes[i].dstSet||(it!=s->sets.end()&&!it->second.live))dump(*s,!writes[i].dstSet?"null-destination-set":"retired-destination-set");
  }
  for(uint32_t i=0;i<copies;i++){event(*s,"copy",d,VK_NULL_HANDLE,copy[i].dstSet,copy[i].descriptorCount);if(!copy[i].srcSet||!copy[i].dstSet)dump(*s,"null-copy-set");}
 }
 auto ds=dstate(d);((PFN_vkUpdateDescriptorSets)ds->gdpa(d,"vkUpdateDescriptorSets"))(d,count,writes,copies,copy);
}
static void cleanup(VkDevice d){std::lock_guard<std::mutex>lock(statesMutex);states.erase(d);}
static PFN_vkVoidFunction hook(const char*n){
 if(!enabled())return nullptr;
#define DESCRIPTOR_HOOK(name,fn) if(!strcmp(n,name))return (PFN_vkVoidFunction)fn
 DESCRIPTOR_HOOK("vkCreateDescriptorPool",createPool);DESCRIPTOR_HOOK("vkDestroyDescriptorPool",destroyPool);DESCRIPTOR_HOOK("vkResetDescriptorPool",resetPool);DESCRIPTOR_HOOK("vkAllocateDescriptorSets",allocate);DESCRIPTOR_HOOK("vkFreeDescriptorSets",freeSets);DESCRIPTOR_HOOK("vkUpdateDescriptorSets",update);
#undef DESCRIPTOR_HOOK
 return nullptr;
}
}
