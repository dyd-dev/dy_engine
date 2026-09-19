#include <dyf/Core/ThreadPool.h>
#include <dyf/RHI.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

namespace RHI = dyf::RHI;
using dyf::Core::ThreadPool;

namespace
{
void Check(bool value, const char* message)
{
    if(!value) throw std::runtime_error(message);
}

struct Lists
{
    RHI::IDevice& device;
    std::vector<RHI::ICommandList*> values;
    explicit Lists(RHI::IDevice& owner) : device(owner) {}
    ~Lists() { for(auto* list : values) device.DestroyCommandList(list); }
    void Submit()
    {
        RHI::FenceHandle fence;
        Check(device.Submit({values.data(), uint32_t(values.size()), nullptr, 0}, fence), "Submit failed");
        Check(bool(fence), "Missing completion fence");
        Check(device.Wait(fence, 5000000000ull), "Fence did not complete");
    }
};

RHI::BufferHandle Buffer(RHI::IDevice& device)
{
    RHI::BufferDesc desc;
    desc.size = 64;
    desc.usage = RHI::BufferUsage::Vertex;
    desc.initialState = RHI::ResourceState::CopyDestination;
    auto* result = device.CreateBuffer(desc);
    Check(result != nullptr, "Buffer creation failed");
    return result;
}

// A bounded rendezvous proves overlap without relying on thread scheduling luck.
struct Rendezvous
{
    std::mutex mutex;
    std::condition_variable ready;
    unsigned arrived = 0;
    void Meet()
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++arrived;
        ready.notify_all();
        Check(ready.wait_for(lock, std::chrono::seconds(5), [&] { return arrived == 2; }),
            "Independent passes did not overlap on two workers");
    }
};

void TestPool()
{
    std::atomic<unsigned> finished{0};
    {
        ThreadPool pool(2);
        auto failed = pool.Enqueue([] { throw std::runtime_error("expected task failure"); });
        bool propagated = false;
        try { failed.get(); } catch(const std::runtime_error&) { propagated = true; }
        Check(propagated, "ThreadPool swallowed a task exception");
        auto selfWait = pool.Enqueue([&] {
            bool rejected = false;
            try { pool.WaitAll(); } catch(const std::logic_error&) { rejected = true; }
            Check(rejected, "Worker waiting on its own pool was not rejected");
        });
        selfWait.get();
        for(unsigned i = 0; i < 32; ++i) (void)pool.Enqueue([&] { ++finished; });
    }
    Check(finished == 32, "ThreadPool destruction lost queued tasks");
}

void TestGlobalStages(RHI::IDevice& device, ThreadPool& pool)
{
    RHI::RenderGraph graph;
    Rendezvous first;
    std::atomic<unsigned> before{0}, after{0};
    std::atomic<bool> barrier{false};
    std::mutex threadMutex;
    std::set<std::thread::id> threads;
    for(unsigned i = 0; i < 2; ++i)
        graph.AddPass("before").SetExecute([&](RHI::ICommandList*) {
            first.Meet();
            { std::lock_guard<std::mutex> lock(threadMutex); threads.insert(std::this_thread::get_id()); }
            ++before;
        });
    graph.AddPass("global-without-resources").GlobalBarrier().SetExecute([&](RHI::ICommandList*) {
        Check(before == 2, "Global pass ran before predecessors completed");
        barrier = true;
    });
    for(unsigned i = 0; i < 2; ++i)
        graph.AddPass("after").SetExecute([&](RHI::ICommandList*) {
            Check(barrier, "Pass ran before its global boundary");
            ++after;
        });
    Check(graph.Compile(), "Global graph compile failed");
    const auto& stages = graph.GetExecutionStages();
    Check(stages.size() == 3 && stages[0].passIndices.size() == 2 &&
        stages[1].passIndices.size() == 1 && stages[2].passIndices.size() == 2, "Incorrect global dependency stages");
    Lists lists(device);
    Check(graph.ExecuteParallel(&device, &pool, lists.values), "Global parallel recording failed");
    Check(after == 2 && threads.size() == 2 && !threads.count(std::this_thread::get_id()), "No actual worker recording");
    Check(std::set<RHI::ICommandList*>(lists.values.begin(), lists.values.end()).size() == lists.values.size(),
        "Different passes reused a command list");
    lists.Submit();
}

void TestCopies(RHI::IDevice& device, ThreadPool* pool, unsigned count)
{
    RHI::ResourceScope resources(device);
    RHI::RenderGraph graph;
    std::vector<RHI::BufferHandle> buffers;
    std::atomic<unsigned> callbacks{0};
    for(unsigned i = 0; i < count; ++i)
    {
        auto* buffer = resources.Keep(Buffer(device));
        buffers.push_back(buffer);
        const auto imported = graph.ImportBuffer(std::to_string(i), buffer,
            RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer);
        graph.AddPass("upload").Write(imported, RHI::ResourceState::CopyDestination)
            .SetExecute([&, buffer, i](RHI::ICommandList* list) {
                const uint32_t data[] = {i, i + 1, i + 2, i + 3};
                Check(device.UpdateBuffer(*list, buffer, 0, data, sizeof(data)), "Worker upload failed");
                ++callbacks;
            });
    }
    // Same-state CopyDestination barriers, then final VertexBuffer transitions.
    graph.AddPass("memory-boundary").GlobalBarrier().SetExecute([](RHI::ICommandList*) {});
    Check(graph.Compile(), "Copy graph compile failed");
    Lists lists(device);
    Check(graph.ExecuteParallel(&device, pool, lists.values), "Copy graph recording failed");
    Check(callbacks == count && lists.values.size() == count + 3, "Passes were skipped or slots were reused");
    lists.Submit();
    RHI::RenderGraph verify;
    for(unsigned i = 0; i < count; ++i)
    {
        const auto handle = verify.ImportBuffer(std::to_string(i), buffers[i],
            RHI::ResourceState::VertexBuffer, RHI::ResourceState::VertexBuffer);
        verify.AddPass("read-final-state").Read(handle, RHI::ResourceState::VertexBuffer)
            .SetExecute([](RHI::ICommandList*) {});
    }
    Check(verify.Compile(), "Final-state graph compile failed");
    Lists checked(device);
    Check(verify.ExecuteParallel(&device, pool, checked.values), "Final-state recording failed");
    checked.Submit();
}

void TestSubmissionOrder(RHI::IDevice& device, ThreadPool& pool)
{
    RHI::ResourceScope resources(device);
    auto* x = resources.Keep(Buffer(device));
    auto* y = resources.Keep(Buffer(device));
    RHI::RenderGraph graph;
    const auto hx = graph.ImportBuffer("x", x, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer);
    const auto hy = graph.ImportBuffer("y", y, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer);
    graph.AddPass("write-x").Write(hx, RHI::ResourceState::CopyDestination).SetExecute([](RHI::ICommandList*) {});
    graph.AddPass("transition-y-after-x").Read(hx, RHI::ResourceState::VertexBuffer)
        .Read(hy, RHI::ResourceState::VertexBuffer).SetExecute([](RHI::ICommandList*) {});
    graph.AddPass("read-y").Read(hy, RHI::ResourceState::VertexBuffer).SetExecute([](RHI::ICommandList*) {});
    Check(graph.Compile(), "Submission-order compile failed");
    Check(graph.GetExecutionStages()[0].passIndices == std::vector<uint32_t>({0, 2}), "Expected independent record stage");
    Lists lists(device);
    Check(graph.ExecuteParallel(&device, &pool, lists.values), "Submission-order recording failed");
    lists.Submit(); // Stage order {0,2,1} would fail: pass 1 owns y's state transition.
}

void TestFailureRecovery(RHI::IDevice& device, ThreadPool& pool)
{
    RHI::ResourceScope resources(device);
    auto* buffer = resources.Keep(Buffer(device));
    RHI::RenderGraph graph;
    const auto imported = graph.ImportBuffer("buffer", buffer,
        RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer);
    Rendezvous overlap;
    std::atomic<bool> finished{false};
    graph.AddPass("throws").SetExecute([&](RHI::ICommandList*) {
        overlap.Meet();
        throw std::runtime_error("expected callback failure");
    });
    graph.AddPass("still-recording").Write(imported, RHI::ResourceState::CopyDestination)
        .SetExecute([&](RHI::ICommandList* list) {
            overlap.Meet();
            const uint32_t data = 17;
            Check(device.UpdateBuffer(*list, buffer, 0, &data, sizeof(data)), "Upload after peer failure failed");
            finished = true;
        });
    Check(graph.Compile(), "Failure graph compile failed");
    Lists lists(device);
    bool caught = false;
    try { (void)graph.ExecuteParallel(&device, &pool, lists.values); }
    catch(const std::runtime_error&) { caught = true; }
    Check(caught && finished && lists.values.empty(), "Failure did not join/clean all workers");

    graph.Reset();
    const auto recovered = graph.ImportBuffer("buffer", buffer,
        RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer);
    auto& pass = graph.AddPass("recovery").Write(recovered, RHI::ResourceState::CopyDestination);
    pass.SetExecute([](RHI::ICommandList* list) { list->BeginRendering({}); });
    Check(graph.Compile(), "Invalid callback compile failed");
    Check(!graph.ExecuteParallel(&device, &pool, lists.values) && lists.values.empty(), "Invalid recording escaped");
    pass.SetExecute([](RHI::ICommandList*) {});
    Check(!graph.ExecuteParallel(&device, &pool, lists.values), "Stale compile was accepted");
    Check(graph.Compile() && graph.ExecuteParallel(&device, &pool, lists.values), "Recovery failed");
    Check(!graph.ExecuteParallel(&device, &pool, lists.values), "Nonempty output accepted");
    lists.Submit(); // Earlier failures must not have submitted any state transition.
}

void TestLifetimeAndBoundaries(RHI::IDevice& device, ThreadPool& pool)
{
    auto* buffer = Buffer(device);
    RHI::RenderGraph graph;
    const auto imported = graph.ImportBuffer("retained", buffer,
        RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer);
    graph.AddPass("release-owner").GlobalBarrier().SetExecute([&](RHI::ICommandList*) { device.DestroyBuffer(buffer); });
    graph.AddPass("use-retained-import").Write(imported, RHI::ResourceState::CopyDestination)
        .SetExecute([&](RHI::ICommandList* list) {
            const uint32_t data = 42;
            Check(device.UpdateBuffer(*list, buffer, 0, &data, sizeof(data)), "Retained import lost after owner release");
        });
    Check(graph.Compile(), "Retained graph compile failed");
    Lists retained(device);
    Check(graph.ExecuteParallel(&device, &pool, retained.values), "Retained graph recording failed");
    retained.Submit();

    RHI::ResourceScope resources(device);
    auto* live = resources.Keep(Buffer(device));
    RHI::RenderGraph wrong;
    (void)wrong.ImportBuffer("wrong-initial", live, RHI::ResourceState::VertexBuffer, RHI::ResourceState::VertexBuffer);
    Check(wrong.Compile(), "Empty graph compile failed");
    Lists invalid(device);
    Check(wrong.ExecuteParallel(&device, &pool, invalid.values), "Deferred boundary recording failed");
    RHI::FenceHandle fence;
    Check(!device.Submit({invalid.values.data(), uint32_t(invalid.values.size()), nullptr, 0}, fence) && !fence,
        "Wrong initial state escaped deferred submission validation");

    auto* dead = Buffer(device);
    RHI::RenderGraph stale;
    (void)stale.ImportBuffer("destroyed", dead, RHI::ResourceState::CopyDestination, RHI::ResourceState::VertexBuffer);
    bool called = false;
    stale.AddPass("must-not-run").SetExecute([&](RHI::ICommandList*) { called = true; });
    Check(stale.Compile(), "Destroyed graph compile failed");
    device.DestroyBuffer(dead);
    Lists empty(device);
    Check(!stale.ExecuteParallel(&device, &pool, empty.values) && !called && empty.values.empty(),
        "Imports were not validated before callbacks");
}

void TestTextures(RHI::IDevice& device, ThreadPool& pool)
{
    RHI::ResourceScope resources(device);
    RHI::RenderGraph graph;
    std::vector<RHI::TextureHandle> textures;
    for(unsigned i = 0; i < 2; ++i)
    {
        RHI::TextureDesc desc;
        desc.width = desc.height = 4;
        desc.format = RHI::Format::R8G8B8A8_UNORM;
        desc.usage = RHI::TextureUsage::ShaderResource;
        auto* texture = resources.Keep(device.CreateTexture(desc));
        textures.push_back(texture);
        const auto imported = graph.ImportTexture(std::to_string(i), texture,
            RHI::ResourceState::Undefined, RHI::ResourceState::ShaderResource);
        graph.AddPass("texture-upload").Write(imported, RHI::ResourceState::CopyDestination)
            .SetExecute([&, texture, i](RHI::ICommandList* list) {
                const std::vector<uint8_t> pixels(64, uint8_t(50 + i));
                Check(device.UpdateTexture(*list, texture, 0, 0, pixels.data(), 64, 16, 64), "Texture upload failed");
            });
    }
    graph.AddPass("texture-memory-boundary").GlobalBarrier().SetExecute([](RHI::ICommandList*) {});
    Check(graph.Compile(), "Texture graph compile failed");
    Lists lists(device);
    Check(graph.ExecuteParallel(&device, &pool, lists.values), "Texture graph recording failed");
    lists.Submit();
    if(device.Supports(RHI::Feature::Rasterization))
    {
        for(unsigned i = 0; i < 2; ++i)
        {
            RHI::TextureReadback pixels;
            Check(device.ReadTexture(textures[i], pixels), "Native texture readback failed");
            for(unsigned y = 0; y < 4; ++y)
                for(unsigned x = 0; x < 16; ++x)
                    Check(pixels.pixels[size_t(y) * pixels.rowPitch + x] == uint8_t(50 + i), "GPU upload differs from recorded input");
        }
    }
}
}

int main()
{
    try
    {
        TestPool();
        std::unique_ptr<RHI::IDevice> device(RHI::IDevice::Create({}));
        Check(bool(device), "Device creation failed");
        ThreadPool pool(2), one(1);
        TestGlobalStages(*device, pool);
        TestCopies(*device, nullptr, 3);
        TestCopies(*device, &one, 3);
        TestCopies(*device, &pool, 70);
        TestSubmissionOrder(*device, pool);
        TestFailureRecovery(*device, pool);
        TestLifetimeAndBoundaries(*device, pool);
        TestTextures(*device, pool);
        // Nested use of the same pool falls back instead of waiting on itself.
        auto nested = pool.Enqueue([&] { TestCopies(*device, &pool, 2); });
        nested.get();
        Check(device->WaitIdle(), "Final WaitIdle failed");
        std::puts("RenderGraph contracts passed: real worker overlap, ordering, >64 lists, barriers, lifetime, failures and fences.");
        return 0;
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "RenderGraph contract failure: %s\n", error.what());
        return 1;
    }
}
