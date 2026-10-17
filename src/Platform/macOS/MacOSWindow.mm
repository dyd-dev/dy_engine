#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include "../Glfw/GlfwWindow.h"
#include <stdexcept>
namespace dyf::Platform
{
class MacOSWindow final : public GlfwWindow
{
public:
    using GlfwWindow::GlfwWindow;
    NativeWindowHandle GetNativeHandle() const override
    { return {NativeWindowKind::Cocoa,m_window?(__bridge void*)glfwGetCocoaWindow(m_window):nullptr}; }
};
std::unique_ptr<WindowBackend> CreateWindowBackend(Window& owner,const WindowDescription& desc)
{
    if(desc.borrowed.kind!=NativeWindowKind::None)throw std::invalid_argument("macOS Window owns its GLFW window.");
    return std::make_unique<MacOSWindow>(owner,desc);
}
}
