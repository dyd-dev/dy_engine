#pragma once
#include <cstdlib>
#include <new>
#include <vector>

// Bounded, thread-local failure injection for host allocations. No production
// allocator or backend API is modified, and no GPU work is submitted.
namespace P2Allocation
{
    inline thread_local int failAfter = -1;
    inline thread_local bool track = false;
    inline thread_local void* live[256]{};
    inline bool CanInjectFailure(std::size_t size) noexcept
    {
#if defined(P2_SKIP_MSVC_DEBUG_PROXY_FAILURES) && defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
        // MSVC Debug iterator proxies allocate in noexcept STL constructors.
        // Exempt proxy-sized allocations from injection; Release exercises all
        // sizes. Both configurations still track every allocation for leaks.
        return size != sizeof(std::_Container_proxy);
#else
        (void)size;
        return true;
#endif
    }
    inline void Begin(int successfulAllocations)
    {
        for (auto& pointer : live) pointer = nullptr;
        track = true;
        failAfter = successfulAllocations;
    }
    inline unsigned End()
    {
        track = false;
        failAfter = -1;
        unsigned count = 0;
        for (auto pointer : live) if (pointer) ++count;
        return count;
    }
}

void* operator new(std::size_t size)
{
    const bool injectFailure = P2Allocation::CanInjectFailure(size);
    if (injectFailure && P2Allocation::failAfter == 0)
    {
        P2Allocation::failAfter = -1;
        throw std::bad_alloc();
    }
    if (injectFailure && P2Allocation::failAfter > 0) --P2Allocation::failAfter;
    void* pointer = std::malloc(size ? size : 1);
    if (!pointer) throw std::bad_alloc();
    if (P2Allocation::track)
        for (auto& slot : P2Allocation::live)
            if (!slot) { slot = pointer; break; }
    return pointer;
}
void operator delete(void* pointer) noexcept
{
    for (auto& slot : P2Allocation::live) if (slot == pointer) slot = nullptr;
    std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
