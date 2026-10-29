#pragma once

#include "dyf/Platform/WakeEvent.h"
#include <atomic>
#include <thread>

namespace dyf::Backends
{
// One producer owns the job until Poll consumes the worker's release publication.
template<class Job, class Result>
class VulkanPresentWorker final
{
public:
    using Execute = Result (*)(const Job&) noexcept;
    explicit VulkanPresentWorker(Execute execute) : m_execute(execute), m_thread([this] { Run(); }) {}
    ~VulkanPresentWorker()
    {
        Wait();
        m_stop.store(true, std::memory_order_release);
        m_work.Signal();
        m_thread.join();
    }
    bool TrySubmit(const Job& job)
    {
        if(Pending()) return false;
        m_job = job;
        m_state.store(State::Running, std::memory_order_release);
        m_work.Signal();
        return true;
    }
    bool Pending() const { return m_state.load(std::memory_order_acquire) != State::Idle; }
    bool PeekResult(Result& result) const
    {
        if(m_state.load(std::memory_order_acquire) != State::Ready) return false;
        result = m_result;
        return true;
    }
    bool Poll(Job& job, Result& result)
    {
        if(!PeekResult(result)) return false;
        job = m_job;
        m_done.WaitFor(0);
        m_state.store(State::Idle, std::memory_order_release);
        return true;
    }
    void Wait()
    {
        while(m_state.load(std::memory_order_acquire) == State::Running)
        {
#if defined(_WIN32)
            // WSI can SendMessage to the window owner; do not dispatch queued app input reentrantly.
            MSG message{};
            PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            m_done.WaitFor(1000000);
#else
            m_done.Wait();
#endif
        }
    }
private:
    enum class State { Idle, Running, Ready };
    void Run() noexcept
    {
        for(;;)
        {
            m_work.Wait();
            if(m_stop.load(std::memory_order_acquire)) return;
            m_result = m_execute(m_job);
            m_state.store(State::Ready, std::memory_order_release);
            m_done.Signal();
        }
    }
    Execute m_execute;
    Job m_job{};
    Result m_result{};
    std::atomic<State> m_state{State::Idle};
    std::atomic<bool> m_stop{false};
    Platform::WakeEvent m_work, m_done;
    std::thread m_thread;
};
}
