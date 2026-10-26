#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <cerrno>
#include <ctime>
#include <semaphore.h>
#if defined(__ANDROID__)
#if __ANDROID_API__ >= 30
#define DYF_WAKE_CLOCKWAIT 1
#elif __ANDROID_API__ >= 28
#define DYF_WAKE_MONOTONIC_NP 1
#endif
#elif defined(__GLIBC__) && defined(__USE_GNU)
#if __GLIBC_PREREQ(2,30)
#define DYF_WAKE_CLOCKWAIT 1
#endif
#endif
#endif

namespace dyf::Platform
{
// Counting wake notifications, initially zero. An early signal is retained until
// Wait consumes it. This parks work recipients; it never protects shared state.
class WakeEvent final
{
public:
    WakeEvent()
    {
#if defined(_WIN32)
        m_event = CreateSemaphoreW(nullptr, 0, LONG_MAX, nullptr);
        if(!m_event) throw std::system_error(int(GetLastError()), std::system_category(), "Native wake creation failed");
#elif defined(__APPLE__)
        m_event = dispatch_semaphore_create(0);
        if(!m_event) throw std::bad_alloc();
#else
        if(sem_init(&m_event, 0, 0) != 0)
            throw std::system_error(errno, std::generic_category(), "Native wake creation failed");
#endif
    }
    ~WakeEvent()
    {
#if defined(_WIN32)
        CloseHandle(m_event);
#elif defined(__APPLE__)
#if !defined(__OBJC__) || !__has_feature(objc_arc)
        dispatch_release(m_event);
#endif
#else
        sem_destroy(&m_event);
#endif
    }
    WakeEvent(const WakeEvent&) = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;

    void Signal() noexcept
    {
#if defined(_WIN32)
        if(!ReleaseSemaphore(m_event, 1, nullptr)) std::terminate();
#elif defined(__APPLE__)
        dispatch_semaphore_signal(m_event);
#else
        if(sem_post(&m_event) != 0) std::terminate();
#endif
    }
    void Wait() noexcept
    {
#if defined(_WIN32)
        if(WaitForSingleObject(m_event, INFINITE) != WAIT_OBJECT_0) std::terminate();
#elif defined(__APPLE__)
        if(dispatch_semaphore_wait(m_event, DISPATCH_TIME_FOREVER) != 0) std::terminate();
#else
        while(sem_wait(&m_event) != 0)
            if(errno != EINTR) std::terminate();
#endif
    }
    // Consumes one notification, or returns false when the finite interval
    // expires. Windows rounds up to milliseconds; zero only checks readiness.
    bool WaitFor(std::uint64_t nanoseconds) noexcept
    {
#if defined(_WIN32)
        for(;;)
        {
            const auto milliseconds = nanoseconds / 1000000u + (nanoseconds % 1000000u != 0);
            const auto slice = static_cast<DWORD>((std::min)(milliseconds, std::uint64_t(INFINITE - 1u)));
            const auto result = WaitForSingleObject(m_event, slice);
            if(result == WAIT_OBJECT_0) return true;
            if(result != WAIT_TIMEOUT) std::terminate();
            const auto elapsed = std::uint64_t(slice) * 1000000u;
            if(nanoseconds <= elapsed) return false;
            nanoseconds -= elapsed;
        }
#elif defined(__APPLE__)
        for(;;)
        {
            // Keep finite waits away from dispatch_time's saturating FOREVER
            // sentinel, even when a caller supplies a very large interval.
            const auto slice = (std::min)(nanoseconds, std::uint64_t{86400000000000});
            if(dispatch_semaphore_wait(m_event, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(slice))) == 0)
                return true;
            if(nanoseconds <= slice) return false;
            nanoseconds -= slice;
        }
#else
        if(nanoseconds == 0)
        {
            while(sem_trywait(&m_event) != 0)
            {
                if(errno == EAGAIN) return false;
                if(errno != EINTR) std::terminate();
            }
            return true;
        }
        timespec deadline{};
#if defined(DYF_WAKE_CLOCKWAIT) || defined(DYF_WAKE_MONOTONIC_NP)
        if(clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) std::terminate();
#else
        if(clock_gettime(CLOCK_REALTIME, &deadline) != 0) std::terminate();
#endif
        const auto seconds = nanoseconds / 1000000000u;
        const auto maximum = (std::numeric_limits<decltype(deadline.tv_sec)>::max)();
        if(seconds > static_cast<std::uint64_t>(maximum - deadline.tv_sec))
        {
            deadline.tv_sec = maximum;
            deadline.tv_nsec = 999999999;
        }
        else
        {
            deadline.tv_sec += static_cast<decltype(deadline.tv_sec)>(seconds);
            deadline.tv_nsec += static_cast<long>(nanoseconds % 1000000000u);
            if(deadline.tv_nsec >= 1000000000)
            {
                if(deadline.tv_sec == maximum) deadline.tv_nsec = 999999999;
                else { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
            }
        }
#if defined(DYF_WAKE_CLOCKWAIT)
        while(sem_clockwait(&m_event, CLOCK_MONOTONIC, &deadline) != 0)
#elif defined(DYF_WAKE_MONOTONIC_NP)
        while(sem_timedwait_monotonic_np(&m_event, &deadline) != 0)
#else
        while(sem_timedwait(&m_event, &deadline) != 0)
#endif
        {
            if(errno == ETIMEDOUT) return false;
            if(errno != EINTR) std::terminate();
        }
        return true;
#endif
    }
private:
#if defined(_WIN32)
    HANDLE m_event = nullptr;
#elif defined(__APPLE__)
    dispatch_semaphore_t m_event = nullptr;
#else
    sem_t m_event;
#endif
};
}

#undef DYF_WAKE_CLOCKWAIT
#undef DYF_WAKE_MONOTONIC_NP
