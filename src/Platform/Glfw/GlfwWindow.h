#pragma once
#include "../WindowBackend.h"
struct GLFWwindow;
namespace dyf::Platform
{
class GlfwWindow : public WindowBackend
{
public:
    GlfwWindow(Window&,const WindowDescription&);
    ~GlfwWindow() override;
    bool IsRunning() const override;
    void RequestClose() const override;
    void Resize(unsigned int,unsigned int) const override;
    WindowSize GetSize() const override;
    WindowSize GetFramebufferSize() const override;
    ContentScale GetContentScale() const override;
    bool HasFocus() const override;
    bool IsMinimized() const override;
    void SetCursorMode(CursorMode) override;
    CursorMode GetCursorMode() const override;
    bool IsRawMouseMotionSupported() const override;
    bool SetRawMouseMotion(bool) override;
    bool IsRawMouseMotionEnabled() const override;
    void* GetGlfwWindow() const noexcept override { return m_window; }
protected:
    GLFWwindow* m_window=nullptr;
private:
    void ResetCursorDelta();
};
}
