#include "dyf/RHI.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>
#if !defined(DY_UPLOAD_VULKAN_NATIVE)
#include "Backends/Vulkan/VulkanCommandList.h"
#include "Backends/Vulkan/VulkanResources.h"
namespace dyf::Backends {
struct VulkanObjectDeleter {
    template<class T> void operator()(T* object) const { delete object; }
};
}

// The actual command recorder/native page code runs with CPU Vulkan boundaries.
// No loader function receives the deterministic fake handles used below.
namespace {
using namespace dyf::Backends;
unsigned failures = 0, stagingCreates = 0, maps = 0, unmaps = 0;
uintptr_t nextHandle = 100;
bool coherent = true;
int failStage = 0;
std::map<VkBuffer, uint64_t> buffers;
std::map<VkDeviceMemory, std::vector<uint8_t>> memories;
std::map<VkBuffer, VkDeviceMemory> bindings;
struct Copy { VkBuffer source; VkBufferCopy region; };
struct ImageCopy { VkBuffer source; VkBufferImageCopy region; };
std::vector<Copy> copies;
std::vector<ImageCopy> images;
template<class T> T Handle() { return reinterpret_cast<T>(++nextHandle); }
void Check(bool value, const char* message) {
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
VulkanContext Context() {
    VulkanContext value{};
    value.device = Handle<VkDevice>(); value.physicalDevice = Handle<VkPhysicalDevice>();
    value.queueIndices.graphicsFamily = value.queueIndices.presentFamily = 0;
    return value;
}
const uint8_t* Data(VkBuffer buffer, uint64_t offset) {
    return memories.at(bindings.at(buffer)).data() + offset;
}
}
extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* output) { *output = Handle<VkImage>(); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements* value) { *value = {128,4,1}; }
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* output) { *output = Handle<VkImageView>(); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* value) { *value = {}; }
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice,VkFormat,VkImageType,VkImageTiling,
    VkImageUsageFlags,VkImageCreateFlags,VkImageFormatProperties* value) {
    *value={}; value->maxExtent={64,64,1}; value->maxMipLevels=7;
    value->maxArrayLayers=4; value->sampleCounts=VK_SAMPLE_COUNT_1_BIT;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* value) {
    *count = 1; if (value) { *value = {}; value->queueFlags = VK_QUEUE_GRAPHICS_BIT; }
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* value) {
    *value = {}; value->memoryTypeCount = 1;
    value->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        (coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* output) {
    if (failStage == 1) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *output = Handle<VkBuffer>(); buffers[*output] = info->size;
    if (info->usage == VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ++stagingCreates;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) { buffers.erase(buffer); bindings.erase(buffer); }
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer buffer, VkMemoryRequirements* value) { *value = { buffers.at(buffer), 4, 1 }; }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* output) {
    if (failStage == 2) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *output = Handle<VkDeviceMemory>(); memories[*output].resize(static_cast<size_t>(info->allocationSize)); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) { memories.erase(memory); }
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize) {
    if (failStage == 3) return VK_ERROR_OUT_OF_HOST_MEMORY;
    bindings[buffer] = memory; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** output) {
    if (failStage == 4) return VK_ERROR_MEMORY_MAP_FAILED;
    ++maps; *output = memories.at(memory).data(); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory) { ++unmaps; }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* output) { *output = Handle<VkCommandPool>(); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* output) { *output = Handle<VkCommandBuffer>(); return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer, VkBuffer source, VkBuffer, uint32_t count, const VkBufferCopy* regions) {
    for (uint32_t i = 0; i < count; ++i) copies.push_back({source, regions[i]});
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer source, VkImage, VkImageLayout, uint32_t count, const VkBufferImageCopy* regions) {
    for (uint32_t i = 0; i < count; ++i) images.push_back({source, regions[i]});
}
}

int main() try {
    const auto context = Context();
    dyf::RHI::BufferDesc desc{}; desc.size = 1024; desc.usage = dyf::RHI::BufferUsage::Vertex;
    desc.initialState = dyf::RHI::ResourceState::CopyDestination;
    std::unique_ptr<VulkanBuffer, VulkanObjectDeleter> target(new VulkanBuffer(context, desc));
    {
        std::unique_ptr<VulkanCommandList, VulkanObjectDeleter> command(new VulkanCommandList(context));
        const auto before = stagingCreates;
        uint8_t payload[7] = {1,2,3,4,5,6,7};
        for (unsigned i = 0; i < 8; ++i) Check(command->RecordBufferUpdate(*target, i * 8, payload, 7), "small upload records");
        std::memset(payload, 99, sizeof(payload));
        Check(stagingCreates - before == 1, "small uploads share one native page");
        for (const auto& copy : copies) {
            Check(copy.region.size == 7 && Data(copy.source, copy.region.srcOffset)[0] == 1 &&
                Data(copy.source, copy.region.srcOffset)[6] == 7, "immediate captured payload survives caller mutation");
        }
    }
    Check(maps == unmaps, "discard unmaps all native staging pages");
    Check(buffers.size() == 1 && memories.size() == 1, "discard releases staging objects");
    for (int stage = 1; stage <= 4; ++stage) {
        std::unique_ptr<VulkanCommandList, VulkanObjectDeleter> command(new VulkanCommandList(context));
        failStage = stage; const uint32_t payload = 42;
        Check(!command->RecordBufferUpdate(*target, 0, &payload, 4), "native staging failure returns false");
        failStage = 0;
        Check(buffers.size() == 1 && memories.size() == 1, "failed allocation rolls back native objects");
    }
    {
        std::unique_ptr<VulkanCommandList, VulkanObjectDeleter> command(new VulkanCommandList(context)); coherent = false; const uint32_t payload = 42;
        Check(!command->RecordBufferUpdate(*target, 0, &payload, 4), "noncoherent-only memory is rejected safely"); coherent = true;
    }
    {
        auto pool = CreateVulkanUploadPool(context);
        const uint32_t payload = 7;
        {
            std::unique_ptr<VulkanCommandList, VulkanObjectDeleter> first(new VulkanCommandList(context, pool));
            Check(first->RecordBufferUpdate(*target, 0, &payload, 4), "first lease records");
            const auto firstSource = copies.back().source;
            std::unique_ptr<VulkanCommandList, VulkanObjectDeleter> second(new VulkanCommandList(context, pool));
            Check(second->RecordBufferUpdate(*target, 4, &payload, 4), "overlapping lease records");
            Check(copies.back().source != firstSource, "live commands never share writable page");
            Check(pool->GetStatistics().inUseBytes >= 2 * 256 * 1024, "live lease bytes stay in use");
        }
        const auto warmed = pool->GetStatistics().allocationCount;
        for (unsigned frame = 0; frame < 24; ++frame) {
            std::unique_ptr<VulkanCommandList, VulkanObjectDeleter> command(new VulkanCommandList(context, pool));
            Check(command->RecordBufferUpdate(*target, 0, &payload, 4), "reused page records");
        }
        const auto stats = pool->GetStatistics();
        Check(stats.allocationCount == warmed && stats.reuseCount >= 24, "native allocation count plateaus after warmup");
        Check(stats.inUseBytes == 0 && stats.cachedBytes > 0, "discarded lease returns to bounded cache");
        // An oversize upload cannot leave idle native pages beyond the budget.
        {
            VulkanUploadArena arena(pool);
            Check(static_cast<bool>(arena.Allocate(9 * 1024 * 1024)), "oversize upload allocates");
        }
        Check(pool->GetStatistics().cachedBytes <= 8 * 1024 * 1024, "idle oversize page respects cache budget");
    }
    for(const bool reuse:{false,true})for(const bool suballocate:{false,true}) {
        auto pool=CreateVulkanUploadPool(context);pool->SetPolicy({reuse,suballocate});
        for(unsigned frame=0;frame<3;++frame) {
            const auto begin=copies.size();
            std::unique_ptr<VulkanCommandList,VulkanObjectDeleter> command(new VulkanCommandList(context,pool));
            uint8_t payload[7]={1,2,3,4,5,6,7};
            for(unsigned i=0;i<8;++i)Check(command->RecordBufferUpdate(*target,i*8,payload,7),"policy upload records");
            std::memset(payload,99,sizeof(payload));
            for(size_t i=begin;i<copies.size();++i) {
                const auto& copy=copies[i];
                Check(copy.region.srcOffset%16==0 && copy.region.size==7 && Data(copy.source,copy.region.srcOffset)[0]==1 &&
                    Data(copy.source,copy.region.srcOffset)[6]==7,"every policy preserves alignment and captured payload");
                if(i>begin)Check((copy.source==copies[begin].source)==suballocate,"suballocation alone changes native page sharing");
            }
        }
        const uint64_t pages=suballocate?1:8;
        const auto stats=pool->GetStatistics();
        Check(stats.requestCount==24 && stats.requestedBytes==168,"policies preserve requested work");
        Check(stats.allocationCount==pages*(reuse?1:3) && stats.reuseCount==pages*(reuse?2:0),"independent policies change native allocation and reuse counts");
        Check(stats.inUseBytes==0 && stats.cachedBytes==(reuse?pages*256*1024:0),"policy discard respects current cache setting");
    }
    {
        auto pool=CreateVulkanUploadPool(context);
        std::unique_ptr<VulkanCommandList,VulkanObjectDeleter> old(new VulkanCommandList(context,pool));
        pool->SetPolicy({false,false});
        const uint32_t payload=42;
        Check(old->RecordBufferUpdate(*target,0,&payload,4),"old policy first upload");const auto oldSource=copies.back().source;
        Check(old->RecordBufferUpdate(*target,4,&payload,4) && copies.back().source==oldSource,"native command snapshots policy before its first upload");
        std::unique_ptr<VulkanCommandList,VulkanObjectDeleter> fresh(new VulkanCommandList(context,pool));
        Check(fresh->RecordBufferUpdate(*target,8,&payload,4),"new policy first upload");const auto freshSource=copies.back().source;
        Check(fresh->RecordBufferUpdate(*target,12,&payload,4) && copies.back().source!=freshSource,"new command uses independent pages");
    }
    {
        dyf::RHI::TextureDesc desc{}; desc.width = 3; desc.height = 2;
        desc.mipLevels = desc.depthOrArraySize = 1; desc.format = dyf::RHI::Format::R8G8B8A8_UNORM;
        desc.usage = dyf::RHI::TextureUsage::ShaderResource;
        std::unique_ptr<VulkanTexture, VulkanObjectDeleter> texture(new VulkanTexture(context, desc));
        std::unique_ptr<VulkanCommandList, VulkanObjectDeleter> command(new VulkanCommandList(context));
        const uint32_t prefix = 42;
        Check(command->RecordBufferUpdate(*target, 0, &prefix, 4), "prefix forces texture nonzero source offset");
        uint8_t padded[32]{};
        for (unsigned row = 0; row < 2; ++row) for (unsigned col = 0; col < 12; ++col) padded[row * 16 + col] = static_cast<uint8_t>(row * 20 + col);
        Check(command->RecordTextureUpdate(*texture, 0, 0, padded, 28, 16, 32), "whole texel row padding records");
        auto image = images.back();
        Check(image.region.bufferOffset != 0 && image.region.bufferOffset % 4 == 0 && image.region.bufferRowLength == 4, "texture carries aligned nonzero offset and texel row pitch");
        Check(Data(image.source, image.region.bufferOffset)[16] == 20, "whole texel padding retained");
        uint8_t odd[26]{};
        for (unsigned row = 0; row < 2; ++row) for (unsigned col = 0; col < 12; ++col) odd[row * 13 + col] = static_cast<uint8_t>(row * 20 + col);
        Check(command->RecordTextureUpdate(*texture, 0, 0, odd, 25, 13, 26), "byte row padding records");
        image = images.back();
        Check(image.region.bufferRowLength == 0 && Data(image.source, image.region.bufferOffset)[12] == 20 &&
            Data(image.source, image.region.bufferOffset)[23] == 31, "byte row padding removed in mapped page");
        desc.width = 1; desc.format = dyf::RHI::Format::R32G32B32_FLOAT;
        std::unique_ptr<VulkanTexture, VulkanObjectDeleter> rgb(new VulkanTexture(context, desc));
        Check(command->RecordTextureUpdate(*rgb, 0, 0, odd, 25, 13, 26), "RGB32 padded row records");
        image = images.back();
        Check(image.region.bufferOffset % 12 == 0 && image.region.bufferOffset % 4 == 0, "RGB32 source offset aligns to 12-byte texel block");
        Check(Data(image.source, image.region.bufferOffset)[12] == 20, "RGB32 block padding preserves row data");
    }
    Check(maps == unmaps && buffers.size() == 1 && memories.size() == 1, "all shared pools and texture leases release native objects");
    std::printf("Upload Vulkan CPU: %u failures, creates=%u maps=%u unmaps=%u\n", failures, stagingCreates, maps, unmaps);
    return failures ? 1 : 0;
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 2; }
#else
#include "dyf/RHI/Readback.h"
#include "dyf/Platform/Log.h"
#include <atomic>
#include <thread>
namespace {
unsigned failures = 0;
void Check(bool value, const char* message) {
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct StatisticsMonitor {
    dyf::RHI::IDevice& device;
    std::atomic<bool> stop{false};
    std::thread worker;
    explicit StatisticsMonitor(dyf::RHI::IDevice& value):device(value),worker([this] {
        while(!stop.load(std::memory_order_relaxed)) {
            (void)device.GetUploadMemoryStatistics();
            std::this_thread::yield();
        }
    }) {}
    ~StatisticsMonitor() { stop.store(true,std::memory_order_relaxed);worker.join(); }
};
#ifdef DY_LIVE_PROFILING_ENABLED
void NativePolicyCombinations(dyf::RHI::IDevice& device,dyf::RHI::BufferHandle buffer,dyf::RHI::TextureHandle texture)
{
    namespace D=dyf::Diagnostics;
    using namespace dyf::RHI;
    const auto id=D::FindLiveObject(&device),source=D::AllocateLiveId();
    Require(id&&source,"native policy source");
    {D::LiveFrameScope frame(id,source);device.PrepareLiveControls(frame.GetKey());}
    D::LiveId controls[2]{};
    {const auto snapshot=D::ReadLiveSnapshot();for(const auto& control:snapshot->controls)if(control.source==source){
        if(control.key=="rhi.uploadPageReuse")controls[0]=control.id;
        if(control.key=="rhi.uploadSuballocation")controls[1]=control.id;
    }}
    Require(controls[0]&&controls[1],"native upload controls registered");
    for(const bool reuse:{false,true})for(const bool suballocate:{false,true})
    {
        const D::LiveId requests[]={D::RequestLiveControl(controls[0],reuse),D::RequestLiveControl(controls[1],suballocate)};
        Require(requests[0]&&requests[1],"native policy requests");
        for(unsigned iteration=0;iteration<3;++iteration)
        {
            const auto before=device.GetUploadMemoryStatistics();
            const uint32_t rowPitch=iteration%2?13:16;
            std::vector<uint8_t> sourceBytes(rowPitch*2,0xee),expected(24);
            for(unsigned row=0;row<2;++row)for(unsigned col=0;col<12;++col)
                sourceBytes[row*rowPitch+col]=expected[row*12+col]=uint8_t(iteration+row*20+col);
            D::LiveFrameKey key;
            {
                D::LiveFrameScope frame(id,source);key=frame.GetKey();device.PrepareLiveControls(key);
                auto* commands=device.AcquireCommandList();Require(commands!=nullptr,"native policy command");
                for(unsigned i=0;i<8;++i){const uint32_t value=iteration+i;Require(device.UpdateBuffer(*commands,buffer,i*4,&value,4),"native policy buffer upload");}
                ResourceBarrierDesc barrier{};barrier.texture=texture;barrier.before=ResourceState::ShaderResource;barrier.after=ResourceState::CopyDestination;
                commands->ResourceBarrier(&barrier,1);
                Require(device.UpdateTexture(*commands,texture,0,0,sourceBytes.data(),rowPitch+12,rowPitch,rowPitch*2),"native policy padded texture");
                std::fill(sourceBytes.begin(),sourceBytes.end(),uint8_t{99});
                barrier.before=ResourceState::CopyDestination;barrier.after=ResourceState::ShaderResource;commands->ResourceBarrier(&barrier,1);
                Require(commands->Close(),"native policy close");FenceHandle completion;
                Require(device.Submit({&commands,1,nullptr,0},completion),"native policy submit");
                Require(device.Wait(completion,UINT64_MAX),"native policy completion");device.DestroyCommandList(commands);
                Require(device.WaitIdle(),"native policy retirement");
                TextureReadback pixels;Require(device.ReadTexture(texture,pixels),"native policy texture readback");
                Check(pixels.width==3&&pixels.height==2&&pixels.rowPitch==12&&pixels.pixels==expected,"every native policy preserves captured pixels and row padding");
                frame.SetOutcome(D::LiveOutcome::Completed);
            }
            const auto after=device.GetUploadMemoryStatistics();const uint64_t pages=suballocate?1:9;
            const auto allocations=after.allocationCount-before.allocationCount,reuses=after.reuseCount-before.reuseCount;
            Check(allocations+reuses==pages&&after.requestCount-before.requestCount==9,"native policy changes pages, not upload requests");
            if(!reuse)Check(reuses==0&&allocations==pages,"native reuse OFF always creates pages");
            else if(iteration)Check(allocations==0&&reuses==pages,"native reuse ON plateaus after warmup");
            const auto snapshot=D::ReadLiveSnapshot();bool observed=false;
            for(const auto& entry:snapshot->sources)if(entry.source==source){observed=true;
                Check(entry.latest.key==key&&entry.latest.recorded.uploadCalls==9&&entry.latest.recorded.uploadBytes==32+rowPitch+12,"native events keep original source and logical upload counts");
                Check(entry.latest.recorded.uploadPageAllocations==allocations&&entry.latest.recorded.uploadPageReuses==reuses&&
                    entry.latest.recorded.uploadPageAllocationBytes==after.allocationBytes-before.allocationBytes,"source counters describe actual page acquisitions");
            }
            Check(observed,"native policy source snapshot exists");
            if(!iteration)for(const auto request:requests){bool applied=false;for(const auto& status:snapshot->controlStatus)if(status.request==request)applied=status.state==D::ControlState::Applied&&status.appliedFrame==key;Check(applied,"native policy applied at exact source boundary");}
        }
    }
    device.ReleaseLiveControls(source);
}
#endif
}
int main() try {
    using namespace dyf::RHI;
    DeviceDesc desc{}; desc.enableValidation = true;
    std::unique_ptr<IDevice> device(IDevice::Create(desc));
    Require(device != nullptr, "create Vulkan device");
    StatisticsMonitor monitor(*device);
    TextureDesc textureDesc{}; textureDesc.width = 3; textureDesc.height = 2;
    textureDesc.format = Format::R8G8B8A8_UNORM; textureDesc.usage = TextureUsage::ShaderResource;
    TextureHandle texture = device->CreateTexture(textureDesc);
    Require(texture != nullptr, "create native readback texture");
    BufferDesc bufferDesc{}; bufferDesc.size = 1024; bufferDesc.usage = BufferUsage::Vertex;
    bufferDesc.initialState = ResourceState::CopyDestination;
    BufferHandle buffer = device->CreateBuffer(bufferDesc);
    Require(buffer != nullptr, "create native buffer");
    uint64_t warmCount = 0;
    for (unsigned frame = 0; frame < 24; ++frame) {
        auto* command = device->AcquireCommandList(); Require(command != nullptr, "acquire native upload");
        const uint32_t prefix = frame + 1;
        Require(device->UpdateBuffer(*command, buffer, 0, &prefix, 4), "upload buffer prefix");
        ResourceBarrierDesc barrier{}; barrier.texture = texture;
        barrier.before = frame == 0 ? ResourceState::Undefined : ResourceState::ShaderResource;
        barrier.after = ResourceState::CopyDestination; command->ResourceBarrier(&barrier, 1);
        const uint32_t rowPitch = frame % 2 ? 13 : 16;
        std::vector<uint8_t> source(rowPitch * 2, 0xee), expected(24);
        for (unsigned row = 0; row < 2; ++row) for (unsigned col = 0; col < 12; ++col)
            source[row * rowPitch + col] = expected[row * 12 + col] = static_cast<uint8_t>(frame + row * 20 + col);
        Require(device->UpdateTexture(*command, texture, 0, 0, source.data(), rowPitch + 12, rowPitch, rowPitch * 2), "upload native padded texture");
        std::fill(source.begin(), source.end(), 99);
        barrier.before = ResourceState::CopyDestination; barrier.after = ResourceState::ShaderResource;
        command->ResourceBarrier(&barrier, 1); Require(command->Close(), "close native upload");
        FenceHandle fence; Require(device->Submit({&command, 1, nullptr, 0}, fence), "submit native upload");
        Require(device->Wait(fence, UINT64_MAX), "wait native upload"); device->DestroyCommandList(command);
        Require(device->WaitIdle(), "retire native upload");
        TextureReadback readback; Require(device->ReadTexture(texture, readback), "native readback");
        Check(readback.width == 3 && readback.height == 2 && readback.rowPitch == 12 && readback.pixels == expected,
            "native nonzero-offset upload preserves captured pixels and row pitch");
        const auto stats = device->GetUploadMemoryStatistics();
        if (frame == 0) warmCount = stats.allocationCount;
        Check(stats.allocationCount == warmCount && stats.inUseBytes == 0, "native allocations plateau after warmup and fences retire leases");
    }
    ICommandList* overlapping[2] = {device->AcquireCommandList(), device->AcquireCommandList()};
    for (unsigned i = 0; i < 2; ++i) {
        Require(overlapping[i] != nullptr, "acquire overlapping native command"); const uint32_t value = i;
        Require(device->UpdateBuffer(*overlapping[i], buffer, i * 4, &value, 4), "record overlapping native upload");
        Require(overlapping[i]->Close(), "close overlapping upload");
    }
    Require(device->PrepareCommandLists(overlapping, 2), "prepare overlapping native uploads");
    const auto live = device->GetUploadMemoryStatistics();
    Check(live.inUseBytes >= 2 * 256 * 1024 && live.allocationCount == warmCount + 1, "overlapping native commands retain distinct pages");
    FenceHandle fence; Require(device->Submit({overlapping, 2, nullptr, 0}, fence), "submit overlapping native commands");
    Require(device->Wait(fence, UINT64_MAX), "complete overlapping upload");
    for (auto* command : overlapping) device->DestroyCommandList(command);
    Require(device->WaitIdle(), "retire overlapping commands");
    auto* discarded = device->AcquireCommandList(); const uint32_t value = 55;
    Require(discarded && device->UpdateBuffer(*discarded, buffer, 0, &value, 4), "record discarded native upload");
    Require(discarded->Close() && device->PrepareCommandLists(&discarded, 1), "prepare discarded native upload");
    Check(device->GetUploadMemoryStatistics().inUseBytes >= 256 * 1024, "prepared discarded command leases a native page");
    device->DestroyCommandList(discarded);
    const auto final = device->GetUploadMemoryStatistics();
    Check(final.inUseBytes == 0 && final.cachedBytes <= 8 * 1024 * 1024 && final.reuseCount >= 24, "native discarded command returns bounded reusable pages");
    std::printf("Upload Vulkan native: %u failures allocationCount=%llu reuseCount=%llu requestCount=%llu requestedBytes=%llu cachedBytes=%llu peakInUseBytes=%llu\n",
        failures, static_cast<unsigned long long>(final.allocationCount), static_cast<unsigned long long>(final.reuseCount),
        static_cast<unsigned long long>(final.requestCount), static_cast<unsigned long long>(final.requestedBytes),
        static_cast<unsigned long long>(final.cachedBytes), static_cast<unsigned long long>(final.peakInUseBytes));
#ifdef DY_LIVE_PROFILING_ENABLED
    NativePolicyCombinations(*device,buffer,texture);
#endif
    device->DestroyTexture(texture); device->DestroyBuffer(buffer);
    return failures ? 1 : 0;
} catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 2; }
#endif
