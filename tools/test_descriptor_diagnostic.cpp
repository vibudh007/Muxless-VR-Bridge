#include <vulkan/vulkan.h>
#include <chrono>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cassert>
#include <algorithm>
static std::vector<std::string>logs;
static void log(const char*fmt,...){char text[1024];va_list a;va_start(a,fmt);vsnprintf(text,sizeof(text),fmt,a);va_end(a);logs.emplace_back(text);}
static bool bridgeConfigBool(const char*){return true;}
static uint32_t observedMax=0,observedCapacity=0;
static VkResult result=VK_SUCCESS;static unsigned updates=0;
static const VkWriteDescriptorSet*observed=nullptr;
static VkDescriptorPool pool=(VkDescriptorPool)(uintptr_t)10;
static VkDescriptorSet set=(VkDescriptorSet)(uintptr_t)20;
static VkResult VKAPI_CALL create(VkDevice,const VkDescriptorPoolCreateInfo*ci,const VkAllocationCallbacks*,VkDescriptorPool*out){observedMax=ci->maxSets;observedCapacity=ci->poolSizeCount?ci->pPoolSizes[0].descriptorCount:0;*out=pool;return result;}
static void VKAPI_CALL destroy(VkDevice,VkDescriptorPool,const VkAllocationCallbacks*){}
static VkResult VKAPI_CALL reset(VkDevice,VkDescriptorPool,VkDescriptorPoolResetFlags){return result;}
static VkResult VKAPI_CALL allocate(VkDevice,const VkDescriptorSetAllocateInfo*ci,VkDescriptorSet*out){for(uint32_t i=0;i<ci->descriptorSetCount;i++)out[i]=result==VK_SUCCESS?set:VK_NULL_HANDLE;return result;}
static VkResult VKAPI_CALL freeSets(VkDevice,VkDescriptorPool,uint32_t,const VkDescriptorSet*){return result;}
static void VKAPI_CALL update(VkDevice,uint32_t,const VkWriteDescriptorSet*w,uint32_t,const VkCopyDescriptorSet*){updates++;observed=w;}
static PFN_vkVoidFunction VKAPI_CALL proc(VkDevice,const char*n){
#define PROC(name,fn) if(!strcmp(n,name))return (PFN_vkVoidFunction)fn
 PROC("vkCreateDescriptorPool",create);PROC("vkDestroyDescriptorPool",destroy);PROC("vkResetDescriptorPool",reset);PROC("vkAllocateDescriptorSets",allocate);PROC("vkFreeDescriptorSets",freeSets);PROC("vkUpdateDescriptorSets",update);
 return nullptr;
}
struct Device {PFN_vkGetDeviceProcAddr gdpa=proc;};
static Device*dstate(VkDevice){static Device d;return &d;}
#include "../shim/descriptor_diagnostic.hpp"
int main(){
 namespace dd=descriptorDiagnostic;VkDevice d=(VkDevice)(uintptr_t)1;VkDescriptorPool out;
 VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};ci.maxSets=200;ci.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3200};ci.poolSizeCount=1;ci.pPoolSizes=&size;
 assert(dd::createPool(d,&ci,nullptr,&out)==VK_SUCCESS&&out==pool);
 assert(observedMax==200&&observedCapacity==3200&&ci.maxSets==200&&size.descriptorCount==3200);
 ci.pNext=&ci;dd::createPool(d,&ci,nullptr,&out);assert(observedMax==200&&observedCapacity==3200);ci.pNext=nullptr;
 ci.maxSets=UINT32_MAX;dd::createPool(d,&ci,nullptr,&out);assert(observedMax==UINT32_MAX);ci.maxSets=200;
 VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=pool;ai.descriptorSetCount=1;
 VkDescriptorSet allocated;assert(dd::allocate(d,&ai,&allocated)==VK_SUCCESS&&allocated==set);
 auto state=dd::state(d);assert(state->sets.at(set).live);
 VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.dstBinding=9;write.descriptorCount=1;
 dd::update(d,1,&write,0,nullptr);assert(observed==&write&&updates==1&&state->dumps==0);
 result=VK_ERROR_OUT_OF_POOL_MEMORY;
 assert(dd::resetPool(d,pool,0)==result&&state->sets.at(set).live);
 assert(dd::freeSets(d,pool,1,&set)==result&&state->sets.at(set).live);
 assert(dd::allocate(d,&ai,&allocated)==result&&allocated==VK_NULL_HANDLE);
 write.dstSet=allocated;dd::update(d,1,&write,0,nullptr);
 assert(updates==2&&observed==&write&&write.dstSet==VK_NULL_HANDLE); // Never mask/repair invalid input.
 bool failed=false,nullWrite=false;for(auto&line:logs){failed|=line.find("DESCRIPTOR_ALLOC_FAILED")!=std::string::npos;nullWrite|=line.find("null-destination-set")!=std::string::npos;}assert(failed&&nullWrite);
 result=VK_SUCCESS;assert(dd::resetPool(d,pool,0)==VK_SUCCESS&&!state->sets.at(set).live);
 dd::allocate(d,&ai,&allocated);assert(state->sets.at(set).live);
 dd::freeSets(d,pool,1,&set);assert(!state->sets.at(set).live);
 dd::allocate(d,&ai,&allocated);dd::destroyPool(d,pool,nullptr);assert(!state->sets.at(set).live);
 assert(dd::hook("vkUpdateDescriptorSets")==(PFN_vkVoidFunction)dd::update);
 assert(dd::hook("vkUnrelated")==nullptr);
 for(unsigned i=0;i<10000;i++)dd::update(d,1,&write,0,nullptr);
 assert(state->recent.size()==128&&state->sets.size()==1&&state->dumps>=4);
 auto count=logs.size();dd::update(d,1,&write,0,nullptr);assert(logs.size()==count);
 dd::cleanup(d);assert(dd::states.empty());
 puts("PASS: allocation failures and null writes retained; original arguments/results forwarded; pool reset/free/destroy lifetime tracked; ring/dump output bounded; cleanup removes device metadata.");
}
