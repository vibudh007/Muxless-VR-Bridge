from pathlib import Path
import xml.etree.ElementTree as E
root=E.parse('/usr/share/vulkan/registry/vk.xml').getroot()
names='''vkGetDrmDisplayEXT vkAcquireDrmDisplayEXT vkReleaseDisplayEXT vkGetPhysicalDeviceDisplayPropertiesKHR vkGetPhysicalDeviceDisplayProperties2KHR vkGetPhysicalDeviceDisplayPlanePropertiesKHR vkGetPhysicalDeviceDisplayPlaneProperties2KHR vkGetDisplayPlaneSupportedDisplaysKHR vkGetDisplayModePropertiesKHR vkGetDisplayModeProperties2KHR vkCreateDisplayModeKHR vkGetDisplayPlaneCapabilitiesKHR vkGetDisplayPlaneCapabilities2KHR vkCreateDisplayPlaneSurfaceKHR vkDestroySurfaceKHR vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilities2KHR vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormats2KHR vkGetPhysicalDeviceSurfacePresentModesKHR vkCreateSwapchainKHR vkDestroySwapchainKHR vkGetSwapchainImagesKHR vkAcquireNextImageKHR vkAcquireNextImage2KHR vkQueuePresentKHR vkQueueSubmit vkQueueSubmit2 vkRegisterDisplayEventEXT vkRegisterDeviceEventEXT vkGetSwapchainCounterEXT vkDisplayPowerControlEXT vkWaitForPresentKHR vkGetRefreshCycleDurationGOOGLE vkGetPastPresentationTimingGOOGLE vkGetDeviceQueue vkGetDeviceQueue2'''.split()
commands={c.findtext('proto/name'):c for c in root.findall('commands/command') if c.find('proto') is not None}
out=['// Generated forwarding-only trace wrappers.']
for name in names:
 c=commands[name];ret=c.findtext('proto/type');params=[p for p in c.findall('param') if 'vulkan' in p.get('api','vulkan').split(',')];decl=[ ''.join(p.itertext()).strip() for p in params];args=[p.findtext('name') for p in params];first=args[0];dev=params[0].findtext('type') in ['VkDevice','VkQueue','VkCommandBuffer'];state='dstate' if dev else 'istate';obj='s->handle';addr='s->gdpa' if dev else 's->gipa'
 out.append(f'static VKAPI_ATTR {ret} VKAPI_CALL trace_{name}('+','.join(decl)+'){')
 cached={'vkQueueSubmit':'queueSubmit','vkQueueSubmit2':'queueSubmit2'}.get(name)
 lookup=f'(PFN_{name}){addr}({obj},"{name}")'
 if cached:lookup=f's->{cached}?s->{cached}:'+lookup
 out.append(f'auto s={state}({first}); auto fn={lookup};')
 out.append(f'static std::atomic<unsigned> calls{{0}};bool emit=calls.load(std::memory_order_relaxed)<24&&calls.fetch_add(1,std::memory_order_relaxed)<24;if(emit)log("CALL {name}");')
 if name=='vkGetDrmDisplayEXT':out.append('struct stat st{};fstat(drmFd,&st);log("DRM_LOOKUP physical=%p fd=%d node=%u:%u connector=%u",(void*)physicalDevice,drmFd,major(st.st_rdev),minor(st.st_rdev),connectorId);')
 if name=='vkCreateSwapchainKHR':out.append('log("SWAPCHAIN surface=%p extent=%ux%u format=%d usage=0x%x images=%u mode=%d old=%p",(void*)pCreateInfo->surface,pCreateInfo->imageExtent.width,pCreateInfo->imageExtent.height,pCreateInfo->imageFormat,pCreateInfo->imageUsage,pCreateInfo->minImageCount,pCreateInfo->presentMode,(void*)pCreateInfo->oldSwapchain);for(auto*b=(const VkBaseInStructure*)pCreateInfo->pNext;b;b=b->pNext)log("SWAPCHAIN_PNEXT type=%d",b->sType);')
 if name=='vkQueuePresentKHR':out.append('if(emit){log("PRESENT queue=%p waits=%u swapchains=%u",(void*)queue,pPresentInfo->waitSemaphoreCount,pPresentInfo->swapchainCount);for(auto*b=(const VkBaseInStructure*)pPresentInfo->pNext;b;b=b->pNext)log("PRESENT_PNEXT type=%d",b->sType);}')
 if name=='vkQueueSubmit':out.append('if(emit)for(uint32_t i=0;i<submitCount;i++)log("SUBMIT waits=%u commands=%u signals=%u",pSubmits[i].waitSemaphoreCount,pSubmits[i].commandBufferCount,pSubmits[i].signalSemaphoreCount);')
 call=f'fn({",".join(args)})'
 if cached:call=f'WMR_PACING_CALL("Vulkan::{name}",{call})'
 out.append((f'auto r={call};if(emit)log("RESULT {name} %d",r);' if ret!='void' else call+';'))
 if name=='vkGetDrmDisplayEXT':out.append('log("DRM_LOOKUP_RESULT result=%d display=%p",r,(void*)*display);')
 if name=='vkGetDeviceQueue':out.append('{std::lock_guard<std::mutex>lock(s->queueFamiliesMutex);s->queueFamilies[*pQueue]=queueFamilyIndex;}log("QUEUE handle=%p family=%u index=%u",(void*)*pQueue,queueFamilyIndex,queueIndex);')
 if name=='vkGetDeviceQueue2':out.append('{std::lock_guard<std::mutex>lock(s->queueFamiliesMutex);s->queueFamilies[*pQueue]=pQueueInfo->queueFamilyIndex;}log("QUEUE2 handle=%p family=%u index=%u",(void*)*pQueue,pQueueInfo->queueFamilyIndex,pQueueInfo->queueIndex);')
 if name=='vkGetSwapchainImagesKHR':out.append('if(emit)log("SWAPCHAIN_IMAGES count=%u data=%p",*pSwapchainImageCount,(void*)pSwapchainImages);')
 if ret!='void':out.append('return r;')
 out.append('}')
out.append('static PFN_vkVoidFunction traceHook(const char*n){')
for n in names:out.append(f'if(!strcmp(n,"{n}"))return (PFN_vkVoidFunction)trace_{n};')
out.append('return nullptr;}')
p=Path(__file__).resolve().parent/'build';p.mkdir(exist_ok=True);(p/'trace-generated.h').write_text('\n'.join(out))
