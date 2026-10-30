// Exercise the production cache without a GPU; only the native pool API is replaced.
// Standalone GCC build uses -flto -ffunction-sections -Wl,--gc-sections to discard unrelated commands.
#include "../src/Backends/Vulkan/VulkanCommandList.cpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(condition) do { if(!(condition)) { \
    std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition); std::exit(1); \
} } while(false)

namespace
{
    thread_local bool pauseAllocation=false, failAllocation=false;
    std::atomic<bool> allocationEntered{false}, allocationReleased{false};
    std::atomic<unsigned> created{0}, destroyed{0};
    thread_local VkResult createResult=VK_SUCCESS, resetResult=VK_SUCCESS;
    thread_local VkResult allocateResult=VK_SUCCESS, beginResult=VK_SUCCESS;
    struct Pool { std::atomic<bool> leased{false}; unsigned visits=0; };
    Pool* Native(VkCommandPool pool) {return reinterpret_cast<Pool*>(pool);}
    VkResult Consume(VkResult& result) {return std::exchange(result,VK_SUCCESS);}
    bool Wait(const std::atomic<bool>& signal)
    {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!signal.load(std::memory_order_acquire))
        {
            if(std::chrono::steady_clock::now()>=deadline)return false;
            std::this_thread::yield();
        }
        return true;
    }
}

void* operator new(std::size_t size)
{
    if(pauseAllocation)
    {
        pauseAllocation=false;
        allocationEntered.store(true,std::memory_order_release);
        while(!allocationReleased.load(std::memory_order_acquire))std::this_thread::yield();
    }
    if(std::exchange(failAllocation,false))throw std::bad_alloc();
    if(auto* memory=std::malloc(size?size:1))return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept {std::free(memory);}
void operator delete(void* memory,std::size_t) noexcept {std::free(memory);}

extern "C"
{
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice,const VkCommandPoolCreateInfo*,const VkAllocationCallbacks*,VkCommandPool* pool)
{
    const auto result=Consume(createResult);
    if(result!=VK_SUCCESS)return result;
    *pool=reinterpret_cast<VkCommandPool>(new Pool);
    ++created;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice,VkCommandPool pool,const VkAllocationCallbacks*)
{
    CHECK(!Native(pool)->leased.load(std::memory_order_relaxed));
    delete Native(pool);
    ++destroyed;
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice,const VkCommandBufferAllocateInfo* info,VkCommandBuffer* buffer)
{
    const auto result=Consume(allocateResult);
    if(result==VK_SUCCESS)*buffer=reinterpret_cast<VkCommandBuffer>(info->commandPool);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice,VkCommandPool pool,VkCommandPoolResetFlags)
{
    CHECK(!Native(pool)->leased.load(std::memory_order_relaxed));
    return Consume(resetResult);
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer,const VkCommandBufferBeginInfo*)
{return Consume(beginResult);}
}

namespace
{
    using Cache=dyf::Backends::VulkanCommandPoolCache;
    using Storage=dyf::Backends::VulkanCommandStorage;
    dyf::Backends::VulkanContext Context()
    {
        dyf::Backends::VulkanContext context{};
        context.device=reinterpret_cast<VkDevice>(uintptr_t{1});
        context.queueIndices.graphicsFamily=0;
        return context;
    }
    void Populate(Cache& cache,unsigned count)
    {
        std::vector<std::unique_ptr<Storage>> held;
        for(unsigned i=0;i<count;++i)held.push_back(cache.Acquire());
        for(auto& storage:held)cache.Return(std::move(storage));
    }
    void CheckProgressDuringGrowth()
    {
        Cache cache(Context());
        auto extra=cache.Acquire();
        Populate(cache,64);
        std::thread growing([&] {pauseAllocation=true;cache.Return(std::move(extra));});
        CHECK(Wait(allocationEntered));
        std::atomic<bool> acquired{false};
        std::thread reader([&] {auto storage=cache.Acquire();acquired.store(true,std::memory_order_release);cache.Return(std::move(storage));});
        const bool progressed=Wait(acquired);
        allocationReleased.store(true,std::memory_order_release);
        growing.join();reader.join();
        CHECK(progressed);
    }
    void CheckConcurrentOwnership()
    {
        auto cache=std::make_shared<Cache>(Context());
        Populate(*cache,97);
        const auto before=created.load();
        for(unsigned i=0;i<1000;++i)cache->Return(cache->Acquire());
        CHECK(created.load()==before);
        std::vector<std::thread> workers;
        for(unsigned thread=0;thread<8;++thread)
            workers.emplace_back([cache] {
                for(unsigned i=0;i<10000;++i)
                {
                    auto storage=cache->Acquire();
                    auto* pool=Native(storage->pool);
                    CHECK(!pool->leased.exchange(true,std::memory_order_relaxed));
                    ++pool->visits;
                    pool->leased.store(false,std::memory_order_relaxed);
                    cache->Return(std::move(storage));
                }
            });
        for(auto& worker:workers)worker.join();
        const auto live=created.load()-destroyed.load(), createdBeforeDrain=created.load();
        unsigned visits=0;
        std::vector<std::unique_ptr<Storage>> held;
        for(unsigned i=0;i<live;++i)
        {
            held.push_back(cache->Acquire());
            auto* pool=Native(held.back()->pool);
            CHECK(!pool->leased.exchange(true,std::memory_order_relaxed));
            visits+=pool->visits;
        }
        CHECK(visits==80000 && created.load()==createdBeforeDrain);
        for(auto& storage:held)
        {
            Native(storage->pool)->leased.store(false,std::memory_order_relaxed);
            cache->Return(std::move(storage));
        }
        auto retained=cache;
        cache.reset();
        auto storage=retained->Acquire();
        retained->DisableReuse();
        retained->Return(std::move(storage));
        bool rejected=false;
        try {storage=retained->Acquire();} catch(const std::runtime_error&) {rejected=true;}
        CHECK(rejected);
    }
    void CheckFailures()
    {
        for(const auto failure : {VK_ERROR_DEVICE_LOST,VK_ERROR_OUT_OF_HOST_MEMORY})
        for(unsigned stage=0;stage<4;++stage)
        {
            Cache cache(Context());
            auto* result=stage==0?&createResult:stage==1?&allocateResult:stage==2?&beginResult:&resetResult;
            if(stage==3)Populate(cache,1);
            *result=failure;
            bool rejected=false;
            try {auto storage=cache.Acquire();} catch(const std::runtime_error&) {rejected=true;}
            CHECK(rejected && cache.IsDeviceLost()==(failure==VK_ERROR_DEVICE_LOST));
            if(failure!=VK_ERROR_DEVICE_LOST)cache.Return(cache.Acquire());
        }
        Cache cache(Context());
        auto storage=cache.Acquire();
        const auto before=destroyed.load();
        failAllocation=true;
        cache.Return(std::move(storage));
        CHECK(!failAllocation && destroyed.load()==before+1);
        auto recovered=cache.Acquire();
        cache.Return(std::move(recovered));
    }
    void CheckConcurrentGrowth()
    {
        Cache cache(Context());
        std::atomic<unsigned> ready{0};
        std::vector<std::thread> workers;
        for(unsigned thread=0;thread<8;++thread)
            workers.emplace_back([&] {
                std::vector<std::unique_ptr<Storage>> held;
                for(unsigned i=0;i<40;++i)held.push_back(cache.Acquire());
                ++ready;
                while(ready.load()!=8)std::this_thread::yield();
                for(auto& storage:held)cache.Return(std::move(storage));
            });
        for(auto& worker:workers)worker.join();
        const auto before=created.load();
        Populate(cache,320);
        CHECK(created.load()==before);
    }
}

int main()
{
    CheckProgressDuringGrowth();
    CheckConcurrentOwnership();
    CheckConcurrentGrowth();
    CheckFailures();
    CHECK(created.load()==destroyed.load());
    std::puts("PASS VulkanCommandPoolCache: independent progress, 80000 concurrent leases, growth, loss, allocation failure, drain");
}
