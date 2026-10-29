#include "../src/Backends/Vulkan/VulkanPresentWorker.h"

#include <atomic>
#include <cstdlib>
#include <cstdio>

#define CHECK(condition) do { if(!(condition)) { \
    std::fprintf(stderr,"CHECK failed at line %d: %s\n",__LINE__,#condition); \
    std::abort(); \
} } while(false)

namespace
{
    std::atomic<bool> entered{false}, releaseCall{false};
    std::atomic<unsigned> finished{0};
    struct Job { int value; bool blocked = false; };
    int Execute(const Job& job) noexcept
    {
        entered.store(true, std::memory_order_release);
        while(job.blocked && !releaseCall.load(std::memory_order_acquire)) std::this_thread::yield();
        finished.fetch_add(1,std::memory_order_relaxed);
        return -job.value;
    }
#if defined(_WIN32)
    unsigned sentMessages=0;
    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if(message == WM_APP) {++sentMessages;return 73;}
        return DefWindowProcW(window, message, wparam, lparam);
    }
    LRESULT SendToOwner(const HWND& window) noexcept { return SendMessageW(window, WM_APP, 0, 0); }
#endif
}

int main()
{
    using Worker = dyf::Backends::VulkanPresentWorker<Job, int>;
    Worker worker(Execute);
    Job input{7, true}, completed{};
    int result = 0;
    CHECK(worker.TrySubmit(input));
    input.value = 99;
    while(!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    CHECK(!worker.TrySubmit({8}));
    CHECK(!worker.Poll(completed, result));
    CHECK(worker.Pending());
    releaseCall.store(true, std::memory_order_release);
    worker.Wait();
    CHECK(!worker.TrySubmit({8})); // Ready results still own the mailbox.
    CHECK(worker.PeekResult(result) && result == -7);
    CHECK(worker.Poll(completed, result) && completed.value == 7 && result == -7);
    CHECK(!worker.Pending());
    for(int i = 1; i <= 10000; ++i)
    {
        CHECK(worker.TrySubmit({i}));
        worker.Wait();
        CHECK(worker.Poll(completed, result) && completed.value == i && result == -i);
    }
    const auto beforeDrain=finished.load();
    { Worker draining(Execute); CHECK(draining.TrySubmit({42})); }
    CHECK(finished.load()==beforeDrain+1);
#if defined(_WIN32)
    WNDCLASSW type{};
    type.lpfnWndProc = WindowProc; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"VulkanPresentWorkerTest";
    CHECK(RegisterClassW(&type));
    HWND window = CreateWindowW(type.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, nullptr);
    CHECK(window);
    {
        dyf::Backends::VulkanPresentWorker<HWND, LRESULT> sentMessage(SendToOwner);
        CHECK(sentMessage.TrySubmit(window));
        sentMessage.Wait();
        HWND completedWindow{}; LRESULT reply{};
        CHECK(sentMessage.Poll(completedWindow, reply) && completedWindow == window && reply == 73);
        CHECK(sentMessage.TrySubmit(window)); // Destruction also pumps sent messages while draining.
    }
    CHECK(sentMessages==2);
    DestroyWindow(window);
    UnregisterClassW(type.lpszClassName, type.hInstance);
#endif
    std::puts("PASS VulkanPresentWorker: immutable jobs, bounded ownership, failure result, wake/reuse, drain");
#if defined(_WIN32)
    std::puts("PASS VulkanPresentWorker: Win32 SendMessage");
#endif
}
