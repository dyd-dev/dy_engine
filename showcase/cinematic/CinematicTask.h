#pragma once
#include <coroutine>
#include <exception>
#include <utility>

namespace showcase
{
enum class PresentationStep { Frame, Ready, Next, Restart };
struct PresentationPlayback
{
    bool preloading=true, activate=false, startPaused=false, verification=false;
};
inline unsigned NextPresentationScene(unsigned current, PresentationStep step)
{
    if(step==PresentationStep::Restart) return 0;
    if(step==PresentationStep::Next) return current<2 ? current+1 : 2;
    return current;
}

// Keep each scene's renderer and assets alive while another scene uses the same window/device.
class CinematicTask
{
public:
    struct promise_type
    {
        PresentationStep step=PresentationStep::Frame;
        int result=0;
        std::exception_ptr error;
        CinematicTask get_return_object() { return CinematicTask{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        std::suspend_always yield_value(PresentationStep value) noexcept { step=value; return {}; }
        void return_value(int value) noexcept { result=value; }
        void unhandled_exception() noexcept { error=std::current_exception(); }
    };
    explicit CinematicTask(std::coroutine_handle<promise_type> handle):m_handle(handle) {}
    CinematicTask(CinematicTask&& other) noexcept:m_handle(std::exchange(other.m_handle,{})) {}
    CinematicTask(const CinematicTask&)=delete;
    ~CinematicTask() { if(m_handle) m_handle.destroy(); }
    PresentationStep Resume()
    {
        if(!m_handle.done()) m_handle.resume();
        if(m_handle.promise().error) std::rethrow_exception(m_handle.promise().error);
        return m_handle.promise().step;
    }
    bool Done() const { return m_handle.done(); }
    int Result() const { return m_handle.promise().result; }
private:
    std::coroutine_handle<promise_type> m_handle;
};
}
