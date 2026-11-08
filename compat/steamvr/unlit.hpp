#pragma once
// Guarded compatibility path for the inspected SteamVR shader and invalid pipeline layout.
// No vendor shader files are changed. Other shaders and valid layouts pass through.
// The exact stock SteamVR 2.19.1 shader was inspected and validated locally.
static uint64_t diagnosticShaderHash(const void*data,size_t bytes){
 auto*p=(const unsigned char*)data;uint64_t hash=14695981039346656037ull;
 for(size_t i=0;i<bytes;i++){hash=(hash^p[i])*1099511628211ull;}
 return hash;
}
static bool diagnosticUnlitVariant(const VkShaderModuleCreateInfo*ci,std::vector<uint32_t>&words){
 // Exact content fingerprint, followed by structural guards. Fail closed on updates.
 if(ci->codeSize!=1588||diagnosticShaderHash(ci->pCode,ci->codeSize)!=0x6390e7ebbbf65c18ull)return false;
 words.assign(ci->pCode,ci->pCode+ci->codeSize/4);
 size_t function=0,load=0;uint32_t type=0;bool layer=false,uboBinding=false,uboSet=false,accessChain=false,store=false;
 for(size_t pos=5;pos<words.size();){uint32_t count=words[pos]>>16,op=words[pos]&65535;if(!count||pos+count>words.size())return false;
  if(op==71&&count==4){layer|=words[pos+1]==71&&words[pos+2]==11&&words[pos+3]==9;uboBinding|=words[pos+1]==49&&words[pos+2]==33&&words[pos+3]==0;uboSet|=words[pos+1]==49&&words[pos+2]==34&&words[pos+3]==0;}
  if(op==65&&count==5&&words[pos+2]==107&&words[pos+3]==49)accessChain=true;
  if(op==61&&count==4&&words[pos+2]==108&&words[pos+3]==107){load=pos;type=words[pos+1];}
  if(op==62&&count==3&&words[pos+1]==71&&words[pos+2]==108)store=true;
  if(op==54&&!function)function=pos;
  pos+=count;
 }
 if(!layer||!uboBinding||!uboSet||!accessChain||!store||!function||!load||words[3]!=145)return false;
 uint32_t zero=words[3]++;words[load]=(4u<<16)|83;words[load+3]=zero;
 words.insert(words.begin()+function,{(4u<<16)|43,type,zero,0});return true;
}

struct DiagnosticShaderDevice {
 std::unordered_map<VkDescriptorSetLayout,bool> vertexUniform;
 std::unordered_map<VkPipelineLayout,bool> pipelineUniform;
 std::unordered_map<VkShaderModule,VkShaderModule> variants;
};
static std::mutex diagnosticShaderMutex;
static std::unordered_map<VkDevice,DiagnosticShaderDevice> diagnosticShaderDevices;
static VKAPI_ATTR VkResult VKAPI_CALL diagnosticCreateSetLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo*ci,const VkAllocationCallbacks*a,VkDescriptorSetLayout*out){
 auto s=dstate(d);auto fn=(PFN_vkCreateDescriptorSetLayout)s->gdpa(d,"vkCreateDescriptorSetLayout");auto result=fn(d,ci,a,out);
 if(result==VK_SUCCESS){
  bool uniform=false;for(uint32_t i=0;i<ci->bindingCount;i++){const auto&b=ci->pBindings[i];if(b.binding==0&&b.descriptorCount&&(b.stageFlags&VK_SHADER_STAGE_VERTEX_BIT)&&(b.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||b.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC))uniform=true;}
  std::lock_guard<std::mutex>lock(diagnosticShaderMutex);diagnosticShaderDevices[d].vertexUniform[*out]=uniform;
 }return result;
}
static VKAPI_ATTR void VKAPI_CALL diagnosticDestroySetLayout(VkDevice d,VkDescriptorSetLayout layout,const VkAllocationCallbacks*a){
 {std::lock_guard<std::mutex>lock(diagnosticShaderMutex);auto it=diagnosticShaderDevices.find(d);if(it!=diagnosticShaderDevices.end())it->second.vertexUniform.erase(layout);}
 auto s=dstate(d);((PFN_vkDestroyDescriptorSetLayout)s->gdpa(d,"vkDestroyDescriptorSetLayout"))(d,layout,a);
}
static VKAPI_ATTR VkResult VKAPI_CALL diagnosticCreatePipelineLayout(VkDevice d,const VkPipelineLayoutCreateInfo*ci,const VkAllocationCallbacks*a,VkPipelineLayout*out){
 auto s=dstate(d);auto result=((PFN_vkCreatePipelineLayout)s->gdpa(d,"vkCreatePipelineLayout"))(d,ci,a,out);
 if(result==VK_SUCCESS){
  std::lock_guard<std::mutex>lock(diagnosticShaderMutex);auto&state=diagnosticShaderDevices[d];
  if(!ci->setLayoutCount)state.pipelineUniform[*out]=false;
  else {auto it=state.vertexUniform.find(ci->pSetLayouts[0]);if(it!=state.vertexUniform.end())state.pipelineUniform[*out]=it->second;}
 }return result;
}
static VKAPI_ATTR void VKAPI_CALL diagnosticDestroyPipelineLayout(VkDevice d,VkPipelineLayout layout,const VkAllocationCallbacks*a){
 {std::lock_guard<std::mutex>lock(diagnosticShaderMutex);auto it=diagnosticShaderDevices.find(d);if(it!=diagnosticShaderDevices.end())it->second.pipelineUniform.erase(layout);}
 auto s=dstate(d);((PFN_vkDestroyPipelineLayout)s->gdpa(d,"vkDestroyPipelineLayout"))(d,layout,a);
}
static VKAPI_ATTR VkResult VKAPI_CALL diagnosticCreateShader(VkDevice d,const VkShaderModuleCreateInfo*ci,const VkAllocationCallbacks*a,VkShaderModule*out){
 auto s=dstate(d);auto fn=(PFN_vkCreateShaderModule)s->gdpa(d,"vkCreateShaderModule");auto result=fn(d,ci,a,out);if(result!=VK_SUCCESS)return result;
 std::vector<uint32_t>words;
 if(diagnosticUnlitVariant(ci,words)){
  VkShaderModuleCreateInfo local=*ci;local.codeSize=words.size()*4;local.pCode=words.data();VkShaderModule variant=VK_NULL_HANDLE;
  auto secondary=fn(d,&local,a,&variant);
  log("COMPAT_UNLIT_VARIANT prepared stock=%p variant=%p result=%d",(void*)*out,(void*)variant,secondary);
  if(secondary==VK_SUCCESS){std::lock_guard<std::mutex>lock(diagnosticShaderMutex);diagnosticShaderDevices[d].variants[*out]=variant;}
 }return result;
}
static VKAPI_ATTR void VKAPI_CALL diagnosticDestroyShader(VkDevice d,VkShaderModule shader,const VkAllocationCallbacks*a){
 VkShaderModule variant=VK_NULL_HANDLE;
 {std::lock_guard<std::mutex>lock(diagnosticShaderMutex);auto state=diagnosticShaderDevices.find(d);if(state!=diagnosticShaderDevices.end()){auto&variants=state->second.variants;auto it=variants.find(shader);if(it!=variants.end()){variant=it->second;variants.erase(it);}}}
 auto s=dstate(d);auto fn=(PFN_vkDestroyShaderModule)s->gdpa(d,"vkDestroyShaderModule");if(variant)fn(d,variant,a);fn(d,shader,a);
}
static VKAPI_ATTR VkResult VKAPI_CALL diagnosticCreateGraphics(VkDevice d,VkPipelineCache cache,uint32_t count,const VkGraphicsPipelineCreateInfo*ci,const VkAllocationCallbacks*a,VkPipeline*out){
 auto s=dstate(d);auto fn=(PFN_vkCreateGraphicsPipelines)s->gdpa(d,"vkCreateGraphicsPipelines");
 std::vector<VkGraphicsPipelineCreateInfo>infos;std::vector<std::vector<VkPipelineShaderStageCreateInfo>>stages;bool changed=false;
 {std::lock_guard<std::mutex>lock(diagnosticShaderMutex);auto&state=diagnosticShaderDevices[d];
  for(uint32_t i=0;i<count;i++){
   auto layout=state.pipelineUniform.find(ci[i].layout);if(layout==state.pipelineUniform.end())continue;
   for(uint32_t j=0;j<ci[i].stageCount;j++){
    const auto&stage=ci[i].pStages[j];auto variant=state.variants.find(stage.module);
    if(stage.stage!=VK_SHADER_STAGE_VERTEX_BIT||variant==state.variants.end())continue;
    if(layout->second){log("COMPAT_UNLIT_PIPELINE original_valid_ubo layout=%p",(void*)ci[i].layout);continue;}
    if(infos.empty()){infos.assign(ci,ci+count);stages.resize(count);}
    if(stages[i].empty())stages[i].assign(ci[i].pStages,ci[i].pStages+ci[i].stageCount);
    stages[i][j].module=variant->second;infos[i].pStages=stages[i].data();changed=true;
    log("COMPAT_UNLIT_PIPELINE replaced_missing_vertex_ubo layout=%p stock=%p variant=%p",(void*)ci[i].layout,(void*)stage.module,(void*)variant->second);
   }
  }
 }
 return fn(d,cache,count,changed?infos.data():ci,a,out);
}
static void diagnosticDeviceCleanup(VkDevice d){std::lock_guard<std::mutex>lock(diagnosticShaderMutex);diagnosticShaderDevices.erase(d);}
