#include "../src/Backends/Metal/MetalCommandScratch.h"
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(condition) do { if(!(condition)) { \
    std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition);std::abort(); \
} } while(false)

int main()
{
    using namespace dyf::Backends;
    MetalCommandScratchPool pool;
    auto scratch=pool.Acquire();
    scratch->operations.resize(64);
    scratch->inlineConstants.resize(256, 7);
    scratch->vertexBindings.emplace(3, MetalVertexBinding{});
    scratch->colorFormats.push_back(dyf::RHI::Format::R8G8B8A8_UNORM);
    const auto* operations=scratch->operations.data();
    const auto* constants=scratch->inlineConstants.data();
    const auto buckets=scratch->vertexBindings.bucket_count();
    pool.Return(std::move(scratch));
    auto reused=pool.Acquire();
    CHECK(reused->operations.empty() && reused->operations.data()==operations);
    CHECK(reused->inlineConstants.empty() && reused->inlineConstants.data()==constants);
    CHECK(reused->vertexBindings.empty() && reused->vertexBindings.bucket_count()==buckets);
    CHECK(reused->colorFormats.empty());
    auto simultaneous=pool.Acquire();
    CHECK(simultaneous->operations.data()!=operations);
    pool.Return(std::move(simultaneous));
    pool.Return(std::move(reused));

    std::vector<std::thread> workers;
    for(uint32_t worker=1;worker<=8;++worker)
        workers.emplace_back([&,worker] {
            for(unsigned iteration=0;iteration<1000;++iteration)
            {
                auto owned=pool.Acquire();
                CHECK(owned->operations.empty() && owned->inlineConstants.empty() && owned->vertexBindings.empty());
                owned->inlineConstants.assign(256, static_cast<uint8_t>(worker));
                std::this_thread::yield();
                for(auto value:owned->inlineConstants)CHECK(value==worker);
                pool.Return(std::move(owned));
            }
        });
    for(auto& worker:workers)worker.join();

    // More live recordings than cache slots retain independent ownership.
    std::vector<std::unique_ptr<MetalCommandScratch>> held;
    for(unsigned index=0;index<80;++index)held.push_back(pool.Acquire());
    for(auto& owned:held)pool.Return(std::move(owned));
    std::puts("PASS Metal CPU scratch: cleared capacity, concurrent ownership, bounded overflow");
}
