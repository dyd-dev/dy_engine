#include <tracy/Tracy.hpp>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace
{
constexpr char HeapName[] = "Game C++ heap";

void* Allocate(size_t size, size_t alignment = 0)
{
    const size_t bytes = size ? size : 1;
    for(;;)
    {
        void* pointer = nullptr;
        if(!alignment) pointer = std::malloc(bytes);
#if defined(_WIN32)
        else pointer = _aligned_malloc(bytes, alignment);
#else
        else if(posix_memalign(&pointer, alignment, bytes) != 0) pointer = nullptr;
#endif
        if(pointer)
        {
            TracySecureAllocNS(pointer, bytes, 8, HeapName);
            return pointer;
        }
        const auto handler = std::get_new_handler();
        if(!handler) throw std::bad_alloc();
        handler();
    }
}

void Release(void* pointer, bool aligned = false) noexcept
{
    if(!pointer) return;
    TracySecureFreeN(pointer, HeapName);
#if defined(_WIN32)
    if(aligned) _aligned_free(pointer);
    else std::free(pointer);
#else
    (void)aligned;
    std::free(pointer);
#endif
}
}

void* operator new(size_t size) { return Allocate(size); }
void* operator new[](size_t size) { return Allocate(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { try { return Allocate(size); } catch(...) { return nullptr; } }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { try { return Allocate(size); } catch(...) { return nullptr; } }
void operator delete(void* pointer) noexcept { Release(pointer); }
void operator delete[](void* pointer) noexcept { Release(pointer); }
void operator delete(void* pointer, size_t) noexcept { Release(pointer); }
void operator delete[](void* pointer, size_t) noexcept { Release(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { Release(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { Release(pointer); }

void* operator new(size_t size, std::align_val_t alignment) { return Allocate(size, size_t(alignment)); }
void* operator new[](size_t size, std::align_val_t alignment) { return Allocate(size, size_t(alignment)); }
void* operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return Allocate(size, size_t(alignment)); } catch(...) { return nullptr; } }
void* operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept { try { return Allocate(size, size_t(alignment)); } catch(...) { return nullptr; } }
void operator delete(void* pointer, std::align_val_t) noexcept { Release(pointer, true); }
void operator delete[](void* pointer, std::align_val_t) noexcept { Release(pointer, true); }
void operator delete(void* pointer, size_t, std::align_val_t) noexcept { Release(pointer, true); }
void operator delete[](void* pointer, size_t, std::align_val_t) noexcept { Release(pointer, true); }
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { Release(pointer, true); }
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { Release(pointer, true); }
