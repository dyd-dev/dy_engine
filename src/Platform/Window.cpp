#include "Window.h"
#include <stdexcept>
#include <GLFW/glfw3.h>
// Expose native window handles for RHI Device initialization
#if defined(_WIN32)
	#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
	#define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(__linux__)
	#define GLFW_EXPOSE_NATIVE_X11
	// #define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#include <GLFW/glfw3native.h>

using namespace dy::Platform;

Window::Window(unsigned int width, unsigned int height)
	: Window(width, height, "New Window") {}

Window::Window(unsigned int width, unsigned int height, const char* title)
{
	if(!glfwInit()) throw std::runtime_error("Failed to initialize GLFW.");
	
	// Tell GLFW to NOT create an OpenGL context
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
	if(!m_window)
	{
		glfwTerminate();
		throw std::runtime_error("Failed to create GLFW window.");
	}
}

Window::~Window()
{
	if(m_window) glfwDestroyWindow(m_window);
	glfwTerminate();
}

bool Window::IsRunning() const { return !glfwWindowShouldClose(m_window); }

void Window::PollEvents() const { glfwPollEvents(); }

void* Window::GetHandle() const
{
#if defined(_WIN32)
	return static_cast<void*>(glfwGetWin32Window(m_window));
#elif defined(__APPLE__)
	return static_cast<void*>(glfwGetCocoaWindow(m_window));
#else
	// vulkan use GLFW_window
	return static_cast<void*>(m_window);
#endif
}