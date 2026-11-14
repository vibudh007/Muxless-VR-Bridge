#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// SteamVR's logical pool keeps its original handle. Actual sets always remain
// real Vulkan handles; only allocation/free/reset/destruction are routed.
class DescriptorArena {
public:
 struct Driver {
  VkDevice device;
  PFN_vkCreateDescriptorPool create;
  PFN_vkDestroyDescriptorPool destroy;
  PFN_vkAllocateDescriptorSets allocate;
  PFN_vkFreeDescriptorSets free;
  PFN_vkResetDescriptorPool reset;
 };
private:
 struct Child {VkDescriptorPool handle;uint32_t live;};
 struct Allocation {VkDescriptorSet set;VkDescriptorPool owner;};
 struct Root {
  std::mutex lock;
  VkDescriptorPool handle;
  VkDescriptorPoolCreateInfo info;
  std::vector<VkDescriptorPoolSize> sizes;
  bool hasAllocator;
  VkAllocationCallbacks allocator{};
  std::vector<Child> children;
  std::vector<Allocation> sets;
  const VkAllocationCallbacks* callbacks()const{return hasAllocator?&allocator:nullptr;}
 };
 Driver driver_;
 std::mutex registryLock_;
 std::unordered_map<VkDescriptorPool,std::shared_ptr<Root>> roots_;
 std::shared_ptr<Root> root(VkDescriptorPool pool){std::lock_guard<std::mutex>l(registryLock_);auto i=roots_.find(pool);return i==roots_.end()?nullptr:i->second;}
 static bool exhausted(VkResult r){return r==VK_ERROR_OUT_OF_POOL_MEMORY||r==VK_ERROR_FRAGMENTED_POOL;}
 VkResult allocateFrom(Root&r,Child&child,const VkDescriptorSetAllocateInfo&request,VkDescriptorSet*out){
  auto actual=request;actual.descriptorPool=child.handle;
  auto result=driver_.allocate(driver_.device,&actual,out);
  if(result==VK_SUCCESS){
   for(uint32_t i=0;i<request.descriptorSetCount;i++)r.sets.push_back({out[i],child.handle});
   child.live+=request.descriptorSetCount;
  }
  return result;
 }
public:
 explicit DescriptorArena(Driver driver):driver_(driver){}
 static bool matches(const VkDescriptorPoolCreateInfo&c){
  if(c.pNext||c.flags!=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT||c.maxSets!=200||c.poolSizeCount!=4)return false;
  std::array<uint32_t,4>counts{1600,800,3200,3200};std::array<bool,4>seen{};
  for(uint32_t i=0;i<4;i++){
   int j=-1;switch(c.pPoolSizes[i].type){case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:j=0;break;case VK_DESCRIPTOR_TYPE_SAMPLER:j=1;break;case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:j=2;break;case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:j=3;break;default:return false;}
   if(seen[j]||counts[j]!=c.pPoolSizes[i].descriptorCount)return false;
   seen[j]=true;
  }return true;
 }
 void track(VkDescriptorPool handle,const VkDescriptorPoolCreateInfo&info,const VkAllocationCallbacks*a){
  if(!matches(info))return;
  auto r=std::make_shared<Root>();r->handle=handle;r->info=info;r->sizes.assign(info.pPoolSizes,info.pPoolSizes+info.poolSizeCount);r->info.pPoolSizes=r->sizes.data();r->hasAllocator=a!=nullptr;if(a)r->allocator=*a;r->children.push_back({handle,0});
  std::lock_guard<std::mutex>l(registryLock_);roots_.emplace(handle,r);
  std::fprintf(stderr,"STEAMVR_ARENA registered root=%p capacity_per_pool=%u\n",(void*)handle,info.maxSets);
 }
 VkResult allocate(const VkDescriptorSetAllocateInfo*c,VkDescriptorSet*out){
  auto r=root(c->descriptorPool);if(!r)return driver_.allocate(driver_.device,c,out);
  std::lock_guard<std::mutex>l(r->lock);
  try{
   const auto needed=r->sets.size()+c->descriptorSetCount;
   if(needed>r->sets.capacity())r->sets.reserve(std::max(needed,std::max(size_t(32),r->sets.capacity()*2)));
   r->children.reserve(r->children.size()+1);
  }catch(const std::bad_alloc&){std::fill_n(out,c->descriptorSetCount,VK_NULL_HANDLE);return VK_ERROR_OUT_OF_HOST_MEMORY;}
  // Unknown chains pass through to the original pool, but successful sets
  // still need ownership tracking so a later mixed free remains correct.
  if(c->pNext||c->descriptorSetCount>r->info.maxSets)return allocateFrom(*r,r->children.front(),*c,out);
  for(auto&child:r->children){
   if(child.live>=r->info.maxSets||c->descriptorSetCount>r->info.maxSets-child.live)continue;
   auto result=allocateFrom(*r,child,*c,out);
   if(result==VK_SUCCESS)return result;
   if(!exhausted(result))return result;
  }
  VkDescriptorPool backing=VK_NULL_HANDLE;auto result=driver_.create(driver_.device,&r->info,r->callbacks(),&backing);
  if(result!=VK_SUCCESS){std::fill_n(out,c->descriptorSetCount,VK_NULL_HANDLE);return result;}
  Child child{backing,0};result=allocateFrom(*r,child,*c,out);
  if(result!=VK_SUCCESS){driver_.destroy(driver_.device,backing,r->callbacks());return result;}
  r->children.push_back(child);
  std::fprintf(stderr,"STEAMVR_ARENA grow root=%p backing=%p pools=%zu live_sets=%zu\n",(void*)r->handle,(void*)backing,r->children.size(),r->sets.size());
  return VK_SUCCESS;
 }
 VkResult free(VkDescriptorPool pool,uint32_t count,const VkDescriptorSet*sets){
  auto r=root(pool);if(!r)return driver_.free(driver_.device,pool,count,sets);
  std::lock_guard<std::mutex>l(r->lock);
  // Validate ownership before freeing anything. Null handles are allowed by Vulkan.
  for(uint32_t i=0;i<count;i++)if(sets[i]&&std::none_of(r->sets.begin(),r->sets.end(),[&](const Allocation&a){return a.set==sets[i];}))return driver_.free(driver_.device,pool,count,sets);
  for(uint32_t i=0;i<count;i++){
   if(!sets[i])continue;
   auto a=std::find_if(r->sets.begin(),r->sets.end(),[&](const Allocation&x){return x.set==sets[i];});
   if(a==r->sets.end())continue;
   auto result=driver_.free(driver_.device,a->owner,1,&sets[i]);if(result!=VK_SUCCESS)return result;
   auto child=std::find_if(r->children.begin(),r->children.end(),[&](const Child&x){return x.handle==a->owner;});child->live--;
   *a=r->sets.back();r->sets.pop_back();
  }
  for(auto i=r->children.begin()+1;i!=r->children.end();){
   if(i->live==0){std::fprintf(stderr,"STEAMVR_ARENA reclaim backing=%p\n",(void*)i->handle);driver_.destroy(driver_.device,i->handle,r->callbacks());i=r->children.erase(i);}else ++i;
  }
  return VK_SUCCESS;
 }
 VkResult reset(VkDescriptorPool pool,VkDescriptorPoolResetFlags flags){
  auto r=root(pool);if(!r)return driver_.reset(driver_.device,pool,flags);
  std::lock_guard<std::mutex>l(r->lock);
  auto result=driver_.reset(driver_.device,pool,flags);if(result!=VK_SUCCESS)return result;
  // The application's reset contract requires all pool uses to have retired.
  for(size_t i=1;i<r->children.size();i++)driver_.destroy(driver_.device,r->children[i].handle,r->callbacks());
  r->children.resize(1);r->children[0].live=0;r->sets.clear();return VK_SUCCESS;
 }
 void destroy(VkDescriptorPool pool,const VkAllocationCallbacks*a){
  auto r=root(pool);if(!r){driver_.destroy(driver_.device,pool,a);return;}
  {std::lock_guard<std::mutex>l(registryLock_);roots_.erase(pool);}
  std::lock_guard<std::mutex>l(r->lock);
  for(size_t i=1;i<r->children.size();i++)driver_.destroy(driver_.device,r->children[i].handle,r->callbacks());
  driver_.destroy(driver_.device,pool,a);
 }
 // VkDevice destruction implicitly destroys its children; do not issue stale calls.
 void forget(){std::lock_guard<std::mutex>l(registryLock_);roots_.clear();}
};
