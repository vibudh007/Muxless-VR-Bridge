#pragma once
#include <xf86drmMode.h>
#include <stdexcept>
// A commit copies request data during the ioctl. Reset the libdrm cursor only
// after that call returns; its property storage can then be reused unchanged.
class AtomicRequestBuffer {
 drmModeAtomicReq*request_=nullptr;
public:
 AtomicRequestBuffer()=default;
 AtomicRequestBuffer(const AtomicRequestBuffer&)=delete;
 AtomicRequestBuffer&operator=(const AtomicRequestBuffer&)=delete;
 drmModeAtomicReq*begin(){
  if(!request_){request_=drmModeAtomicAlloc();if(!request_)throw std::runtime_error("KMS request allocation failed");}
  drmModeAtomicSetCursor(request_,0);return request_;
 }
 ~AtomicRequestBuffer(){if(request_)drmModeAtomicFree(request_);}
};
