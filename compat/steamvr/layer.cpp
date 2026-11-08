// Optional SteamVR compatibility layer. Independent of the GPU bridge.
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace {
struct Instance {
    VkInstance handle;
    PFN_vkGetInstanceProcAddr gipa;
    bool active;
};
struct Device {
    PFN_vkGetDeviceProcAddr gdpa;
    bool active;
};
std::mutex mapsMutex;
std::unordered_map<const void*, std::shared_ptr<Instance>> instances;
std::unordered_map<VkDevice, std::shared_ptr<Device>> devices;

const void* dispatchKey(const void* handle) {
    return handle ? *static_cast<const void* const*>(handle) : nullptr;
}
std::shared_ptr<Instance> instanceState(const void* handle) {
    std::lock_guard<std::mutex> lock(mapsMutex);
    auto it = instances.find(dispatchKey(handle));
    return it != instances.end() ? it->second : nullptr;
}
std::shared_ptr<Device> dstate(VkDevice device) {
    std::lock_guard<std::mutex> lock(mapsMutex);
    auto it = devices.find(device);
    return it != devices.end() ? it->second : nullptr;
}
void log(const char* format, ...) {
    std::fputs("STEAMVR_SHADER_COMPAT ", stderr);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}
bool isSteamVrCompositor() {
    char executable[4096];
    auto size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size <= 0) return false;
    executable[size] = 0;
    auto* slash = std::strrchr(executable, '/');
    return slash && !std::strcmp(slash + 1, "vrcompositor");
}

#include "unlit.hpp"

VKAPI_ATTR VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo* info,
                                              const VkAllocationCallbacks* allocator,
                                              VkInstance* out) {
    auto* link = reinterpret_cast<const VkLayerInstanceCreateInfo*>(info->pNext);
    for (; link; link = reinterpret_cast<const VkLayerInstanceCreateInfo*>(link->pNext)) {
        if (link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO) break;
    }
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    auto next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const_cast<VkLayerInstanceCreateInfo*>(link)->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(next(nullptr, "vkCreateInstance"));
    auto result = create(info, allocator, out);
    if (result == VK_SUCCESS) {
        auto state = std::make_shared<Instance>(Instance{*out, next, isSteamVrCompositor()});
        std::lock_guard<std::mutex> lock(mapsMutex);
        instances[dispatchKey(*out)] = std::move(state);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance handle, const VkAllocationCallbacks* allocator) {
    auto state = instanceState(handle);
    if (!state) return;
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(state->gipa(handle, "vkDestroyInstance"));
    {
        std::lock_guard<std::mutex> lock(mapsMutex);
        instances.erase(dispatchKey(handle));
    }
    destroy(handle, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                            const VkAllocationCallbacks* allocator, VkDevice* out) {
    auto instance = instanceState(physical);
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    auto* link = reinterpret_cast<const VkLayerDeviceCreateInfo*>(info->pNext);
    for (; link; link = reinterpret_cast<const VkLayerDeviceCreateInfo*>(link->pNext)) {
        if (link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO) break;
    }
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    auto gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const_cast<VkLayerDeviceCreateInfo*>(link)->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance->handle, "vkCreateDevice"));
    auto result = create(physical, info, allocator, out);
    if (result == VK_SUCCESS) {
        auto state = std::make_shared<Device>(Device{gdpa, instance->active});
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices[*out] = std::move(state);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice handle, const VkAllocationCallbacks* allocator) {
    auto state = dstate(handle);
    if (!state) return;
    diagnosticDeviceCleanup(handle);
    auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(state->gdpa(handle, "vkDestroyDevice"));
    {
        std::lock_guard<std::mutex> lock(mapsMutex);
        devices.erase(handle);
    }
    destroy(handle, allocator);
}

PFN_vkVoidFunction shaderHook(const char* name) {
#define HOOK(vk, fn) if (!std::strcmp(name, vk)) return reinterpret_cast<PFN_vkVoidFunction>(fn)
    HOOK("vkCreateDescriptorSetLayout", diagnosticCreateSetLayout);
    HOOK("vkDestroyDescriptorSetLayout", diagnosticDestroySetLayout);
    HOOK("vkCreatePipelineLayout", diagnosticCreatePipelineLayout);
    HOOK("vkDestroyPipelineLayout", diagnosticDestroyPipelineLayout);
    HOOK("vkCreateShaderModule", diagnosticCreateShader);
    HOOK("vkDestroyShaderModule", diagnosticDestroyShader);
    HOOK("vkCreateGraphicsPipelines", diagnosticCreateGraphics);
#undef HOOK
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice device, const char* name) {
    if (!std::strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(gdpa);
    auto state = dstate(device);
    if (!state) return nullptr;
    if (!std::strcmp(name, "vkDestroyDevice")) return reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
    if (state->active) if (auto hook = shaderHook(name)) return hook;
    return state->gdpa(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gipa(VkInstance instance, const char* name) {
#define HOOK(vk, fn) if (!std::strcmp(name, vk)) return reinterpret_cast<PFN_vkVoidFunction>(fn)
    HOOK("vkGetInstanceProcAddr", gipa);
    HOOK("vkCreateInstance", createInstance);
    if (!instance) return nullptr;
    HOOK("vkGetDeviceProcAddr", gdpa);
    HOOK("vkDestroyInstance", destroyInstance);
    HOOK("vkCreateDevice", createDevice);
    HOOK("vkDestroyDevice", destroyDevice);
#undef HOOK
    auto state = instanceState(instance);
    if (!state) return nullptr;
    if (state->active) if (auto hook = shaderHook(name)) return hook;
    return state->gipa(instance, name);
}
}

extern "C" __attribute__((visibility("default")))
VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* info) {
    if (!info || info->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || info->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    info->loaderLayerInterfaceVersion = 2;
    info->pfnGetInstanceProcAddr = gipa;
    info->pfnGetDeviceProcAddr = gdpa;
    info->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
