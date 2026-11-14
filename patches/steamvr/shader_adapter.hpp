#pragma once
// SVR-002: native shader/layout construction only. No frame hooks or logging.
namespace ShaderAdapter {
struct Dispatch {
  PFN_vkCreateDescriptorSetLayout createSetLayout;
  PFN_vkDestroyDescriptorSetLayout destroySetLayout;
  PFN_vkCreatePipelineLayout createPipelineLayout;
  PFN_vkDestroyPipelineLayout destroyPipelineLayout;
  PFN_vkCreateShaderModule createShader;
  PFN_vkDestroyShaderModule destroyShader;
  PFN_vkCreateGraphicsPipelines createGraphics;
};
static std::mutex dispatchMutex;
static std::unordered_map<VkDevice, Dispatch> dispatches;
static void registerDevice(VkDevice device, PFN_vkGetDeviceProcAddr next) {
  Dispatch dispatch{reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
                        next(device, "vkCreateDescriptorSetLayout")),
                    reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
                        next(device, "vkDestroyDescriptorSetLayout")),
                    reinterpret_cast<PFN_vkCreatePipelineLayout>(
                        next(device, "vkCreatePipelineLayout")),
                    reinterpret_cast<PFN_vkDestroyPipelineLayout>(
                        next(device, "vkDestroyPipelineLayout")),
                    reinterpret_cast<PFN_vkCreateShaderModule>(
                        next(device, "vkCreateShaderModule")),
                    reinterpret_cast<PFN_vkDestroyShaderModule>(
                        next(device, "vkDestroyShaderModule")),
                    reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
                        next(device, "vkCreateGraphicsPipelines"))};
  std::lock_guard<std::mutex> guard(dispatchMutex);
  dispatches.emplace(device, dispatch);
}
static const Dispatch *dstate(VkDevice device) {
  std::lock_guard<std::mutex> guard(dispatchMutex);
  // Map references survive insertion/rehashing. Vulkan requires Device
  // destruction to be externally synchronized against all operations using that
  // device.
  return &dispatches.at(device);
}
#include "shader_compat.hpp"
static void cleanup(VkDevice device) {
  diagnosticDeviceCleanup(device);
  std::lock_guard<std::mutex> guard(dispatchMutex);
  dispatches.erase(device);
}
static bool enabled() {
  static const bool value = []() {
    std::ifstream file(patchConfigDirectory() + "/shader-compat.enabled");
    std::string text;
    std::getline(file, text);
    return text == "1";
  }();
  return value;
}
static PFN_vkVoidFunction hook(const char *name) {
  if (std::strncmp(name, "vkCreate", 8) && std::strncmp(name, "vkDestroy", 9))
    return nullptr;
  if (!enabled())
    return nullptr;
#define SHADER_HOOK(api, function)                                             \
  if (!std::strcmp(name, api))                                                 \
  return reinterpret_cast<PFN_vkVoidFunction>(function)
  SHADER_HOOK("vkCreateDescriptorSetLayout", diagnosticCreateSetLayout);
  SHADER_HOOK("vkDestroyDescriptorSetLayout", diagnosticDestroySetLayout);
  SHADER_HOOK("vkCreatePipelineLayout", diagnosticCreatePipelineLayout);
  SHADER_HOOK("vkDestroyPipelineLayout", diagnosticDestroyPipelineLayout);
  SHADER_HOOK("vkCreateShaderModule", diagnosticCreateShader);
  SHADER_HOOK("vkDestroyShaderModule", diagnosticDestroyShader);
  SHADER_HOOK("vkCreateGraphicsPipelines", diagnosticCreateGraphics);
#undef SHADER_HOOK
  return nullptr;
}
} // namespace ShaderAdapter
