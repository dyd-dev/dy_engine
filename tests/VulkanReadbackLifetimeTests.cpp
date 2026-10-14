#include "Backends/Vulkan/VulkanResources.h"
#include "dyf/RHI.h"
#include "dyf/Platform/Log.h"

#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

// Interpose only the external Vulkan API in this test executable. The engine's
// real readback/ownership paths run unchanged. A real idle wait completes before
// returning a synthetic error, so a failing baseline cannot free in-flight work.
namespace
{
    template<typename Function>
    Function Native(const char* name)
    {
        static const HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
        const auto address = loader ? GetProcAddress(loader, name) : nullptr;
        if(!address) throw std::runtime_error("Vulkan loader entry point unavailable");
        return reinterpret_cast<Function>(address);
    }

    struct Observation
    {
        bool capture = false;
        bool failBind = false;
        bool failShutdown = false;
        VkResult waitResult = VK_SUCCESS;
        VkResult submitResult = VK_SUCCESS;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkImage source = VK_NULL_HANDLE;
        unsigned buffersDestroyed = 0;
        unsigned memoryFreed = 0;
        unsigned poolsDestroyed = 0;
        unsigned sourcesDestroyed = 0;
        unsigned injectedWaits = 0;
    } observed;

    unsigned failures = 0;
    void Check(bool value, const char* message)
    {
        if(value) return;
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    void Require(bool value, const char* message)
    {
        if(!value) throw std::runtime_error(message);
    }

    struct Reentry
    {
        dyf::RHI::IDevice* device = nullptr;
        dyf::RHI::TextureHandle source = nullptr;
        unsigned calls = 0;
        bool lostAtCallback = false;
    } reentry;

    void OnReadbackFailure(const dyf::Platform::LogRecord& record, void*)
    {
        if(record.category != "Vulkan" || record.message.find("vkQueueWaitIdle(readback)") == std::string::npos)
            return;
        ++reentry.calls;
        reentry.lostAtCallback = reentry.device->IsLost();
        dyf::RHI::TextureReadback nested;
        Check(!reentry.device->ReadTexture(reentry.source, nested), "callback cannot submit another readback");
    }
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(
    VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* allocator, VkBuffer* buffer)
{
    const auto result = Native<PFN_vkCreateBuffer>("vkCreateBuffer")(device, info, allocator, buffer);
    if(observed.capture && result == VK_SUCCESS) observed.buffer = *buffer;
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* allocator, VkDeviceMemory* memory)
{
    const auto result = Native<PFN_vkAllocateMemory>("vkAllocateMemory")(device, info, allocator, memory);
    if(observed.capture && result == VK_SUCCESS) observed.memory = *memory;
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
    if(observed.capture && observed.failBind) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return Native<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device, buffer, memory, offset);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(
    VkDevice device, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks* allocator, VkCommandPool* pool)
{
    const auto result = Native<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device, info, allocator, pool);
    if(observed.capture && result == VK_SUCCESS) observed.pool = *pool;
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(
    VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
{
    if(observed.capture && observed.submitResult != VK_SUCCESS) return observed.submitResult;
    return Native<PFN_vkQueueSubmit>("vkQueueSubmit")(queue, count, submits, fence);
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue)
{
    const auto result = Native<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue);
    if(result == VK_SUCCESS && observed.capture && observed.waitResult != VK_SUCCESS)
    {
        ++observed.injectedWaits;
        return observed.waitResult;
    }
    return result;
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
    const auto result = Native<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device);
    if(result == VK_SUCCESS && observed.failShutdown) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return result;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator)
{
    if(buffer && buffer == observed.buffer) ++observed.buffersDestroyed;
    Native<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device, buffer, allocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeMemory(
    VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator)
{
    if(memory && memory == observed.memory) ++observed.memoryFreed;
    Native<PFN_vkFreeMemory>("vkFreeMemory")(device, memory, allocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator)
{
    if(pool && pool == observed.pool) ++observed.poolsDestroyed;
    Native<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device, pool, allocator);
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks* allocator)
{
    if(image && image == observed.source) ++observed.sourcesDestroyed;
    Native<PFN_vkDestroyImage>("vkDestroyImage")(device, image, allocator);
}

int main(int argc, char** argv) try
{
    using namespace dyf::RHI;
    const std::string scenario = argc == 2 ? argv[1] : "wait-host-oom";
    const bool success = scenario == "success";
    const bool preSubmit = scenario == "pre-submit-failure";
    const bool submitOom = scenario == "submit-oom";
    const bool abandon = scenario == "shutdown-unknown";
    const bool callback = scenario == "wait-callback-reentry";
    Require(success || preSubmit || submitOom || abandon || callback || scenario == "wait-host-oom" ||
        scenario == "wait-device-oom" || scenario == "wait-device-lost", "unknown scenario");

    DeviceDesc desc{};
    desc.enableValidation = true;
    std::unique_ptr<IDevice> device(IDevice::Create(desc));
    Require(device != nullptr, "create Vulkan device with validation");
    TextureDesc textureDesc{};
    textureDesc.width = textureDesc.height = 2;
    textureDesc.format = Format::R8G8B8A8_UNORM;
    textureDesc.usage = TextureUsage::ShaderResource;
    auto* texture = device->CreateTexture(textureDesc);
    Require(texture != nullptr, "create texture");
    const std::array<uint8_t, 16> expected = {1,2,3,255, 4,5,6,255, 7,8,9,255, 10,11,12,255};
    auto* commands = device->AcquireCommandList();
    Require(commands != nullptr, "acquire upload commands");
    ResourceBarrierDesc barrier{};
    barrier.texture = texture;
    barrier.before = ResourceState::Undefined;
    barrier.after = ResourceState::CopyDestination;
    commands->ResourceBarrier(&barrier, 1);
    Require(device->UpdateTexture(*commands, texture, 0, 0, expected.data(), 16, 8, 16), "record upload");
    barrier.before = ResourceState::CopyDestination;
    barrier.after = ResourceState::ShaderResource;
    commands->ResourceBarrier(&barrier, 1);
    Require(commands->Close(), "close upload");
    FenceHandle completion;
    Require(device->Submit({&commands, 1, nullptr, 0}, completion), "submit upload");
    Require(device->Wait(completion, UINT64_MAX), "complete upload");
    device->DestroyCommandList(commands);
    Require(device->WaitIdle(), "drain upload ownership");
    auto* native = dynamic_cast<dyf::Backends::VulkanTexture*>(texture);
    Require(native != nullptr, "Vulkan texture type");
    observed.source = native->GetImage();
    observed.failBind = preSubmit;
    observed.submitResult = submitOom ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_SUCCESS;
    if(!success && !preSubmit && !submitOom)
        observed.waitResult = scenario == "wait-device-lost" ? VK_ERROR_DEVICE_LOST :
            scenario == "wait-device-oom" ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_OUT_OF_HOST_MEMORY;
    TextureReadback output;
    output.width = 91;
    output.pixels = {17, 19};
    if(callback)
    {
        reentry.device = device.get();
        reentry.source = texture;
        dyf::Platform::Log::SetCallback(OnReadbackFailure);
    }
    observed.capture = true;
    const bool read = device->ReadTexture(texture, output);
    observed.capture = false;
    dyf::Platform::Log::SetCallback(nullptr);
    if(callback)
    {
        Check(reentry.calls == 1, "failure invokes callback once");
        Check(reentry.lostAtCallback, "device faults before invoking failure callback");
    }
    Check(read == success, "readback return value");
    const bool completedOrNotSubmitted = success || preSubmit || submitOom;
    if(success)
    {
        Check(output.width == 2 && output.height == 2 && output.rowPitch == 8 && output.pixels.size() == 16,
            "successful readback dimensions");
        Check(output.pixels.size() == expected.size() &&
            std::memcmp(output.pixels.data(), expected.data(), expected.size()) == 0, "successful readback pixels");
        Check(native->GetState(0, 0) == ResourceState::ShaderResource, "readback preserves resource state");
    }
    else
        Check(output.width == 91 && output.pixels == std::vector<uint8_t>({17,19}), "failed readback preserves output");
    Require(observed.buffer && observed.memory, "readback allocation hooks observed real resources");
    if(!preSubmit) Require(observed.pool != VK_NULL_HANDLE, "readback command pool observed");
    if(!completedOrNotSubmitted)
        Check(observed.injectedWaits == 1, "completion failure injected after successful real submission");
    Check(observed.buffersDestroyed == (completedOrNotSubmitted ? 1u : 0u), "buffer lives until completion is established");
    Check(observed.memoryFreed == (completedOrNotSubmitted ? 1u : 0u), "memory lives until completion is established");
    Check(observed.poolsDestroyed == (completedOrNotSubmitted && !preSubmit ? 1u : 0u), "command pool lives until completion is established");
    Check(device->IsLost() == !completedOrNotSubmitted, "uncertain completion faults device; pre-submit failure does not");
    device->DestroyTexture(texture);
    Check(observed.sourcesDestroyed == (completedOrNotSubmitted ? 1u : 0u), "source survives caller ownership release on uncertain completion");
    observed.failShutdown = abandon;
    device.reset();
    const unsigned released = abandon ? 0u : 1u;
    Check(observed.buffersDestroyed == released, "shutdown releases buffer only with completion evidence");
    Check(observed.memoryFreed == released, "shutdown releases memory only with completion evidence");
    Check(observed.poolsDestroyed == (preSubmit ? 0u : released), "shutdown releases command pool exactly once");
    Check(observed.sourcesDestroyed == released, "shutdown releases retained source exactly once");
    std::printf("Vulkan readback %s: %s (%u failures)\n", scenario.c_str(), failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
catch(const std::exception& error)
{
    std::fprintf(stderr, "SETUP/EXECUTION ERROR: %s\n", error.what());
    return 2;
}
