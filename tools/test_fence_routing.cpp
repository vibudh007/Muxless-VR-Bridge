// Actual wrappers, fake downstream Vulkan entry points: no GPU required.
#include <atomic>
#include <cstdlib>
#include <new>
static std::atomic<size_t> allocations{0};
void*operator new(size_t n){allocations++;if(auto*p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete(void*p)noexcept{std::free(p);}void operator delete(void*p,size_t)noexcept{std::free(p);}
#include "../shim/layer.cpp"
#include <cassert>
static const VkFence* observed=nullptr;static unsigned calls=0;static bool localReady=false,deviceLost=false;
static VkDevice outputDevice=(VkDevice)(uintptr_t)1;
extern "C" VkResult vkWaitForFences(VkDevice d,uint32_t,const VkFence*f,VkBool32,uint64_t){assert(d==outputDevice);observed=f;calls++;return VK_SUCCESS;}
extern "C" VkResult vkResetFences(VkDevice d,uint32_t,const VkFence*f){assert(d==outputDevice);observed=f;calls++;return VK_SUCCESS;}
extern "C" VkResult vkGetFenceStatus(VkDevice d,VkFence){assert(d==outputDevice);return deviceLost?VK_ERROR_DEVICE_LOST:VK_SUCCESS;}
static VkResult VKAPI_CALL waitLocal(VkDevice,uint32_t,const VkFence*f,VkBool32,uint64_t){observed=f;calls++;return VK_SUCCESS;}
static VkResult VKAPI_CALL resetLocal(VkDevice,uint32_t,const VkFence*f){observed=f;calls++;return VK_SUCCESS;}
static VkResult VKAPI_CALL statusLocal(VkDevice,VkFence){return localReady?VK_SUCCESS:VK_NOT_READY;}
static PFN_vkVoidFunction VKAPI_CALL proc(VkDevice,const char*n){
 if(!strcmp(n,"vkWaitForFences"))return (PFN_vkVoidFunction)waitLocal;
 if(!strcmp(n,"vkResetFences"))return (PFN_vkVoidFunction)resetLocal;
 if(!strcmp(n,"vkGetFenceStatus"))return (PFN_vkVoidFunction)statusLocal;return nullptr;
}
int main(){
 {BridgeOperation outer("outer");auto start=bridgeOperationStart.load();{BridgeOperation inner("inner");assert(!strcmp(bridgeOperation.load(),"inner"));}assert(!strcmp(bridgeOperation.load(),"outer")&&bridgeOperationStart.load()==start);}
 assert(!strcmp(bridgeOperation.load(),"idle"));
 int token;void*dispatch=&token;auto d=(VkDevice)&dispatch;
 auto state=std::make_shared<Device>();state->gdpa=proc;devices[&token]=state;
 Bridge owner;bridge=&owner;owner.output.device=outputDevice;
 VkFence remote=(VkFence)(uintptr_t)10,local=(VkFence)(uintptr_t)11;owner.outputFences.insert(remote);
 VkFence mixed[]={remote,local};
 auto chain=std::make_unique<ProxySwapchain>(&owner);VkSwapchainKHR swapchain=(VkSwapchainKHR)(uintptr_t)20;owner.chains[swapchain]=std::move(chain);
 VkAcquireNextImageInfoKHR info{VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR};info.swapchain=swapchain;info.deviceMask=2;uint32_t index;
 assert(bridgeAcquire2(d,&info,&index)==VK_ERROR_FEATURE_NOT_PRESENT);
 info.deviceMask=1;info.pNext=&info;assert(bridgeAcquire2(d,&info,&index)==VK_ERROR_FEATURE_NOT_PRESENT);
 for(auto*f:{&remote,&local}){
  auto before=allocations.load();
  for(unsigned i=0;i<10000;i++){
   assert(bridgeWaitFences(d,1,f,VK_TRUE,0)==VK_SUCCESS&&observed==f);
   assert(bridgeResetFences(d,1,f)==VK_SUCCESS&&observed==f);
  }
  assert(allocations.load()==before);
 }
 assert(bridgeWaitFences(d,2,mixed,VK_FALSE,0)==VK_SUCCESS);
 assert(bridgeWaitFences(d,2,mixed,VK_TRUE,0)==VK_TIMEOUT);
 localReady=true;assert(bridgeWaitFences(d,2,mixed,VK_TRUE,0)==VK_SUCCESS);
 localReady=false;deviceLost=true;assert(bridgeWaitFences(d,2,mixed,VK_TRUE,0)==VK_ERROR_DEVICE_LOST);
 deviceLost=false;assert(bridgeResetFences(d,2,mixed)==VK_SUCCESS);
 auto before=allocations.load();assert(bridgeResetFences(d,2,mixed)==VK_SUCCESS);assert(before==allocations.load());
 owner.output.device=VK_NULL_HANDLE;bridge=nullptr;devices.clear();
 puts("PASS: local/output wait/reset use original arrays with zero allocations; mixed any/all/timeout/device-error routing preserved; mixed reset scratch reused.");
}
