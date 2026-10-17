#pragma once
#include "dyf/Platform/Window.h"
#include <vector>

namespace dyf::Platform
{
struct WindowDescription
{
    unsigned int width=0,height=0;
    const char* title=nullptr;
    NativeWindowHandle borrowed;
};
class WindowBackend
{
public:
    explicit WindowBackend(Window& window):owner(window) {}
    virtual ~WindowBackend()=default;
    virtual bool IsRunning() const=0;
    virtual void RequestClose() const=0;
    virtual void Resize(unsigned int,unsigned int) const=0;
    virtual WindowSize GetSize() const=0;
    virtual WindowSize GetFramebufferSize() const=0;
    virtual ContentScale GetContentScale() const=0;
    virtual bool HasFocus() const=0;
    virtual bool IsMinimized() const=0;
    virtual void SetCursorMode(CursorMode)=0;
    virtual CursorMode GetCursorMode() const=0;
    virtual bool IsRawMouseMotionSupported() const=0;
    virtual bool SetRawMouseMotion(bool)=0;
    virtual bool IsRawMouseMotionEnabled() const=0;
    virtual NativeWindowHandle GetNativeHandle() const=0;
    virtual void* GetGlfwWindow() const noexcept { return nullptr; }
protected:
    void ResetCursorPosition(double x,double y);
    Window& owner;
};
struct WindowPlatformAccess
{
    static WindowBackend* Backend(const Window& window) noexcept { return window.m_backend.get(); }
};
const std::vector<Window*>& RegisteredWindows() noexcept;
std::unique_ptr<WindowBackend> CreateWindowBackend(Window&,const WindowDescription&);
void PollPlatformEvents();
void WaitPlatformEvents(double);
}
