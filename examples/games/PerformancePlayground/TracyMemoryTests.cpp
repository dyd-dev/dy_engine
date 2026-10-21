#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <cstdio>

int main()
{
    try
    {
        void* zero = ::operator new(0);
        if(!zero) throw std::runtime_error("zero-sized new must return storage");
        ::operator delete(zero);
        void* array = ::operator new[](128, std::nothrow);
        if(!array) throw std::runtime_error("nothrow array allocation");
        std::memset(array, 42, 128);
        ::operator delete[](array, size_t(128));
        constexpr auto alignment = std::align_val_t(256);
        void* aligned = ::operator new(513, alignment, std::nothrow);
        if(!aligned || uintptr_t(aligned) % 256) throw std::runtime_error("over-aligned allocation");
        std::memset(aligned, 1, 513);
        ::operator delete(aligned, size_t(513), alignment);
        ::operator delete(nullptr);
        struct alignas(128) Throwing { Throwing() { throw 1; } };
        try { (void)new Throwing; } catch(int) {}
        std::puts("PASS zero, array, aligned, nothrow, sized delete and throwing constructor");
        return 0;
    }
    catch(const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
