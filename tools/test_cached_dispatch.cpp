#include "../shim/layer.cpp"
#include <cassert>
static unsigned lookups=0,barriers=0,waits=0,resets=0,statuses=0,submits=0;
static VkResult VKAPI_CALL wait(VkDevice,uint32_t count,const VkFence*f,VkBool32 all,uint64_t timeout){assert(count==1&&f&&all&&timeout==99);waits++;return VK_TIMEOUT;}
static VkResult VKAPI_CALL reset(VkDevice,uint32_t count,const VkFence*f){assert(count==1&&f);resets++;return VK_ERROR_DEVICE_LOST;}
static VkResult VKAPI_CALL status(VkDevice,VkFence){statuses++;return VK_NOT_READY;}
static void VKAPI_CALL barrier(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t count,const VkImageMemoryBarrier*images){assert(count==0&&images==nullptr);barriers++;}
static VkResult VKAPI_CALL submit(VkQueue,uint32_t count,const VkSubmitInfo*info,VkFence fence){assert(count==0&&info==nullptr&&!fence);submits++;return VK_ERROR_DEVICE_LOST;}
static PFN_vkVoidFunction VKAPI_CALL forbidden(VkDevice,const char*){lookups++;assert(false);return nullptr;}
int main(){
 char tmp[]="/tmp/wmr-dispatch-XXXXXX";assert(mkdtemp(tmp));setenv("WMR_BRIDGE_LOG_DIR",tmp,1);setenv("XDG_CONFIG_HOME",tmp,1);
 int token;void*dispatch=&token;auto d=(VkDevice)&dispatch;auto q=(VkQueue)&dispatch;auto cb=(VkCommandBuffer)&dispatch;
 auto s=std::make_shared<Device>();s->handle=d;s->gdpa=forbidden;s->pipelineBarrier=barrier;s->waitFences=wait;s->resetFences=reset;s->fenceStatus=status;s->queueSubmit=submit;devices[&token]=s;
 VkFence fence=(VkFence)(uintptr_t)9;
 for(unsigned i=0;i<1000;i++){
  bridgeBarrier(cb,0,0,0,0,nullptr,0,nullptr,0,nullptr);
  assert(bridgeWaitFences(d,1,&fence,VK_TRUE,99)==VK_TIMEOUT);
  assert(bridgeResetFences(d,1,&fence)==VK_ERROR_DEVICE_LOST);
  assert(bridgeFenceStatus(d,fence)==VK_NOT_READY);
  assert(trace_vkQueueSubmit(q,0,nullptr,VK_NULL_HANDLE)==VK_ERROR_DEVICE_LOST);
 }
 assert(lookups==0&&barriers==1000&&waits==1000&&resets==1000&&statuses==1000&&submits==1000);
 devices.clear();std::filesystem::remove_all(tmp);puts("PASS: actual layer wrappers preserve arguments/results for5000calls without downstream function lookups.");
}
