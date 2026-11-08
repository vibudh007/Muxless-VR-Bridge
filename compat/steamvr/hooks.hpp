#pragma once

static bool steamVrShaderEnabled(){
 const char* disabled=getenv("DISABLE_WMR_STEAMVR_COMPAT");
 if(bridgeCreatingPrivateInstance||(disabled&&!strcmp(disabled,"1"))||!bridgeExplicitlyEnabled())return false;
 static const bool compositor=[](){
  char executable[4096];
  auto size=readlink("/proc/self/exe",executable,sizeof(executable)-1);
  if(size<=0||size>=static_cast<ssize_t>(sizeof(executable)-1))return false;
  executable[size]=0;
  auto* slash=strrchr(executable,'/');
  return slash&&!strcmp(slash+1,"vrcompositor");
 }();
 return compositor;
}

namespace steamVrShader {
#include "unlit.hpp"

static PFN_vkVoidFunction hook(const char* name){
#define HOOK(vk, fn) if(!strcmp(name,vk))return reinterpret_cast<PFN_vkVoidFunction>(fn)
 HOOK("vkCreateDescriptorSetLayout",diagnosticCreateSetLayout);
 HOOK("vkDestroyDescriptorSetLayout",diagnosticDestroySetLayout);
 HOOK("vkCreatePipelineLayout",diagnosticCreatePipelineLayout);
 HOOK("vkDestroyPipelineLayout",diagnosticDestroyPipelineLayout);
 HOOK("vkCreateShaderModule",diagnosticCreateShader);
 HOOK("vkDestroyShaderModule",diagnosticDestroyShader);
 HOOK("vkCreateGraphicsPipelines",diagnosticCreateGraphics);
#undef HOOK
 return nullptr;
}
}
