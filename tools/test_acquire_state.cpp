#include "../shim/acquire_state.hpp"
#include <cassert>
#include <cstdio>
struct Clock {
 using duration=std::chrono::nanoseconds;using time_point=std::chrono::time_point<Clock>;
 static duration elapsed;static time_point now(){return time_point(elapsed);}
};
Clock::duration Clock::elapsed{};
int main(){
 AcquireState<Clock> state;uint32_t index=99;unsigned acquisitions=0,waits=0,resets=0;
 auto acquire=[&](uint64_t budget,uint32_t*out){assert(budget==100);acquisitions++;*out=7;Clock::elapsed+=Clock::duration(80);return VK_SUBOPTIMAL_KHR;};
 auto timeout=[&](uint64_t budget){assert(budget==20);waits++;Clock::elapsed+=Clock::duration(20);return VK_TIMEOUT;};
 auto reset=[&](){resets++;return VK_SUCCESS;};
 assert(state.next(100,&index,acquire,timeout,reset)==VK_TIMEOUT);
 assert(index==99&&resets==0&&state.pending());
 auto ready=[&](uint64_t budget){assert(budget==100);waits++;return VK_SUCCESS;};
 assert(state.next(100,&index,acquire,ready,reset)==VK_SUBOPTIMAL_KHR);
 assert(index==7&&acquisitions==1&&waits==2&&resets==1&&!state.pending());
 AcquireState<Clock> empty;
 auto notReady=[&](uint64_t budget,uint32_t*){assert(budget==0);return VK_NOT_READY;};
 auto forbiddenWait=[&](uint64_t)->VkResult{assert(false);return VK_ERROR_UNKNOWN;};
 assert(empty.next(0,&index,notReady,forbiddenWait,reset)==VK_NOT_READY);
 const auto infinite=std::numeric_limits<uint64_t>::max();
 auto immediate=[&](uint64_t budget,uint32_t*out){assert(budget==infinite);*out=3;return VK_SUCCESS;};
 auto lost=[&](uint64_t budget){assert(budget==infinite);return VK_ERROR_DEVICE_LOST;};
 assert(empty.next(infinite,&index,immediate,lost,reset)==VK_ERROR_DEVICE_LOST);
 assert(resets==1&&index==7);
 puts("PASS: one timeout budget; pending image/fence retained across timeout; no duplicate acquire; result and infinite/zero timeout semantics preserved.");
}
