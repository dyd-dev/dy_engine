#include "dyf/RHI.h"
#include "dyf/Platform/Log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#define private public
#include "Backends/Vulkan/VulkanDevice.h"
#include "Backends/Vulkan/VulkanCommandList.h"
#include "Backends/Vulkan/VulkanPipeline.h"
#include "Backends/Vulkan/VulkanResources.h"
#include "Backends/Vulkan/VulkanSwapchain.h"
#include "../src/Backends/Vulkan/VulkanDevice.cpp"
#include "../src/Backends/Vulkan/VulkanCommandList.cpp"
#include "../src/Backends/Vulkan/VulkanPipeline.cpp"
#define HasUsage ResourceHasUsage
#include "../src/Backends/Vulkan/VulkanResources.cpp"
#undef HasUsage
#include "../src/Backends/Vulkan/VulkanSwapchain.cpp"
#undef private
#include "P2BackendTestAllocation.h"

namespace RHI = dyf::RHI;
using namespace dyf::Backends;
namespace
{
unsigned failures = 0;
unsigned buffers = 0, memories = 0, samplers = 0, pipelines = 0, layouts = 0, setLayouts = 0;
unsigned copies = 0, dispatches = 0, acquireCalls = 0, acquireWaits = 0, initBarriers = 0;
int samplerFailure = -1, stagingFailure = -1;
bool failPipelineTracker = false, failPool = false;
VkQueueFlags queueFlags = VK_QUEUE_GRAPHICS_BIT;
VkBool32 clipped = VK_TRUE;
VkPipelineStageFlags2 sourceStages = 0, destinationStages = 0;
uintptr_t nextHandle = 100;
template<typename T> T Handle() { return reinterpret_cast<T>(++nextHandle); }
template<typename T> VkResult Create(T* output) { *output = Handle<T>(); return VK_SUCCESS; }
void Check(bool value, const char* message)
{
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
VulkanContext Context()
{
    VulkanContext context{};
    context.device = Handle<VkDevice>();
    context.physicalDevice = Handle<VkPhysicalDevice>();
    context.graphicsQueue = Handle<VkQueue>();
    context.presentQueue = context.graphicsQueue;
    context.queueIndices.graphicsFamily = context.queueIndices.presentFamily = 0;
    return context;
}
RHI::TextureDesc TextureDesc()
{
    RHI::TextureDesc desc{};
    desc.width = desc.height = 2;
    desc.mipLevels = desc.depthOrArraySize = 1;
    desc.format = RHI::Format::R8G8B8A8_UNORM;
    desc.usage = RHI::TextureUsage::RenderTarget;
    return desc;
}
RHI::ResourceBindingLayout StaticSampler()
{
    RHI::ResourceBindingLayout binding{};
    binding.type = RHI::ResourceBindingType::StaticSampler;
    binding.stages = RHI::ShaderStageFlags::Compute;
    binding.count = 2;
    auto& sampler = binding.staticSampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = RHI::SamplerFilter::Linear;
    sampler.addressU = sampler.addressV = sampler.addressW = RHI::SamplerAddressMode::ClampToEdge;
    sampler.maxAnisotropy = 1;
    sampler.mipLodBias = sampler.minLod = 0;
    sampler.maxLod = 1;
    return binding;
}
RHI::ShaderDesc ShaderDesc(RHI::ShaderStage stage)
{
    // The external shader-module call is mocked; no shader runs on a GPU.
    static const uint32_t words[] = {0x07230203, 0x00010000, 0, 1, 0};
    RHI::ShaderDesc desc{};
    desc.stage = stage;
    desc.binary = words;
    desc.binarySize = sizeof(words);
    desc.entryPoint = "main";
    return desc;
}
}

// CPU-only Vulkan boundary: deterministic handles, bounded storage, no loader
// calls and no real queue submissions in these focused ownership tests.
extern "C" {
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* value)
{
    *value = {};
    value->limits.maxPushConstantsSize = 128;
    value->limits.maxSamplerAnisotropy = 16;
    for (auto& count : value->limits.maxComputeWorkGroupCount) count = 65535;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures* value) { *value = {}; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* value)
{
    *count = 1;
    if (value) { *value = {}; value->queueFlags = queueFlags; value->queueCount = 1; }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* value)
{
    *value = {};
    value->memoryTypeCount = 1;
    value->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer* value) { ++buffers; return Create(value); }
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) { --buffers; }
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements* value) { *value = {64, 4, 1}; }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* value) { ++memories; return Create(value); }
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) { --memories; }
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize size, VkMemoryMapFlags, void** output)
{ static unsigned char storage[64]{}; if (size > sizeof(storage)) return VK_ERROR_MEMORY_MAP_FAILED; *output = storage; return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory) { if (stagingFailure >= 0) { P2Allocation::failAfter = stagingFailure; stagingFailure = -1; } }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* output)
{ if (failPool) { failPool = false; return VK_ERROR_OUT_OF_HOST_MEMORY; } return Create(output); }
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* output) { return Create(output); }
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const VkBufferCopy*) { ++copies; }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t, const VkBufferImageCopy*) { ++copies; }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo* info)
{
    if (info->bufferMemoryBarrierCount) { sourceStages = info->pBufferMemoryBarriers[0].srcStageMask; destinationStages = info->pBufferMemoryBarriers[0].dstStageMask; }
    if (info->imageMemoryBarrierCount && info->pImageMemoryBarriers[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        info->pImageMemoryBarriers[0].newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) ++initBarriers;
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) {}
VKAPI_ATTR void VKAPI_CALL vkCmdDispatch(VkCommandBuffer, uint32_t, uint32_t, uint32_t) { ++dispatches; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo*, const VkAllocationCallbacks*, VkShaderModule* output) { return Create(output); }
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice, const VkSamplerCreateInfo*, const VkAllocationCallbacks*, VkSampler* output)
{ ++samplers; const auto result = Create(output); if (samplerFailure >= 0) { P2Allocation::failAfter = samplerFailure; samplerFailure = -1; } return result; }
VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks*) { --samplers; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* output) { ++setLayouts; return Create(output); }
VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks*) { --setLayouts; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout* output) { ++layouts; return Create(output); }
VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks*) { --layouts; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* output)
{ ++pipelines; const auto result = Create(output); if (failPipelineTracker) { failPipelineTracker = false; P2Allocation::failAfter = 0; } return result; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline* output) { ++pipelines; return Create(output); }
VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks*) { --pipelines; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* output) { return Create(output); }
VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice, VkFence) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t* output) { ++acquireCalls; *output = 0; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue, uint32_t count, const VkSubmitInfo* infos, VkFence)
{ for (uint32_t index = 0; index < count; ++index) acquireWaits += infos[index].waitSemaphoreCount; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* value)
{
    *value = {}; value->minImageCount = 2; value->maxImageCount = 3; value->currentExtent = {2,2};
    value->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    value->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    value->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count, VkSurfaceFormatKHR* output)
{ *count = 1; if (output) *output = {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t* count, VkPresentModeKHR* output)
{ *count = 1; if (output) *output = VK_PRESENT_MODE_FIFO_KHR; return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR* info, const VkAllocationCallbacks*, VkSwapchainKHR* output)
{ clipped = info->clipped; return Create(output); }
VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t* count, VkImage* output)
{ *count = 2; if (output) { output[0] = Handle<VkImage>(); output[1] = Handle<VkImage>(); } return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* output) { return Create(output); }
}

namespace
{
void ComputeAndSamplerOwnership()
{
    const auto context = Context();
    VulkanShader shader(context, ShaderDesc(RHI::ShaderStage::Compute));
    auto binding = StaticSampler();
    RHI::ComputePipelineDesc desc{};
    desc.computeShader = &shader;
    desc.layout.bindings = &binding;
    desc.layout.bindingCount = 1;
    for (int allocation : {0, 1})
    {
        const unsigned before = samplers;
        samplerFailure = allocation;
        bool threw = false;
        try { VulkanPipeline pipeline(context, desc); }
        catch (const std::bad_alloc&) { threw = true; }
        P2Allocation::failAfter = -1;
        Check(threw && samplers == before, "Sampler registration/follow-up allocation failure must release handles");
    }
    for (bool dynamic : {false, true})
    {
        if (dynamic) binding.type = RHI::ResourceBindingType::ConstantBuffer;
        VulkanPipeline pipeline(context, desc);
        VulkanCommandList commands(context);
        const unsigned before = dispatches;
        commands.BindComputePipelineNative(&pipeline);
        commands.DispatchNative(1, 1, 1);
        Check(commands.CloseNative() == !dynamic && dispatches == before + (dynamic ? 0 : 1),
            "Static-only compute can dispatch without a set; dynamic compute still requires one");
    }
    desc.layout = {};
    VulkanDevice owner;
    owner.m_impl->m_context = context;
    const auto beforePipelines = pipelines, beforeLayouts = layouts;
    failPipelineTracker = true;
    Check(owner.m_impl->CreateComputePipeline(desc) == nullptr, "Tracker failure must preserve null return contract");
    Check(owner.m_impl->m_pipelines.empty() && pipelines == beforePipelines && layouts == beforeLayouts,
        "Failed compute tracker registration must release the pipeline and layout");
    auto* pipeline = owner.m_impl->CreateComputePipeline(desc);
    Check(pipeline != nullptr, "Compute creation must recover after tracker allocation failure");
    owner.m_impl->DestroyPipeline(pipeline);
}

void StagingOwnership()
{
    const auto context = Context();
    RHI::BufferDesc desc{};
    desc.size = 16; desc.usage = RHI::BufferUsage::Storage; desc.initialState = RHI::ResourceState::CopyDestination;
    VulkanBuffer buffer(context, desc);
    VulkanTexture texture(TextureDesc(), Handle<VkImage>(), Handle<VkImageView>());
    // Use a non-owning external image as an upload target for CPU command recording.
    texture.m_ownsImage = true;
    const unsigned initialBuffers = buffers, initialMemories = memories;
    const uint32_t data[4]{};
    for (bool image : {false, true}) for (int fail : {0, 1})
    {
        const auto initialCopies = copies;
        bool threw = false;
        {
            VulkanCommandList commands(context);
            stagingFailure = fail;
            try
            {
                if (image) (void)commands.RecordTextureUpdate(texture, 0, 0, data, 16, 8, 16);
                else (void)commands.RecordBufferUpdate(buffer, 0, data, 16);
            }
            catch (const std::bad_alloc&) { threw = true; }
            P2Allocation::failAfter = -1;
        }
        Check(threw, "Staging ownership failure must be injected");
        Check(buffers == initialBuffers && memories == initialMemories, "Staging exception must release buffer and memory");
        Check(copies == initialCopies, "No copy may be recorded before host ownership and operation registration");
    }
    {
        VulkanCommandList commands(context);
        Check(commands.RecordBufferUpdate(buffer, 0, data, 16) &&
            commands.RecordTextureUpdate(texture, 0, 0, data, 16, 8, 16), "Normal buffer and texture upload recording must succeed");
    }
    Check(buffers == initialBuffers && memories == initialMemories, "Normal upload destruction must balance allocations");
    texture.m_ownsImage = false;
}

void ShaderBarrierStages()
{
    for (bool compute : {false, true})
    {
        queueFlags = VK_QUEUE_GRAPHICS_BIT | (compute ? VK_QUEUE_COMPUTE_BIT : 0);
        const auto context = Context();
        RHI::BufferDesc desc{};
        desc.size = 16; desc.usage = RHI::BufferUsage::Storage | RHI::BufferUsage::Constant;
        desc.initialState = RHI::ResourceState::CopyDestination;
        VulkanBuffer buffer(context, desc);
        for (auto state : {RHI::ResourceState::ConstantBuffer, RHI::ResourceState::ShaderResource, RHI::ResourceState::UnorderedAccess})
        {
            VulkanCommandList commands(context);
            RHI::ResourceBarrierDesc barrier{};
            barrier.buffer = &buffer; barrier.before = RHI::ResourceState::CopyDestination; barrier.after = state;
            commands.ResourceBarrierNative(&barrier, 1);
            Check(((destinationStages & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0) == compute,
                "Shader barrier destination stages must match queue compute capability");
            barrier.before = state; barrier.after = RHI::ResourceState::CopyDestination;
            commands.ResourceBarrierNative(&barrier, 1);
            Check(((sourceStages & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0) == compute,
                "Shader barrier source stages must match queue compute capability");
        }
    }
}

void StencilReadOnly()
{
    const auto context = Context();
    VulkanShader shader(context, ShaderDesc(RHI::ShaderStage::Vertex));
    RHI::GraphicsPipelineDesc desc{};
    desc.vertexShader = &shader;
    desc.topology = RHI::PrimitiveTopology::TriangleList;
    desc.raster.fillMode = RHI::FillMode::Solid;
    desc.raster.cullMode = RHI::CullMode::None;
    desc.raster.frontFace = RHI::FrontFace::CounterClockwise;
    desc.depthStencil.format = RHI::Format::D24_UNORM_S8_UINT;
    desc.depthStencil.stencilEnabled = true;
    auto& front = desc.depthStencil.front;
    front.compareOp = RHI::CompareOp::Always;
    front.failOp = front.depthFailOp = front.passOp = RHI::StencilOp::Keep;
    desc.depthStencil.back = front;
    for (int mode = 0; mode != 5; ++mode)
    {
        desc.depthStencil.stencilWriteMask = mode == 0 ? 0 : 255;
        desc.depthStencil.front.passOp = mode == 1 ? RHI::StencilOp::Keep : RHI::StencilOp::Replace;
        desc.depthStencil.depthWriteEnabled = mode == 3;
        desc.depthStencil.stencilEnabled = mode != 4;
        VulkanPipeline pipeline(context, desc);
        VulkanCommandList commands(context);
        commands.m_rendering = true;
        commands.m_depthState = RHI::ResourceState::DepthRead;
        commands.BindGraphicsPipelineNative(&pipeline);
        const bool writes = mode == 2 || mode == 3;
        Check(commands.m_failed == writes, "DepthRead binding must allow only read-only stencil/depth pipelines");
        commands.m_rendering = false;
    }
}

void FrameRetryAndRetirement()
{
    VulkanDevice owner;
    auto& impl = *owner.m_impl;
    impl.m_context = Context();
    impl.m_swapchain.m_swapchain = Handle<VkSwapchainKHR>();
    impl.m_swapchain.m_swapchainImages.push_back(Handle<VkImage>());
    impl.m_frameSlots.resize(1);
    impl.m_imagesInFlight.resize(1);
    impl.m_imageAvailableSemaphores.push_back(Handle<VkSemaphore>());
    impl.m_renderFinishedSemaphores.push_back(Handle<VkSemaphore>());
    impl.m_backBuffers.emplace_back(new VulkanTexture(TextureDesc(), impl.m_swapchain.GetImages()[0], Handle<VkImageView>()));
    const auto initialAcquires = acquireCalls, initialWaits = acquireWaits, initialBarriers = initBarriers;
    failPool = true;
    Check(!impl.BeginFrame(), "Injected first initialization pool failure must fail BeginFrame");
    Check(impl.m_imageAcquired && !impl.m_frameReady, "Failed initialization must preserve acquisition but clear readiness");
    Check(impl.BeginFrame(), "BeginFrame must retry the acquired image initialization");
    Check(acquireCalls == initialAcquires + 1 && acquireWaits == initialWaits + 1 && initBarriers == initialBarriers + 1,
        "Retry must initialize exactly once without reacquiring or double-waiting");
    Check(impl.Present(), "A retried initialized frame must support direct Present");
    impl.CollectCompletedSubmissions();
    const auto handle = impl.m_swapchain.GetHandle();
    const auto semaphore = impl.m_imageAvailableSemaphores[0];
    const auto count = impl.m_backBuffers.size();
    P2Allocation::failAfter = 0;
    bool threw = false;
    try { (void)impl.RetireSwapchainGeneration(); }
    catch (const std::bad_alloc&) { threw = true; }
    P2Allocation::failAfter = -1;
    Check(threw && impl.m_swapchain.GetHandle() == handle && impl.m_backBuffers.size() == count &&
        impl.m_imageAvailableSemaphores.size() == 1 && impl.m_imageAvailableSemaphores[0] == semaphore,
        "Retirement allocation failure must preserve the current generation");
    if (impl.m_swapchain.GetHandle())
    {
        Check(impl.RetireSwapchainGeneration() == handle && impl.m_retiredSwapchains.size() == 1,
            "Retirement retry must transfer the complete generation");
        impl.DestroyRetiredSwapchains();
    }
}

void ReadbackSwapchainClipping()
{
    auto context = Context();
    context.surface = Handle<VkSurfaceKHR>();
    for (bool readback : {false, true})
    {
        VulkanSwapchain swapchain;
        VulkanSwapchain::InitializationStatus status;
        bool retired = false;
        Check(swapchain.Initialize(context, nullptr, VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, 2, readback, 2, 2, VK_NULL_HANDLE, retired, status),
            "Swapchain creation must succeed in either readback mode");
        Check(clipped == (readback ? VK_FALSE : VK_TRUE), "Readback swapchain must request preservation of obscured pixels");
        swapchain.Cleanup(context.device);
    }
}
}

int main() try
{
    ComputeAndSamplerOwnership();
    StagingOwnership();
    ShaderBarrierStages();
    StencilReadOnly();
    FrameRetryAndRetirement();
    ReadbackSwapchainClipping();
    std::printf("P2 Vulkan: %u failures\n", failures);
    return failures ? 1 : 0;
}
catch (const std::exception& error) { P2Allocation::failAfter = -1; std::fprintf(stderr, "%s\n", error.what()); return 2; }
