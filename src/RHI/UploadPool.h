#pragma once
#include "../Platform/Profiler/CollectorInternal.h"
#include "UploadMemory.h"
#ifdef DY_LIVE_PROFILING_ENABLED
#include "dyf/Platform/Profiler.h"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace dyf::RHI::Detail
{
// Page supplies capacity, mapped bytes and API-specific destruction. A lease
// belongs to a native command list until its existing retirement/discard path.
template<class Page>
class UploadPagePool final : public std::enable_shared_from_this<UploadPagePool<Page>>
{
    static void Accumulate(uint64_t& value,uint64_t amount) noexcept
    { value=amount>UINT64_MAX-value ? UINT64_MAX : value+amount; }
    static void Accumulate(std::atomic<uint64_t>& value,uint64_t amount) noexcept
    {
        auto current=value.load(std::memory_order_relaxed);
        while(!value.compare_exchange_weak(current,amount>UINT64_MAX-current?UINT64_MAX:current+amount,
            std::memory_order_relaxed)) {}
    }
    static constexpr uint32_t PolicyBits(UploadPolicy policy) noexcept
    { return (policy.reusePages?1u:0u)|(policy.suballocate?2u:0u); }
public:
    using Factory=std::function<std::unique_ptr<Page>(uint64_t)>;
    // A slot has one arena writer at a time. Its counters accumulate across
    // arena lifetimes, so readers never need a pointer into a destroyed arena.
    struct alignas(128) RequestStatistics
    {
        static_assert(std::atomic<uint64_t>::is_always_lock_free,
            "Upload request statistics require lock-free 64-bit atomics.");
        std::atomic<uint64_t> count{0},bytes{0};
        void Record(uint64_t amount) noexcept
        {
            auto requests=count.load(std::memory_order_relaxed);
            auto requestedBytes=bytes.load(std::memory_order_relaxed);
            Accumulate(requests,1);Accumulate(requestedBytes,amount);
            count.store(requests,std::memory_order_relaxed);
            bytes.store(requestedBytes,std::memory_order_relaxed);
        }
    private:
        friend class UploadPagePool;
        std::atomic<bool> claimed{false};
    };
private:
    struct PageSlot { std::atomic<Page*> page{nullptr}; };
    static_assert(std::atomic<Page*>::is_always_lock_free && std::atomic<bool>::is_always_lock_free &&
        std::atomic<uint32_t>::is_always_lock_free,"Upload pools require lock-free ownership atomics.");
public:
    class Lease
    {
        friend class UploadPagePool;
        std::shared_ptr<UploadPagePool> m_owner;
        std::unique_ptr<Page> m_page;
        Lease(std::shared_ptr<UploadPagePool> owner,std::unique_ptr<Page> page)
            :m_owner(std::move(owner)),m_page(std::move(page)) {}
        void Release() noexcept
        { if(m_page)m_owner->Return(std::move(m_page));m_owner.reset(); }
    public:
        Lease()=default;
        Lease(const Lease&)=delete;
        Lease& operator=(const Lease&)=delete;
        Lease(Lease&&)=default;
        Lease& operator=(Lease&& other) noexcept
        {
            if(this!=&other) { Release();m_owner=std::move(other.m_owner);m_page=std::move(other.m_page); }
            return *this;
        }
        ~Lease() { Release(); }
        [[nodiscard]] Page* Get() const noexcept { return m_page.get(); }
        explicit operator bool() const noexcept { return m_page!=nullptr; }
    };
    explicit UploadPagePool(Factory factory,uint64_t pageSize=256*1024,uint64_t cacheBudget=8*1024*1024)
        :m_factory(std::move(factory)),m_pageSize(std::max(uint64_t{1},pageSize)),m_cacheBudget(cacheBudget) {}
    ~UploadPagePool()
    {
        // Arenas and page leases retain the pool. Only its final owner frees
        // these stable slots, after all concurrent readers/writers have left.
        for(auto& slot:m_free)delete slot.page.load(std::memory_order_relaxed);
    }

    void SetPolicy(UploadPolicy policy)
    { m_policy.store(PolicyBits(policy),std::memory_order_release); }
    [[nodiscard]] UploadPolicy GetPolicy() const
    {
        const auto policy=m_policy.load(std::memory_order_acquire);
        return {(policy&1u)!=0,(policy&2u)!=0};
    }
    [[nodiscard]] Lease Acquire(uint64_t minimumCapacity)
    { return Acquire(minimumCapacity,GetPolicy().reusePages); }
    [[nodiscard]] Lease Acquire(uint64_t minimumCapacity,bool reusePages)
    {
        if(!minimumCapacity || minimumCapacity>std::numeric_limits<size_t>::max() || !m_factory)return {};
        auto owner=this->shared_from_this();
        std::unique_ptr<Page> page=reusePages?TakeCached(minimumCapacity):nullptr;
        [[maybe_unused]] const bool reused=page!=nullptr;
        if(!page)
        {
            const auto capacity=std::max(minimumCapacity,m_pageSize);
            if(capacity>std::numeric_limits<size_t>::max())return {};
            page=m_factory(capacity);
            if(!page || !page->mapped || page->capacity<capacity || page->capacity>std::numeric_limits<size_t>::max())return {};
            if(!ReserveInUse(page->capacity))return {};
            Accumulate(m_allocationCount,1);
            Accumulate(m_allocationBytes,page->capacity);
        }
        else
        {
            if(!ReserveInUse(page->capacity))return {};
            Accumulate(m_reuseCount,1);
        }
#ifdef DY_LIVE_PROFILING_ENABLED
        const auto context=Platform::GetLiveContext();
        if(context.frame.frame&&Platform::IsLiveCollectorEnabled())
        {
            Platform::LiveEvent event;event.id=Platform::AllocateLiveId();event.context=context;
            event.kind=Platform::LiveEventKind::UploadMemory;event.stage=Platform::LiveStage::NativeTranslate;
            event.arguments[0]=reused?1:0;event.bytes=page->capacity;event.operations=1;
            Platform::RecordLiveEvent(event);
        }
#endif
        return Lease(std::move(owner),std::move(page));
    }
    [[nodiscard]] RequestStatistics* RegisterRequests() noexcept
    {
        for(auto& slot:m_requests)
        {
            bool available=false;
            if(slot.claimed.compare_exchange_strong(available,true,std::memory_order_acquire,
                std::memory_order_relaxed))return &slot;
        }
        return nullptr;
    }
    void RetireRequests(RequestStatistics& requests) noexcept
    { requests.claimed.store(false,std::memory_order_release); }
    void RecordRequest(uint64_t bytes) noexcept
    {
        // A bounded slot scan may lose a race or exceed 128 active arenas.
        // Keep those requests exact without allocating permanent registry nodes.
        Accumulate(m_overflowRequestCount,1);Accumulate(m_overflowRequestedBytes,bytes);
    }
    [[nodiscard]] UploadMemoryStatistics GetStatistics() const
    {
        // Concurrent values are sampled independently, as arena request counts
        // already were. Slot addresses and completed counts stay valid forever
        // within this pool; retiring/reusing an arena cannot double-count them.
        UploadMemoryStatistics statistics{};
        statistics.allocationCount=m_allocationCount.load(std::memory_order_relaxed);
        statistics.allocationBytes=m_allocationBytes.load(std::memory_order_relaxed);
        statistics.reuseCount=m_reuseCount.load(std::memory_order_relaxed);
        statistics.inUseBytes=m_inUseBytes.load(std::memory_order_relaxed);
        statistics.peakInUseBytes=std::max(statistics.inUseBytes,m_peakInUseBytes.load(std::memory_order_relaxed));
        statistics.cachedBytes=m_cachedBytes.load(std::memory_order_relaxed);
        statistics.requestCount=m_overflowRequestCount.load(std::memory_order_relaxed);
        statistics.requestedBytes=m_overflowRequestedBytes.load(std::memory_order_relaxed);
        for(const auto& slot:m_requests)
        {
            Accumulate(statistics.requestCount,slot.count.load(std::memory_order_relaxed));
            Accumulate(statistics.requestedBytes,slot.bytes.load(std::memory_order_relaxed));
        }
        return statistics;
    }
private:
    bool ReserveInUse(uint64_t bytes) noexcept
    {
        auto current=m_inUseBytes.load(std::memory_order_relaxed);
        while(bytes<=UINT64_MAX-current)
            if(m_inUseBytes.compare_exchange_weak(current,current+bytes,std::memory_order_relaxed))
            {
                const auto used=current+bytes;
                auto peak=m_peakInUseBytes.load(std::memory_order_relaxed);
                while(peak<used && !m_peakInUseBytes.compare_exchange_weak(peak,used,std::memory_order_relaxed)) {}
                return true;
            }
        return false;
    }
    std::unique_ptr<Page> TakeCached(uint64_t minimumCapacity) noexcept
    {
        std::unique_ptr<Page> best;
        const auto idealCapacity=std::max(minimumCapacity,m_pageSize);
        for(auto& slot:m_free)
        {
            if(!slot.page.load(std::memory_order_relaxed))continue;
            // Claim ownership before reading capacity. No reader dereferences a
            // published pointer; exchange-to-null cannot suffer stack-node ABA.
            std::unique_ptr<Page> page(slot.page.exchange(nullptr,std::memory_order_acquire));
            if(!page)continue;
            m_cachedBytes.fetch_sub(page->capacity,std::memory_order_relaxed);
            if(page->capacity>=minimumCapacity && (!best || page->capacity<best->capacity))
                page.swap(best);
            if(page)Cache(std::move(page));
            // Keep best-fit reuse for mixed page sizes without dereferencing
            // any unclaimed pointer. The common exact-size hit ends the scan.
            if(best && best->capacity==idealCapacity)return best;
        }
        return best;
    }
    void Cache(std::unique_ptr<Page> page) noexcept
    {
        if(!GetPolicy().reusePages)return;
        const auto bytes=page->capacity;
        auto cached=m_cachedBytes.load(std::memory_order_relaxed);
        while(cached<=m_cacheBudget && bytes<=m_cacheBudget-cached)
        {
            if(!m_cachedBytes.compare_exchange_weak(cached,cached+bytes,std::memory_order_relaxed))continue;
            auto* owned=page.release();
            for(auto& slot:m_free)
            {
                Page* empty=nullptr;
                if(slot.page.compare_exchange_strong(empty,owned,std::memory_order_release,
                    std::memory_order_relaxed))return;
            }
            m_cachedBytes.fetch_sub(bytes,std::memory_order_relaxed);
            delete owned;
            return;
        }
    }
    void Return(std::unique_ptr<Page> page) noexcept
    {
        m_inUseBytes.fetch_sub(page->capacity,std::memory_order_relaxed);
        Cache(std::move(page));
    }
    Factory m_factory;
    uint64_t m_pageSize,m_cacheBudget;
    // The byte budget remains authoritative; 64 slots additionally bound cache
    // metadata and make return allocation-free. Excess pages are just released.
    std::array<PageSlot,64> m_free;
    std::array<RequestStatistics,128> m_requests;
    std::atomic<uint64_t> m_overflowRequestCount{0},m_overflowRequestedBytes{0};
    std::atomic<uint64_t> m_allocationCount{0},m_allocationBytes{0},m_reuseCount{0};
    std::atomic<uint64_t> m_inUseBytes{0},m_peakInUseBytes{0},m_cachedBytes{0};
    std::atomic<uint32_t> m_policy{PolicyBits(UploadPolicy{})};
};

template<class Page>
class UploadArena final
{
public:
    struct Allocation
    {
        Page* page=nullptr;
        uint64_t offset=0;
        uint8_t* data=nullptr;
        explicit operator bool() const noexcept { return page!=nullptr; }
    };
    explicit UploadArena(std::shared_ptr<UploadPagePool<Page>> pool)
        :m_pool(std::move(pool)),m_policy(m_pool?m_pool->GetPolicy():UploadPolicy{})
    { if(m_pool)m_requests=m_pool->RegisterRequests(); }
    UploadArena(std::shared_ptr<UploadPagePool<Page>> pool,UploadPolicy policy)
        :m_pool(std::move(pool)),m_policy(policy)
    { if(m_pool)m_requests=m_pool->RegisterRequests(); }
    UploadArena(const UploadArena&)=delete;
    UploadArena& operator=(const UploadArena&)=delete;
    ~UploadArena() { if(m_requests)m_pool->RetireRequests(*m_requests); }
    [[nodiscard]] Allocation Allocate(uint64_t bytes,uint64_t alignment=16)
    {
        if(!m_pool || !bytes || bytes>std::numeric_limits<size_t>::max() || !alignment || (alignment&(alignment-1)))return {};
        uint64_t offset=0;
        Page* page=!m_policy.suballocate||m_pages.empty()?nullptr:m_pages.back().Get();
        if(page && m_offset<=UINT64_MAX-(alignment-1))
        {
            offset=(m_offset+alignment-1)&~(alignment-1);
            if(offset>page->capacity || bytes>page->capacity-offset)page=nullptr;
        }
        else page=nullptr;
        if(!page)
        {
            auto lease=m_pool->Acquire(bytes,m_policy.reusePages);
            if(!lease)return {};
            m_pages.push_back(std::move(lease));
            page=m_pages.back().Get();offset=0;
        }
        if(m_requests)m_requests->Record(bytes);else m_pool->RecordRequest(bytes);
        m_offset=offset+bytes;
        return {page,offset,page->mapped+static_cast<size_t>(offset)};
    }
private:
    std::shared_ptr<UploadPagePool<Page>> m_pool;
    // Freeze recording policy; returning a retired page uses the pool's current cache policy.
    const UploadPolicy m_policy;
    std::vector<typename UploadPagePool<Page>::Lease> m_pages;
    typename UploadPagePool<Page>::RequestStatistics* m_requests=nullptr;
    uint64_t m_offset=0;
};
}
