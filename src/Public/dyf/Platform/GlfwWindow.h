#pragma once
struct GLFWwindow;
namespace dyf::Platform
{
class Window;
// Optional GLFW integration. Null for a Window backed by another platform API.
[[nodiscard]] GLFWwindow* GetGlfwWindow(const Window&) noexcept;
}
