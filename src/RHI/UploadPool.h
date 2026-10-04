#pragma once
#include "dyf/RHI/UploadMemory.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
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
public:
    using Factory=std::function<std::unique_ptr<Page>(uint64_t)>;
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

    [[nodiscard]] Lease Acquire(uint64_t minimumCapacity)
    {
        if(!minimumCapacity || minimumCapacity>std::numeric_limits<size_t>::max() || !m_factory)return {};
        auto owner=this->shared_from_this();
        std::unique_ptr<Page> page;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto best=m_free.end();
            for(auto it=m_free.begin();it!=m_free.end();++it)
                if((*it)->capacity>=minimumCapacity && (best==m_free.end() || (*it)->capacity<(*best)->capacity))best=it;
            if(best!=m_free.end())
            {
                page=std::move(*best);m_free.erase(best);
                m_statistics.cachedBytes-=page->capacity;
                Accumulate(m_statistics.reuseCount,1);
                m_statistics.inUseBytes+=page->capacity;
                m_statistics.peakInUseBytes=std::max(m_statistics.peakInUseBytes,m_statistics.inUseBytes);
            }
        }
        if(!page)
        {
            const auto capacity=std::max(minimumCapacity,m_pageSize);
            if(capacity>std::numeric_limits<size_t>::max())return {};
            page=m_factory(capacity);
            if(!page || !page->mapped || page->capacity<capacity || page->capacity>std::numeric_limits<size_t>::max())return {};
            std::lock_guard<std::mutex> lock(m_mutex);
            if(page->capacity>UINT64_MAX-m_statistics.inUseBytes)return {};
            Accumulate(m_statistics.allocationCount,1);
            Accumulate(m_statistics.allocationBytes,page->capacity);
            m_statistics.inUseBytes+=page->capacity;
            m_statistics.peakInUseBytes=std::max(m_statistics.peakInUseBytes,m_statistics.inUseBytes);
        }
        return Lease(std::move(owner),std::move(page));
    }
    void RecordRequest(uint64_t bytes)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        Accumulate(m_statistics.requestCount,1);Accumulate(m_statistics.requestedBytes,bytes);
    }
    [[nodiscard]] UploadMemoryStatistics GetStatistics() const
    { std::lock_guard<std::mutex> lock(m_mutex);return m_statistics; }
private:
    void Return(std::unique_ptr<Page> page) noexcept
    {
        try
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_statistics.inUseBytes-=page->capacity;
            if(page->capacity<=m_cacheBudget-m_statistics.cachedBytes)
            {
                const auto capacity=page->capacity;
                m_free.push_back(std::move(page));
                m_statistics.cachedBytes+=capacity;
            }
        }
        catch(...) {} // Cache growth is optional; release the page on host OOM.
        // Native destruction occurs outside the pool lock.
    }
    Factory m_factory;
    uint64_t m_pageSize,m_cacheBudget;
    mutable std::mutex m_mutex;
    std::vector<std::unique_ptr<Page>> m_free;
    UploadMemoryStatistics m_statistics;
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
    explicit UploadArena(std::shared_ptr<UploadPagePool<Page>> pool):m_pool(std::move(pool)) {}
    UploadArena(const UploadArena&)=delete;
    UploadArena& operator=(const UploadArena&)=delete;
    [[nodiscard]] Allocation Allocate(uint64_t bytes,uint64_t alignment=16)
    {
        if(!m_pool || !bytes || bytes>std::numeric_limits<size_t>::max() || !alignment || (alignment&(alignment-1)))return {};
        uint64_t offset=0;
        Page* page=m_pages.empty()?nullptr:m_pages.back().Get();
        if(page && m_offset<=UINT64_MAX-(alignment-1))
        {
            offset=(m_offset+alignment-1)&~(alignment-1);
            if(offset>page->capacity || bytes>page->capacity-offset)page=nullptr;
        }
        else page=nullptr;
        if(!page)
        {
            auto lease=m_pool->Acquire(bytes);
            if(!lease)return {};
            m_pages.push_back(std::move(lease));
            page=m_pages.back().Get();offset=0;
        }
        m_pool->RecordRequest(bytes);
        m_offset=offset+bytes;
        return {page,offset,page->mapped+static_cast<size_t>(offset)};
    }
private:
    std::shared_ptr<UploadPagePool<Page>> m_pool;
    std::vector<typename UploadPagePool<Page>::Lease> m_pages;
    uint64_t m_offset=0;
};
}
