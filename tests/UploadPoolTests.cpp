#include "Backends/D3D12/D3D12UploadPool.h"
#include "Backends/Vulkan/VulkanUploadPool.h"
#include "Backends/Metal/MetalUploadPool.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
std::atomic<int> checks{0}, failures{0};
#define CHECK(condition) do { ++checks; if(!(condition)) { ++failures; std::cerr << "Failed: " #condition << " line " << __LINE__ << '\n'; } } while(false)
struct Page {
    uint64_t capacity;
    std::vector<uint8_t> bytes;
    uint8_t* mapped;
    explicit Page(uint64_t size) : capacity(size), bytes(static_cast<size_t>(size)), mapped(bytes.data()) {}
};
template<template<class> class PagePool,template<class> class PageArena>
void CheckPool() {
    using Pool=PagePool<Page>;
    using Arena=PageArena<Page>;
    const auto MakePool=[](uint64_t pageSize=128,uint64_t cacheBudget=512) {
        return std::make_shared<Pool>([](uint64_t size) {return std::make_unique<Page>(size);},pageSize,cacheBudget);
    };
    {
        auto pool=MakePool();
        auto first=std::make_unique<Arena>(pool);
        auto a=first->Allocate(17,16), b=first->Allocate(8,64);
        CHECK(a && b && a.page==b.page && a.offset==0 && b.offset==64);
        std::memset(a.data,0x25,17); std::memset(b.data,0x71,8);
        auto second=std::make_unique<Arena>(pool);
        auto c=second->Allocate(17,16);
        CHECK(c && c.page!=a.page);
        std::memset(c.data,0x4a,17);
        CHECK(a.data[16]==0x25 && b.data[7]==0x71);
        CHECK(pool->GetStatistics().inUseBytes==256 && pool->GetStatistics().cachedBytes==0);
        Page* released=a.page;
        first.reset();
        Arena third(pool); auto d=third.Allocate(17);
        CHECK(d && d.page==released && d.page!=c.page);
        CHECK(c.data[16]==0x4a);
        const auto stats=pool->GetStatistics();
        CHECK(stats.allocationCount==2 && stats.reuseCount==1 && stats.requestCount==4);
        CHECK(stats.requestedBytes==59 && stats.peakInUseBytes==256);
    }
    {
        auto pool=MakePool();
        for(int frame=0;frame<64;++frame) { Arena commands(pool); CHECK(commands.Allocate(64)); }
        const auto stats=pool->GetStatistics();
        CHECK(stats.allocationCount==1 && stats.reuseCount==63);
        CHECK(stats.inUseBytes==0 && stats.cachedBytes==128 && stats.allocationBytes==128);
        CHECK(stats.requestCount==64 && stats.requestedBytes==4096);
    }
    {
        auto pool=MakePool(64,128);
        { Arena commands(pool); CHECK(commands.Allocate(1024)); }
        CHECK(pool->GetStatistics().cachedBytes==0 && pool->GetStatistics().inUseBytes==0);
        { Arena a(pool),b(pool),c(pool); CHECK(a.Allocate(64)); CHECK(b.Allocate(64)); CHECK(c.Allocate(64)); }
        CHECK(pool->GetStatistics().cachedBytes==128 && pool->GetStatistics().inUseBytes==0);
    }
    {
        bool fail=true, throwing=false;
        auto pool=std::make_shared<Pool>([&](uint64_t size)->std::unique_ptr<Page> {
            if(throwing) throw std::bad_alloc();
            return fail ? nullptr : std::make_unique<Page>(size);
        },128,128);
        Arena arena(pool);
        CHECK(!arena.Allocate(64));
        CHECK(pool->GetStatistics().allocationCount==0 && pool->GetStatistics().requestCount==0);
        throwing=true;
        try { (void)arena.Allocate(64); CHECK(false); } catch(const std::bad_alloc&) { CHECK(true); }
        throwing=false; fail=false;
        CHECK(arena.Allocate(64));
        CHECK(pool->GetStatistics().allocationCount==1);
        CHECK(!arena.Allocate(0) && !arena.Allocate(1,0) && !arena.Allocate(1,3));
    }
    {
        auto pool=MakePool(128,512);
        std::vector<std::thread> threads;
        for(int worker=0;worker<4;++worker) threads.emplace_back([pool,worker] {
            for(int n=0;n<100;++n) {
                Arena arena(pool); auto a=arena.Allocate(31,16),b=arena.Allocate(17,32);
                CHECK(a && b && b.offset==32 && a.page==b.page);
                std::memset(a.data,worker+1,31); std::memset(b.data,worker+9,17);
                std::this_thread::yield();
                CHECK(a.data[30]==worker+1 && b.data[16]==worker+9);
            }
        });
        for(auto& thread:threads)thread.join();
        const auto stats=pool->GetStatistics();
        CHECK(stats.requestCount==800 && stats.requestedBytes==19200);
        CHECK(stats.inUseBytes==0 && stats.cachedBytes<=512);
    }
}
template<template<class> class PagePool,template<class> class PageArena>
void CheckPolicies() {
    using Pool=PagePool<Page>;
    using Arena=PageArena<Page>;
    for(const bool reuse:{false,true})for(const bool suballocate:{false,true}) {
        auto pool=std::make_shared<Pool>([](uint64_t size){return std::make_unique<Page>(size);},128,1024);
        pool->SetPolicy({reuse,suballocate});
        for(unsigned frame=0;frame<3;++frame) {
            Arena arena(pool);
            const auto a=arena.Allocate(17,16),b=arena.Allocate(8,64),c=arena.Allocate(13,16);
            CHECK(a && b && c);
            CHECK(a.offset==0 && b.offset==(suballocate?64:0) && c.offset==(suballocate?80:0));
            CHECK((a.page==b.page)==suballocate && (b.page==c.page)==suballocate);
            std::memset(a.data,frame+1,17);std::memset(b.data,frame+9,8);std::memset(c.data,frame+17,13);
            CHECK(a.data[16]==frame+1 && b.data[7]==frame+9 && c.data[12]==frame+17);
            CHECK(pool->GetStatistics().inUseBytes==(suballocate?128:384));
        }
        const uint64_t pages=suballocate?1:3;
        const auto stats=pool->GetStatistics();
        CHECK(stats.allocationCount==pages*(reuse?1:3));
        CHECK(stats.reuseCount==pages*(reuse?2:0));
        CHECK(stats.allocationBytes==stats.allocationCount*128 && stats.requestedBytes==114 && stats.requestCount==9);
        CHECK(stats.inUseBytes==0 && stats.cachedBytes==(reuse?pages*128:0));
    }
    {
        auto pool=std::make_shared<Pool>([](uint64_t size){return std::make_unique<Page>(size);},128,1024);
        auto old=std::make_unique<Arena>(pool);
        const auto a=old->Allocate(17);std::memset(a.data,0x2a,17);
        pool->SetPolicy({false,false});
        CHECK(pool->GetStatistics().inUseBytes==128);
        const auto b=old->Allocate(8,32);
        CHECK(b && b.page==a.page && b.offset==32 && a.data[16]==0x2a);
        {
            Arena fresh(pool);
            const auto c=fresh.Allocate(17),d=fresh.Allocate(8);
            CHECK(c && d && c.page!=d.page && c.page!=a.page && d.page!=a.page);
            CHECK(c.offset==0 && d.offset==0);
            old.reset();
            CHECK(pool->GetStatistics().inUseBytes==256 && pool->GetStatistics().cachedBytes==0);
        }
        CHECK(pool->GetStatistics().inUseBytes==0 && pool->GetStatistics().cachedBytes==0);
    }
    {
        auto pool=std::make_shared<Pool>([](uint64_t size){return std::make_unique<Page>(size);},128,1024);
        {Arena warm(pool);CHECK(warm.Allocate(17));}
        pool->SetPolicy({false,true});
        CHECK(pool->GetStatistics().cachedBytes==128);
        {Arena uncached(pool);CHECK(uncached.Allocate(17));}
        CHECK(pool->GetStatistics().allocationCount==2 && pool->GetStatistics().reuseCount==0);
        CHECK(pool->GetStatistics().cachedBytes==128);
        pool->SetPolicy({true,true});
        {Arena reused(pool);CHECK(reused.Allocate(17));}
        CHECK(pool->GetStatistics().allocationCount==2 && pool->GetStatistics().reuseCount==1);
    }
    for(const bool reuse:{false,true})for(const bool suballocate:{false,true}) {
        bool fail=false,throwing=false;
        auto pool=std::make_shared<Pool>([&](uint64_t size)->std::unique_ptr<Page>{
            if(throwing)throw std::bad_alloc();
            return fail?nullptr:std::make_unique<Page>(size);
        },128,1024);
        pool->SetPolicy({reuse,suballocate});
        Arena arena(pool);
        const auto a=arena.Allocate(17);CHECK(a);std::memset(a.data,0x39,17);
        fail=true;CHECK(!arena.Allocate(256));
        throwing=true;
        try{(void)arena.Allocate(256);CHECK(false);}catch(const std::bad_alloc&){CHECK(true);}
        CHECK(a.data[16]==0x39 && pool->GetStatistics().inUseBytes==128);
        CHECK(pool->GetStatistics().allocationCount==1 && pool->GetStatistics().requestCount==1);
        CHECK(!arena.Allocate(0) && !arena.Allocate(1,0) && !arena.Allocate(1,3));
        fail=false;throwing=false;CHECK(arena.Allocate(256));
        CHECK(pool->GetStatistics().requestCount==2 && pool->GetStatistics().requestedBytes==273);
    }
}
}
int main() {
    CheckPool<dyf::Backends::D3D12Detail::UploadPagePool,dyf::Backends::D3D12Detail::UploadArena>();
    CheckPolicies<dyf::Backends::D3D12Detail::UploadPagePool,dyf::Backends::D3D12Detail::UploadArena>();
    CheckPool<dyf::Backends::VulkanDetail::UploadPagePool,dyf::Backends::VulkanDetail::UploadArena>();
    CheckPolicies<dyf::Backends::VulkanDetail::UploadPagePool,dyf::Backends::VulkanDetail::UploadArena>();
    CheckPool<dyf::Backends::MetalDetail::UploadPagePool,dyf::Backends::MetalDetail::UploadArena>();
    CheckPolicies<dyf::Backends::MetalDetail::UploadPagePool,dyf::Backends::MetalDetail::UploadArena>();
    std::cout << checks << " checks of the backend upload pools, " << failures << " failures\n";
    return failures ? 1 : 0;
}
