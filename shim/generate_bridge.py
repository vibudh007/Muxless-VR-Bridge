from pathlib import Path
import xml.etree.ElementTree as E
root=E.parse('/usr/share/vulkan/registry/vk.xml').getroot()
commands={c.findtext('proto/name'):c for c in root.findall('commands/command') if c.find('proto') is not None}
names='''vkGetPhysicalDeviceDisplayPropertiesKHR vkGetPhysicalDeviceDisplayProperties2KHR vkGetPhysicalDeviceDisplayPlanePropertiesKHR vkGetPhysicalDeviceDisplayPlaneProperties2KHR vkGetDisplayPlaneSupportedDisplaysKHR vkGetDisplayModePropertiesKHR vkGetDisplayModeProperties2KHR vkCreateDisplayModeKHR vkGetDisplayPlaneCapabilitiesKHR vkGetDisplayPlaneCapabilities2KHR vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilities2KHR vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormats2KHR vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfaceCapabilities2EXT vkReleaseDisplayEXT'''.split()
out=['// Generated bridge dispatch for display queries: application GPU remains unchanged.']
for name in names:
 c=commands[name];ret=c.findtext('proto/type');params=[p for p in c.findall('param') if 'vulkan' in p.get('api','vulkan').split(',')];decl=[''.join(p.itertext()).strip() for p in params];args=[p.findtext('name') for p in params];first=args[0]
 out.append(f'static VKAPI_ATTR {ret} VKAPI_CALL bridge_{name}('+','.join(decl)+'){')
 cond='bridge&&physicalDevice==bridge->publicPhysical'
 if 'Surface' in name:
  surface='pSurfaceInfo->surface' if 'pSurfaceInfo' in args else 'surface'
  cond+=f'&&bridgedSurface({surface})'
 elif name=='vkReleaseDisplayEXT':cond+='&&display==bridge->display'
 alt=['bridge->output.physical']+args[1:]
 out.append('if('+cond+'){')
 out.append(f'auto fn=(PFN_{name})vkGetInstanceProcAddr(bridge->instance,"{name}");')
 out.append(f'auto r=fn({",".join(alt)});log("BRIDGE_QUERY {name} result=%d",r);return r;' if ret!='void' else f'fn({",".join(alt)});return;')
 out.append('}')
 out.append(f'auto s=istate({first});auto fn=(PFN_{name})s->gipa(s->handle,"{name}");')
 out.append(('return ' if ret!='void' else '')+f'fn({",".join(args)});'+'}')
out.append('static PFN_vkVoidFunction bridgeGeneratedHook(const char*n){')
for name in names:out.append(f'if(!strcmp(n,"{name}"))return (PFN_vkVoidFunction)bridge_{name};')
out.append('return nullptr;}')
(Path(__file__).resolve().parent/'build/bridge-generated.h').write_text('\n'.join(out))
