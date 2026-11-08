#include "../shim/atomic_request.hpp"
#include <cassert>
#include <cstdio>
int main(){
 AtomicRequestBuffer storage;auto*first=storage.begin();
 for(unsigned epoch=0;epoch<10000;epoch++){
  auto*req=storage.begin();assert(req==first&&drmModeAtomicGetCursor(req)==0);
  for(unsigned i=0;i<14;i++)assert(drmModeAtomicAddProperty(req,10+i/4,100+i,uint64_t(epoch)*100+i)>=0);
  assert(drmModeAtomicGetCursor(req)==14);
 }
 assert(drmModeAtomicGetCursor(storage.begin())==0);
 puts("PASS: real libdrm request reused for10000 complete rebuilds; cursor resets discard prior properties and permit new values without reallocating the request object.");
}
