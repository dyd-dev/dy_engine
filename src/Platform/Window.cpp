#include "Window.h"
#include <stdexcept>
#include <GLFW/glfw3.h>
// Expose native window handles for RHI Device initialization
#if defined(_WIN32)
	#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
	#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3native.h>

using namespace dy::Platform;

namespace
{
	bool g_f11Pressed = false;

	void OnKey(GLFWwindow*, int key, int, int action, int)
	{
		if(key == GLFW_KEY_F11 && action == GLFW_PRESS)
		{
			g_f11Pressed = true;
		}
	}
}

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

	glfwSetKeyCallback(m_window, OnKey);
}

Window::~Window()
{
	if(m_window) glfwDestroyWindow(m_window);
	glfwTerminate();
}

bool Window::IsRunning() const { return !glfwWindowShouldClose(m_window); }

void Window::PollEvents() const { glfwPollEvents(); }

void Window::Resize(unsigned int width, unsigned int height) const
{
	glfwSetWindowSize(m_window, static_cast<int>(width), static_cast<int>(height));
}

bool Window::ConsumeKeyPress(Key key)
{
	if(key != Key::F11 || !g_f11Pressed) return false;
	g_f11Pressed = false;
	return true;
}

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
