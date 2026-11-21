#include "output-audit.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace v3 {
namespace {

struct Attachment {
    uint32_t index;
    VkAttachmentLoadOp load;
    VkAttachmentStoreOp store;
    bool resolve;
};

struct Target {
    VkImage image;
    VkAttachmentLoadOp load;
    VkAttachmentStoreOp store;
    bool resolve;
};

struct Pass {
    const char *command{};
    VkRect2D area{};
    std::vector<Target> targets;
    uint32_t drawCommands{}, knownDraws{}, indirectCountCommands{}, secondaryBuffers{}, clearCommands{};
    bool stamp{true};
};

struct PipelineState {
    int blend{-1}, logicOp{-1}, writeMask{-1};
    bool dynamicViewport{}, dynamicScissor{};
    std::optional<VkViewport> viewport;
    std::optional<VkRect2D> scissor;
};

struct DrawState {
    VkPipeline pipeline{};
    std::optional<VkViewport> viewport;
    std::optional<VkRect2D> scissor;
};

std::mutex auditMutex;
std::unordered_map<VkImageView, VkImage> outputViews;
std::unordered_map<VkFramebuffer, std::vector<VkImageView>> framebuffers;
std::unordered_map<VkRenderPass, std::vector<Attachment>> renderPasses;
std::unordered_map<VkCommandBuffer, Pass> activePasses;
std::unordered_map<VkPipeline, PipelineState> pipelines;
std::unordered_map<VkCommandBuffer, DrawState> recordings;
std::unordered_map<VkCommandBuffer, std::pair<VkDevice, VkCommandPool>> commandPools;
std::array<std::array<char, 384>, 40> seen{};
size_t seenCount{};
std::atomic_flag auditErrorReported = ATOMIC_FLAG_INIT;

template<class Fn> Fn next(const void *dispatchable, const char *name) {
    return reinterpret_cast<Fn>(auditNextDevice(dispatchable, name));
}

template<class Fn> void observe(Fn observation) noexcept {
    try {
        observation();
    } catch (...) {
        if (!auditErrorReported.test_and_set())
            std::fputs("v3 output audit: could not record diagnostic state\n", stderr);
    }
}

void forgetRecording(VkCommandBuffer cb) {
    activePasses.erase(cb);
    recordings.erase(cb);
}

void clearPool(VkDevice device, VkCommandPool pool, bool destroy) {
    const std::lock_guard lock(auditMutex);
    const auto owner = std::make_pair(device, pool);
    for (auto it = commandPools.begin(); it != commandPools.end(); ) {
        if (it->second != owner) {
            ++it;
            continue;
        }
        forgetRecording(it->first);
        if (destroy)
            it = commandPools.erase(it);
        else
            ++it;
    }
}

void logOnce(const char *message) {
    if (seenCount == seen.size())
        return;
    const auto end = seen.begin() + seenCount;
    if (std::find_if(seen.begin(), end, [&](const auto &entry) {
            return std::strcmp(entry.data(), message) == 0;
        }) != end)
        return;
    std::snprintf(seen[seenCount++].data(), seen.front().size(), "%s", message);
    std::fprintf(stderr, "v3 output audit: %s\n", message);
}

VkImage imageFor(VkImageView view) {
    const auto found = outputViews.find(view);
    return found == outputViews.end() ? VK_NULL_HANDLE : found->second;
}

template<class Info> void storeRenderPass(VkRenderPass handle, const Info *info) {
    std::vector<Attachment> attachments;
    auto add = [&](uint32_t index, bool resolve) {
        if (index >= info->attachmentCount || std::any_of(attachments.begin(), attachments.end(),
                [&](const auto &a) { return a.index == index && a.resolve == resolve; }))
            return;
        const auto &description = info->pAttachments[index];
        attachments.push_back({index, description.loadOp, description.storeOp, resolve});
    };
    for (uint32_t i = 0; i < info->subpassCount; ++i) {
        const auto &subpass = info->pSubpasses[i];
        for (uint32_t j = 0; j < subpass.colorAttachmentCount; ++j) {
            add(subpass.pColorAttachments[j].attachment, false);
            if (subpass.pResolveAttachments)
                add(subpass.pResolveAttachments[j].attachment, true);
        }
    }
    const std::lock_guard lock(auditMutex);
    renderPasses[handle] = std::move(attachments);
}

void remember(VkCommandBuffer cb, Pass pass) {
    if (pass.targets.empty())
        return;
    if (pass.stamp) {
        for (auto target = pass.targets.begin(); target != pass.targets.end(); ++target)
            if (std::none_of(pass.targets.begin(), target,
                             [&](const auto &other) { return other.image == target->image; }))
                auditBeginWrite(cb, target->image);
    }
    const std::lock_guard lock(auditMutex);
    activePasses[cb] = std::move(pass);
}

void beginRenderPass(VkCommandBuffer cb, const char *command, const VkRenderPassBeginInfo &info) {
    Pass pass{command, info.renderArea};
    {
        const std::lock_guard lock(auditMutex);
        const auto definitions = renderPasses.find(info.renderPass);
        if (definitions == renderPasses.end())
            return;
        uint32_t count = 0;
        const VkImageView *views = nullptr;
        const auto framebuffer = framebuffers.find(info.framebuffer);
        if (framebuffer != framebuffers.end()) {
            count = framebuffer->second.size();
            views = framebuffer->second.data();
        } else {
            for (auto *item = static_cast<const VkBaseInStructure *>(info.pNext); item; item = item->pNext) {
                if (item->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
                    continue;
                const auto *imageless = reinterpret_cast<const VkRenderPassAttachmentBeginInfo *>(item);
                count = imageless->attachmentCount;
                views = imageless->pAttachments;
                break;
            }
        }
        for (const auto &a : definitions->second) {
            if (a.index >= count)
                continue;
            if (const auto image = imageFor(views[a.index]))
                pass.targets.push_back({image, a.load, a.store, a.resolve});
        }
    }
    remember(cb, std::move(pass));
}

void beginDynamic(VkCommandBuffer cb, const char *command, const VkRenderingInfo &info) {
    Pass pass{command, info.renderArea};
    // Vulkan disallows synchronization commands between suspended and resumed instances.
    pass.stamp = !(info.flags & (VK_RENDERING_RESUMING_BIT | VK_RENDERING_SUSPENDING_BIT));
    {
        const std::lock_guard lock(auditMutex);
        for (uint32_t i = 0; i < info.colorAttachmentCount; ++i) {
            const auto &a = info.pColorAttachments[i];
            if (const auto image = imageFor(a.imageView))
                pass.targets.push_back({image, a.loadOp, a.storeOp, false});
            if (a.resolveMode != VK_RESOLVE_MODE_NONE)
                if (const auto resolve = imageFor(a.resolveImageView))
                    pass.targets.push_back({resolve, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                            VK_ATTACHMENT_STORE_OP_STORE, true});
        }
    }
    remember(cb, std::move(pass));
}

void endPass(VkCommandBuffer cb) {
    Pass pass;
    {
        const std::lock_guard lock(auditMutex);
        const auto found = activePasses.find(cb);
        if (found == activePasses.end())
            return;
        pass = std::move(found->second);
        activePasses.erase(found);
        for (const auto &target : pass.targets) {
            char message[256];
            std::snprintf(message, sizeof(message),
                          "%s offset=%d,%d extent=%ux%u role=%s load=%d store=%d draw_calls=%u draws=%u "
                          "indirect_count_calls=%u secondary_buffers=%u clear_calls=%u bracketed=%u",
                          pass.command, pass.area.offset.x, pass.area.offset.y, pass.area.extent.width,
                          pass.area.extent.height, target.resolve ? "resolve" : "color",
                          int(target.load), int(target.store), pass.drawCommands, pass.knownDraws,
                          pass.indirectCountCommands, pass.secondaryBuffers, pass.clearCommands, unsigned(pass.stamp));
            logOnce(message);
        }
    }
    if (pass.stamp) {
        for (auto target = pass.targets.begin(); target != pass.targets.end(); ++target)
            if (std::none_of(pass.targets.begin(), target,
                             [&](const auto &other) { return other.image == target->image; }))
                auditEndWrite(cb, target->image);
    }
}

template<class Work> void editPass(VkCommandBuffer cb, Work work) {
    const std::lock_guard lock(auditMutex);
    const auto pass = activePasses.find(cb);
    if (pass != activePasses.end())
        work(pass->second);
}

PipelineState pipelineState(const VkGraphicsPipelineCreateInfo &info) {
    PipelineState state;
    const auto dynamic = [&](VkDynamicState key) {
        const auto *list = info.pDynamicState;
        if (!list || !list->dynamicStateCount)
            return false;
        const auto *end = list->pDynamicStates + list->dynamicStateCount;
        return std::find(list->pDynamicStates, end, key) != end;
    };
    const VkPipelineCreateFlags2CreateInfo *flags2 = nullptr;
    for (auto *item = static_cast<const VkBaseInStructure *>(info.pNext); item; item = item->pNext) {
        if (item->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT ||
            item->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)
            return state;
        if (item->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)
            flags2 = reinterpret_cast<const VkPipelineCreateFlags2CreateInfo *>(item);
    }
    const VkPipelineCreateFlags2 flags = flags2 ? flags2->flags : info.flags;
    if ((flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) ||
        (info.pRasterizationState && !dynamic(VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE) &&
         info.pRasterizationState->rasterizerDiscardEnable))
        return state;
    if (const auto *blend = info.pColorBlendState) {
        if (!dynamic(VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT))
            state.logicOp = blend->logicOpEnable;
        if (blend->attachmentCount == 1 && blend->pAttachments) {
            if (!dynamic(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT))
                state.blend = blend->pAttachments[0].blendEnable;
            if (!dynamic(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT) &&
                !dynamic(VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT)) {
                state.writeMask = blend->pAttachments[0].colorWriteMask;
                for (auto *item = static_cast<const VkBaseInStructure *>(blend->pNext); item; item = item->pNext) {
                    if (item->sType == VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT) {
                        const auto *write = reinterpret_cast<const VkPipelineColorWriteCreateInfoEXT *>(item);
                        if (write->attachmentCount && !write->pColorWriteEnables[0])
                            state.writeMask = 0;
                    }
                }
            }
        }
    }
    state.dynamicViewport = dynamic(VK_DYNAMIC_STATE_VIEWPORT) || dynamic(VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT);
    state.dynamicScissor = dynamic(VK_DYNAMIC_STATE_SCISSOR) || dynamic(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT);
    if (const auto *view = info.pViewportState) {
        if (!state.dynamicViewport && view->viewportCount && view->pViewports)
            state.viewport = view->pViewports[0];
        if (!state.dynamicScissor && view->scissorCount && view->pScissors)
            state.scissor = view->pScissors[0];
    }
    return state;
}

void logDrawState(VkCommandBuffer cb, const Pass &pass) {
    const auto recording = recordings.find(cb);
    if (recording == recordings.end())
        return;
    const auto pipeline = pipelines.find(recording->second.pipeline);
    if (pipeline == pipelines.end())
        return;
    const auto &state = pipeline->second;
    const auto &viewport = state.dynamicViewport ? recording->second.viewport : state.viewport;
    const auto &scissor = state.dynamicScissor ? recording->second.scissor : state.scissor;
    char viewportText[80] = "unknown";
    char scissorText[80] = "unknown";
    if (viewport)
        std::snprintf(viewportText, sizeof(viewportText), "%g,%g %gx%g", viewport->x, viewport->y,
                      viewport->width, viewport->height);
    if (scissor)
        std::snprintf(scissorText, sizeof(scissorText), "%d,%d %ux%u", scissor->offset.x,
                      scissor->offset.y, scissor->extent.width, scissor->extent.height);
    char message[384];
    std::snprintf(message, sizeof(message),
                  "graphics draw area=%d,%d %ux%u blend=%d logic_op=%d write_mask=%d viewport0=%s scissor0=%s",
                  pass.area.offset.x, pass.area.offset.y, pass.area.extent.width, pass.area.extent.height,
                  state.blend, state.logicOp, state.writeMask, viewportText, scissorText);
    logOnce(message);
}

void countDraw(VkCommandBuffer cb, uint32_t count, bool indirectCount = false) {
    editPass(cb, [=](auto &pass) {
        ++pass.drawCommands;
        pass.knownDraws += count;
        pass.indirectCountCommands += indirectCount;
        logDrawState(cb, pass);
    });
}

template<class Work> void transfer(VkCommandBuffer cb, const char *command, VkImage destination,
                                   uint32_t regions, VkExtent3D first, Work work) {
    if (!auditOwnsImage(destination)) {
        work();
        return;
    }
    auditBeginWrite(cb, destination);
    work();
    auditEndWrite(cb, destination);
    char message[256];
    std::snprintf(message, sizeof(message), "%s destination_regions=%u first_dst_extent=%ux%ux%u",
                  command, regions, first.width, first.height, first.depth);
    const std::lock_guard lock(auditMutex);
    logOnce(message);
}

template<class Region> VkExtent3D blitExtent(const Region &region) {
    const auto &begin = region.dstOffsets[0];
    const auto &end = region.dstOffsets[1];
    return {uint32_t(std::abs(end.x - begin.x)), uint32_t(std::abs(end.y - begin.y)),
            uint32_t(std::abs(end.z - begin.z))};
}

#define CMD(name) VKAPI_ATTR void VKAPI_CALL wrap##name
#define DEVICE(name) VKAPI_ATTR VkResult VKAPI_CALL wrap##name
#define DOWN(name, handle) next<PFN_vk##name>(handle, "vk" #name)

DEVICE(CreateImageView)(VkDevice d, const VkImageViewCreateInfo *ci,
                        const VkAllocationCallbacks *allocator, VkImageView *out) {
    const auto result = DOWN(CreateImageView, d)(d, ci, allocator, out);
    if (result == VK_SUCCESS && auditOwnsImage(ci->image)) {
        observe([&] {
            const std::lock_guard lock(auditMutex);
            outputViews[*out] = ci->image;
        });
    }
    return result;
}

CMD(DestroyImageView)(VkDevice d, VkImageView view, const VkAllocationCallbacks *allocator) {
    {
        const std::lock_guard lock(auditMutex);
        outputViews.erase(view);
    }
    DOWN(DestroyImageView, d)(d, view, allocator);
}

DEVICE(CreateFramebuffer)(VkDevice d, const VkFramebufferCreateInfo *ci,
                          const VkAllocationCallbacks *allocator, VkFramebuffer *out) {
    const auto result = DOWN(CreateFramebuffer, d)(d, ci, allocator, out);
    if (result == VK_SUCCESS && ci->attachmentCount && !(ci->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) {
        observe([&] {
            const std::lock_guard lock(auditMutex);
            if (std::any_of(ci->pAttachments, ci->pAttachments + ci->attachmentCount,
                            [](VkImageView view) { return imageFor(view) != VK_NULL_HANDLE; }))
                framebuffers[*out].assign(ci->pAttachments, ci->pAttachments + ci->attachmentCount);
        });
    }
    return result;
}

CMD(DestroyFramebuffer)(VkDevice d, VkFramebuffer fb, const VkAllocationCallbacks *allocator) {
    {
        const std::lock_guard lock(auditMutex);
        framebuffers.erase(fb);
    }
    DOWN(DestroyFramebuffer, d)(d, fb, allocator);
}

#define CREATE_PASS(name, infoType) \
DEVICE(name)(VkDevice d, const infoType *ci, const VkAllocationCallbacks *allocator, VkRenderPass *out) { \
    const auto result = DOWN(name, d)(d, ci, allocator, out); \
    if (result == VK_SUCCESS) \
        observe([&] { storeRenderPass(*out, ci); }); \
    return result; \
}
CREATE_PASS(CreateRenderPass, VkRenderPassCreateInfo)
CREATE_PASS(CreateRenderPass2, VkRenderPassCreateInfo2)
CREATE_PASS(CreateRenderPass2KHR, VkRenderPassCreateInfo2)
#undef CREATE_PASS

CMD(DestroyRenderPass)(VkDevice d, VkRenderPass pass, const VkAllocationCallbacks *allocator) {
    {
        const std::lock_guard lock(auditMutex);
        renderPasses.erase(pass);
    }
    DOWN(DestroyRenderPass, d)(d, pass, allocator);
}

DEVICE(CreateGraphicsPipelines)(VkDevice d, VkPipelineCache cache, uint32_t count,
                                const VkGraphicsPipelineCreateInfo *infos,
                                const VkAllocationCallbacks *allocator, VkPipeline *out) {
    const auto result = DOWN(CreateGraphicsPipelines, d)(d, cache, count, infos, allocator, out);
    observe([&] {
        const std::lock_guard lock(auditMutex);
        for (uint32_t i = 0; i < count; ++i)
            if (out[i])
                pipelines[out[i]] = pipelineState(infos[i]);
    });
    return result;
}

CMD(DestroyPipeline)(VkDevice d, VkPipeline pipeline, const VkAllocationCallbacks *allocator) {
    {
        const std::lock_guard lock(auditMutex);
        pipelines.erase(pipeline);
    }
    DOWN(DestroyPipeline, d)(d, pipeline, allocator);
}

DEVICE(AllocateCommandBuffers)(VkDevice d, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *out) {
    const auto result = DOWN(AllocateCommandBuffers, d)(d, info, out);
    if (result == VK_SUCCESS)
        observe([&] {
            const std::lock_guard lock(auditMutex);
            for (uint32_t i = 0; i < info->commandBufferCount; ++i)
                commandPools.emplace(out[i], std::make_pair(d, info->commandPool));
        });
    return result;
}

CMD(FreeCommandBuffers)(VkDevice d, VkCommandPool pool, uint32_t count, const VkCommandBuffer *buffers) {
    {
        const std::lock_guard lock(auditMutex);
        for (uint32_t i = 0; i < count; ++i) {
            forgetRecording(buffers[i]);
            commandPools.erase(buffers[i]);
        }
    }
    DOWN(FreeCommandBuffers, d)(d, pool, count, buffers);
}

CMD(DestroyCommandPool)(VkDevice d, VkCommandPool pool, const VkAllocationCallbacks *allocator) {
    if (pool)
        clearPool(d, pool, true);
    DOWN(DestroyCommandPool, d)(d, pool, allocator);
}

DEVICE(ResetCommandPool)(VkDevice d, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    const auto result = DOWN(ResetCommandPool, d)(d, pool, flags);
    if (result == VK_SUCCESS)
        clearPool(d, pool, false);
    return result;
}

DEVICE(ResetCommandBuffer)(VkCommandBuffer cb, VkCommandBufferResetFlags flags) {
    const auto result = DOWN(ResetCommandBuffer, cb)(cb, flags);
    if (result == VK_SUCCESS) {
        const std::lock_guard lock(auditMutex);
        forgetRecording(cb);
    }
    return result;
}

DEVICE(BeginCommandBuffer)(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info) {
    const auto result = DOWN(BeginCommandBuffer, cb)(cb, info);
    if (result == VK_SUCCESS) {
        const std::lock_guard lock(auditMutex);
        forgetRecording(cb);
    }
    return result;
}

DEVICE(EndCommandBuffer)(VkCommandBuffer cb) {
    {
        const std::lock_guard lock(auditMutex);
        forgetRecording(cb);
    }
    return DOWN(EndCommandBuffer, cb)(cb);
}

CMD(CmdBindPipeline)(VkCommandBuffer cb, VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
    DOWN(CmdBindPipeline, cb)(cb, bindPoint, pipeline);
    if (bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
        observe([&] {
            const std::lock_guard lock(auditMutex);
            recordings[cb].pipeline = pipeline;
        });
}

CMD(CmdSetViewport)(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport *values) {
    DOWN(CmdSetViewport, cb)(cb, first, count, values);
    if (!first && count)
        observe([&] {
            const std::lock_guard lock(auditMutex);
            recordings[cb].viewport = values[0];
        });
}

CMD(CmdSetScissor)(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D *values) {
    DOWN(CmdSetScissor, cb)(cb, first, count, values);
    if (!first && count)
        observe([&] {
            const std::lock_guard lock(auditMutex);
            recordings[cb].scissor = values[0];
        });
}

#define SET_WITH_COUNT(name, type, field) \
CMD(name)(VkCommandBuffer cb, uint32_t count, const type *values) { \
    DOWN(name, cb)(cb, count, values); \
    if (count) \
        observe([&] { \
            const std::lock_guard lock(auditMutex); \
            recordings[cb].field = values[0]; \
        }); \
}
SET_WITH_COUNT(CmdSetViewportWithCount, VkViewport, viewport)
SET_WITH_COUNT(CmdSetViewportWithCountEXT, VkViewport, viewport)
SET_WITH_COUNT(CmdSetScissorWithCount, VkRect2D, scissor)
SET_WITH_COUNT(CmdSetScissorWithCountEXT, VkRect2D, scissor)
#undef SET_WITH_COUNT

CMD(CmdBeginRenderPass)(VkCommandBuffer cb, const VkRenderPassBeginInfo *info, VkSubpassContents contents) {
    observe([&] { beginRenderPass(cb, "vkCmdBeginRenderPass", *info); });
    DOWN(CmdBeginRenderPass, cb)(cb, info, contents);
}

#define BEGIN_PASS2(name) \
CMD(name)(VkCommandBuffer cb, const VkRenderPassBeginInfo *info, const VkSubpassBeginInfo *subpass) { \
    observe([&] { beginRenderPass(cb, "vk" #name, *info); }); \
    DOWN(name, cb)(cb, info, subpass); \
}
BEGIN_PASS2(CmdBeginRenderPass2)
BEGIN_PASS2(CmdBeginRenderPass2KHR)
#undef BEGIN_PASS2

#define BEGIN_RENDERING(name) \
CMD(name)(VkCommandBuffer cb, const VkRenderingInfo *info) { \
    observe([&] { beginDynamic(cb, "vk" #name, *info); }); \
    DOWN(name, cb)(cb, info); \
}
BEGIN_RENDERING(CmdBeginRendering)
BEGIN_RENDERING(CmdBeginRenderingKHR)
#undef BEGIN_RENDERING

#define END_PASS(name) \
CMD(name)(VkCommandBuffer cb) { \
    DOWN(name, cb)(cb); \
    endPass(cb); \
}
END_PASS(CmdEndRenderPass)
END_PASS(CmdEndRendering)
END_PASS(CmdEndRenderingKHR)
#undef END_PASS

#define END_PASS2(name) \
CMD(name)(VkCommandBuffer cb, const VkSubpassEndInfo *info) { \
    DOWN(name, cb)(cb, info); \
    endPass(cb); \
}
END_PASS2(CmdEndRenderPass2)
END_PASS2(CmdEndRenderPass2KHR)
#undef END_PASS2

CMD(CmdDraw)(VkCommandBuffer cb, uint32_t vertices, uint32_t instances, uint32_t vertex, uint32_t instance) {
    DOWN(CmdDraw, cb)(cb, vertices, instances, vertex, instance);
    countDraw(cb, 1);
}

CMD(CmdDrawIndexed)(VkCommandBuffer cb, uint32_t indices, uint32_t instances, uint32_t index,
                    int32_t vertex, uint32_t instance) {
    DOWN(CmdDrawIndexed, cb)(cb, indices, instances, index, vertex, instance);
    countDraw(cb, 1);
}

#define DRAW_INDIRECT(name) \
CMD(name)(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, uint32_t count, uint32_t stride) { \
    DOWN(name, cb)(cb, buffer, offset, count, stride); \
    countDraw(cb, count); \
}
DRAW_INDIRECT(CmdDrawIndirect)
DRAW_INDIRECT(CmdDrawIndexedIndirect)
#undef DRAW_INDIRECT

#define DRAW_COUNT(name) \
CMD(name)(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, \
          VkDeviceSize countOffset, uint32_t maxCount, uint32_t stride) { \
    DOWN(name, cb)(cb, buffer, offset, countBuffer, countOffset, maxCount, stride); \
    countDraw(cb, 0, true); \
}
DRAW_COUNT(CmdDrawIndirectCount)
DRAW_COUNT(CmdDrawIndexedIndirectCount)
DRAW_COUNT(CmdDrawIndirectCountKHR)
DRAW_COUNT(CmdDrawIndexedIndirectCountKHR)
#undef DRAW_COUNT

CMD(CmdExecuteCommands)(VkCommandBuffer cb, uint32_t count, const VkCommandBuffer *secondary) {
    DOWN(CmdExecuteCommands, cb)(cb, count, secondary);
    editPass(cb, [=](auto &pass) { pass.secondaryBuffers += count; });
}

CMD(CmdClearAttachments)(VkCommandBuffer cb, uint32_t count, const VkClearAttachment *attachments,
                         uint32_t rectCount, const VkClearRect *rects) {
    DOWN(CmdClearAttachments, cb)(cb, count, attachments, rectCount, rects);
    editPass(cb, [](auto &pass) { ++pass.clearCommands; });
}

#define TRANSFER_IMAGE(name, regionType, extent) \
CMD(name)(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst, \
          VkImageLayout dstLayout, uint32_t count, const regionType *regions) { \
    transfer(cb, "vk" #name, dst, count, count ? (extent) : VkExtent3D{}, [&] { \
        DOWN(name, cb)(cb, src, srcLayout, dst, dstLayout, count, regions); \
    }); \
}
TRANSFER_IMAGE(CmdCopyImage, VkImageCopy, regions[0].extent)
TRANSFER_IMAGE(CmdResolveImage, VkImageResolve, regions[0].extent)
#undef TRANSFER_IMAGE

CMD(CmdBlitImage)(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst,
                  VkImageLayout dstLayout, uint32_t count, const VkImageBlit *regions, VkFilter filter) {
    transfer(cb, "vkCmdBlitImage", dst, count, count ? blitExtent(regions[0]) : VkExtent3D{}, [&] {
        DOWN(CmdBlitImage, cb)(cb, src, srcLayout, dst, dstLayout, count, regions, filter);
    });
}

CMD(CmdCopyBufferToImage)(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout,
                          uint32_t count, const VkBufferImageCopy *regions) {
    transfer(cb, "vkCmdCopyBufferToImage", dst, count, count ? regions[0].imageExtent : VkExtent3D{}, [&] {
        DOWN(CmdCopyBufferToImage, cb)(cb, src, dst, layout, count, regions);
    });
}

#define TRANSFER2(name, infoType, extent) \
CMD(name)(VkCommandBuffer cb, const infoType *info) { \
    transfer(cb, "vk" #name, info->dstImage, info->regionCount, \
             info->regionCount ? (extent) : VkExtent3D{}, [&] { DOWN(name, cb)(cb, info); }); \
}
TRANSFER2(CmdCopyImage2, VkCopyImageInfo2, info->pRegions[0].extent)
TRANSFER2(CmdCopyImage2KHR, VkCopyImageInfo2, info->pRegions[0].extent)
TRANSFER2(CmdResolveImage2, VkResolveImageInfo2, info->pRegions[0].extent)
TRANSFER2(CmdResolveImage2KHR, VkResolveImageInfo2, info->pRegions[0].extent)
TRANSFER2(CmdBlitImage2, VkBlitImageInfo2, blitExtent(info->pRegions[0]))
TRANSFER2(CmdBlitImage2KHR, VkBlitImageInfo2, blitExtent(info->pRegions[0]))
TRANSFER2(CmdCopyBufferToImage2, VkCopyBufferToImageInfo2, info->pRegions[0].imageExtent)
TRANSFER2(CmdCopyBufferToImage2KHR, VkCopyBufferToImageInfo2, info->pRegions[0].imageExtent)
#undef TRANSFER2

CMD(CmdClearColorImage)(VkCommandBuffer cb, VkImage dst, VkImageLayout layout,
                       const VkClearColorValue *color, uint32_t count, const VkImageSubresourceRange *ranges) {
    transfer(cb, "vkCmdClearColorImage", dst, count, {}, [&] {
        DOWN(CmdClearColorImage, cb)(cb, dst, layout, color, count, ranges);
    });
}

#undef DOWN
#undef DEVICE
#undef CMD

}

PFN_vkVoidFunction outputAuditHook(const char *name) {
    static const bool enabled = [] {
        const auto *value = std::getenv("V3_OUTPUT_AUDIT");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (!enabled)
        return nullptr;
#define HOOK(function) if (std::strcmp(name, "vk" #function) == 0) \
    return reinterpret_cast<PFN_vkVoidFunction>(wrap##function)
    HOOK(CreateImageView);
    HOOK(DestroyImageView);
    HOOK(CreateFramebuffer);
    HOOK(DestroyFramebuffer);
    HOOK(CreateRenderPass);
    HOOK(CreateRenderPass2);
    HOOK(CreateRenderPass2KHR);
    HOOK(DestroyRenderPass);
    HOOK(CreateGraphicsPipelines);
    HOOK(DestroyPipeline);
    HOOK(AllocateCommandBuffers);
    HOOK(FreeCommandBuffers);
    HOOK(DestroyCommandPool);
    HOOK(ResetCommandPool);
    HOOK(ResetCommandBuffer);
    HOOK(BeginCommandBuffer);
    HOOK(EndCommandBuffer);
    HOOK(CmdBindPipeline);
    HOOK(CmdSetViewport);
    HOOK(CmdSetScissor);
    HOOK(CmdSetViewportWithCount);
    HOOK(CmdSetViewportWithCountEXT);
    HOOK(CmdSetScissorWithCount);
    HOOK(CmdSetScissorWithCountEXT);
    HOOK(CmdBeginRenderPass);
    HOOK(CmdBeginRenderPass2);
    HOOK(CmdBeginRenderPass2KHR);
    HOOK(CmdBeginRendering);
    HOOK(CmdBeginRenderingKHR);
    HOOK(CmdEndRenderPass);
    HOOK(CmdEndRenderPass2);
    HOOK(CmdEndRenderPass2KHR);
    HOOK(CmdEndRendering);
    HOOK(CmdEndRenderingKHR);
    HOOK(CmdDraw);
    HOOK(CmdDrawIndexed);
    HOOK(CmdDrawIndirect);
    HOOK(CmdDrawIndexedIndirect);
    HOOK(CmdDrawIndirectCount);
    HOOK(CmdDrawIndexedIndirectCount);
    HOOK(CmdDrawIndirectCountKHR);
    HOOK(CmdDrawIndexedIndirectCountKHR);
    HOOK(CmdExecuteCommands);
    HOOK(CmdClearAttachments);
    HOOK(CmdCopyImage);
    HOOK(CmdCopyImage2);
    HOOK(CmdCopyImage2KHR);
    HOOK(CmdBlitImage);
    HOOK(CmdBlitImage2);
    HOOK(CmdBlitImage2KHR);
    HOOK(CmdResolveImage);
    HOOK(CmdResolveImage2);
    HOOK(CmdResolveImage2KHR);
    HOOK(CmdCopyBufferToImage);
    HOOK(CmdCopyBufferToImage2);
    HOOK(CmdCopyBufferToImage2KHR);
    HOOK(CmdClearColorImage);
#undef HOOK
    return nullptr;
}

}
