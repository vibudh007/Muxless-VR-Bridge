#pragma once

#include <vulkan/vulkan.h>

namespace v3 {

PFN_vkVoidFunction auditNextDevice(const void *dispatchable, const char *name);
bool auditOwnsImage(VkImage image);
void auditBeginWrite(VkCommandBuffer commands, VkImage image);
void auditEndWrite(VkCommandBuffer commands, VkImage image);
PFN_vkVoidFunction outputAuditHook(const char *name);

}
