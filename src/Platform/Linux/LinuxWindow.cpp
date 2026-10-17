#include "../Glfw/GlfwWindow.h"
#include <stdexcept>
namespace dyf::Platform
{
class LinuxWindow final : public GlfwWindow
{
public:
    using GlfwWindow::GlfwWindow;
    NativeWindowHandle GetNativeHandle() const override { return {NativeWindowKind::Glfw,m_window}; }
};
std::unique_ptr<WindowBackend> CreateWindowBackend(Window& owner,const WindowDescription& desc)
{
    if(desc.borrowed.kind!=NativeWindowKind::None)throw std::invalid_argument("Linux Window owns its GLFW window.");
    return std::make_unique<LinuxWindow>(owner,desc);
}
}
