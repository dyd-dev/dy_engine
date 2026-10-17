#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "../Glfw/GlfwWindow.h"
#include <stdexcept>
namespace dyf::Platform
{
class WindowsWindow final : public GlfwWindow
{
public:
    using GlfwWindow::GlfwWindow;
    NativeWindowHandle GetNativeHandle() const override
    { return {NativeWindowKind::Win32,m_window?static_cast<void*>(glfwGetWin32Window(m_window)):nullptr}; }
};
std::unique_ptr<WindowBackend> CreateWindowBackend(Window& owner,const WindowDescription& desc)
{
    if(desc.borrowed.kind!=NativeWindowKind::None)throw std::invalid_argument("Windows Window owns its GLFW window.");
    return std::make_unique<WindowsWindow>(owner,desc);
}
}
