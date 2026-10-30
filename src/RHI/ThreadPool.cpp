#include "ThreadPool.h"
#include "dyf/Platform/WakeEvent.h"
#include "dyf/Platform/Profiler.h"
#include "../Platform/Profiler/CollectorInternal.h"
#include <algorithm>
#include <atomic>
#include <exception>
#include <stdexcept>

namespace dyf::RHI
{
namespace
{
thread_local const ThreadPool* currentPool = nullptr;

static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
    "Command recording requires lock-free counters and stop flags.");

// Reused by one external calling thread, including calls into different pools.
// The queued request retains one reference until Signal has returned. A caller
// may therefore return or exit its thread without destroying an in-flight wake.
struct CallerCompletion final
{
    Platform::WakeEvent ready;
    std::atomic<uint32_t> references{1};
    void Retain() noexcept { references.fetch_add(1, std::memory_order_relaxed); }
    void Release() noexcept
    {
        if(references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
};
struct ThreadCompletion final
{
    CallerCompletion* value = new CallerCompletion;
    ~ThreadCompletion() { value->Release(); }
};
}

struct ThreadPool::Batch
{
    Batch* nextRequest = nullptr;
    const void* context;
    const BatchTask task;
    const uint32_t count, participants;
    std::atomic<uint32_t> next{0}, remaining;
    std::atomic<bool> completed{false};
    CallerCompletion* completion;
    std::exception_ptr failure;
#ifdef DY_LIVE_PROFILING_ENABLED
    const Platform::LiveContext liveContext = Platform::GetLiveContext();
#endif
    Batch(const void* state, BatchTask callback, uint32_t size, uint32_t workers, CallerCompletion* wake)
        : context(state), task(callback), count(size), participants(workers), remaining(workers), completion(wake) {}
};

struct ThreadPool::Storage
{
    static_assert(std::atomic<Batch*>::is_always_lock_free,
        "Command recording requires lock-free request and mailbox pointers.");
    struct alignas(128) WorkerJob
    {
        std::atomic<Batch*> batch{nullptr};
        Platform::WakeEvent ready;
        std::exception_ptr failure;
    };
    explicit Storage(uint32_t workers) : jobs(std::make_unique<WorkerJob[]>(workers)) {}
    std::unique_ptr<WorkerJob[]> jobs;
    // Producers only push; worker zero alone detaches/reverses and dispatches.
    std::atomic<Batch*> requests{nullptr};
    Platform::WakeEvent requestReady, batchFinished;
    std::atomic<bool> stopping{false}, workersStopping{false};
};

ThreadPool::ThreadPool(uint32_t threadCount)
{
    if(!threadCount) threadCount = (std::max)(1u, std::thread::hardware_concurrency());
    m_storage = std::make_unique<Storage>(threadCount);
    m_workers.reserve(threadCount);
    try
    {
        for(uint32_t i = 0; i < threadCount; ++i)
            m_workers.emplace_back([this, i] { WorkerLoop(i); });
    }
    catch(...)
    {
        StopWorkers();
        throw;
    }
}

ThreadPool::~ThreadPool() { StopWorkers(); }

void ThreadPool::StopWorkers()
{
    if(m_workers.empty()) return;
    m_storage->stopping.store(true, std::memory_order_release);
    m_storage->requestReady.Signal();
    // The dispatcher drains accepted requests while all recording workers still
    // exist. Only then may idle workers leave and their wake objects be released.
    m_workers.front().join();
    m_storage->workersStopping.store(true, std::memory_order_release);
    for(size_t i = 1; i < m_workers.size(); ++i) m_storage->jobs[i].ready.Signal();
    for(size_t i = 1; i < m_workers.size(); ++i) m_workers[i].join();
}

bool ThreadPool::IsWorkerThread() const { return currentPool == this; }

void ThreadPool::RunBatch(uint32_t count, const void* context, BatchTask task)
{
    if(!count) return;
    // A worker never waits for another pool: this also prevents nested pool cycles.
    if(currentPool || count == 1 || m_workers.size() <= 1)
    {
        for(uint32_t i = 0; i < count; ++i) task(context, i);
        return;
    }
    if(m_storage->stopping.load(std::memory_order_acquire))
        throw std::runtime_error("ThreadPool is stopping.");
    thread_local ThreadCompletion caller;
    auto* completion = caller.value;
    Batch batch(context, task, count, (std::min)(count, GetThreadCount()), completion);
    completion->Retain();
    auto* head = m_storage->requests.load(std::memory_order_relaxed);
    do { batch.nextRequest = head; }
    while(!m_storage->requests.compare_exchange_weak(head, &batch,
        std::memory_order_release, std::memory_order_relaxed));
    // Notify only the empty-to-nonempty transition. Reversing each detached list
    // preserves the requests' CAS publication order without a serial-owner lock.
    if(!head) m_storage->requestReady.Signal();
    do { completion->ready.Wait(); }
    while(!batch.completed.load(std::memory_order_acquire));
    if(batch.failure) std::rethrow_exception(batch.failure);
}

void ThreadPool::ExecuteBatch(Batch& batch, uint32_t worker)
{
    {
#ifdef DY_LIVE_PROFILING_ENABLED
        Platform::LiveContextScope restored(batch.liveContext);
#endif
#if defined(DY_LIVE_PROFILING_ENABLED) || defined(DY_PROFILER_TRACING_ENABLED)
        Platform::CpuScope timing(Platform::LiveStage::CpuPrepare, "ThreadPool tasks");
#endif
        for(;;)
        {
            auto item = batch.next.load(std::memory_order_relaxed);
            // Bounded claims never wrap, even with UINT32_MAX items.
            while(item < batch.count && !batch.next.compare_exchange_weak(
                item, item + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {}
            if(item >= batch.count) break;
            try
            {
                batch.task(batch.context, item);
            }
            catch(...)
            {
                auto& failure = m_storage->jobs[worker].failure;
                if(!failure) failure = std::current_exception();
            }
        }
    }
    // End live scopes before publishing completion. No worker touches the batch
    // after this decrement, including while the last native Signal returns.
    if(batch.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
        m_storage->batchFinished.Signal();
}

void ThreadPool::WorkerLoop(uint32_t index)
{
    currentPool = this;
    if(index != 0)
    {
        auto& job = m_storage->jobs[index];
        for(;;)
        {
            job.ready.Wait();
            if(m_storage->workersStopping.load(std::memory_order_acquire)) break;
            if(auto* batch = job.batch.exchange(nullptr, std::memory_order_acquire)) ExecuteBatch(*batch, index);
        }
    }
    else
    {
        for(;;)
        {
            m_storage->requestReady.Wait();
            auto* requests = m_storage->requests.exchange(nullptr, std::memory_order_acquire);
            Batch* first = nullptr;
            while(requests)
            {
                auto* next = requests->nextRequest;
                requests->nextRequest = first;
                first = requests;
                requests = next;
            }
            while(first)
            {
                auto* batch = first;
                first = batch->nextRequest;
                const auto participants = batch->participants;
                for(uint32_t i = 0; i < participants; ++i) m_storage->jobs[i].failure = nullptr;
                for(uint32_t i = 1; i < participants; ++i)
                {
                    m_storage->jobs[i].batch.store(batch, std::memory_order_release);
                    m_storage->jobs[i].ready.Signal();
                }
                ExecuteBatch(*batch, 0);
                m_storage->batchFinished.Wait();
                // Acquire the workers' completion chain before consuming slots.
                (void)batch->remaining.load(std::memory_order_acquire);
                for(uint32_t i = 0; i < participants; ++i)
                {
                    if(!batch->failure) batch->failure = m_storage->jobs[i].failure;
                    m_storage->jobs[i].failure = nullptr;
                }
                auto* completion = batch->completion;
                batch->completed.store(true, std::memory_order_release);
                // The caller may release the stack descriptor as soon as signaled.
                // Only the independently retained completion remains in use below.
                completion->ready.Signal();
                completion->Release();
            }
            if(m_storage->stopping.load(std::memory_order_acquire) &&
                !m_storage->requests.load(std::memory_order_acquire)) break;
        }
    }
    currentPool = nullptr;
}
}
