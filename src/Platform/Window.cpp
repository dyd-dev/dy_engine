#include "dyf/Platform/Log.h"
#include "dyf/Platform/Window.h"
#include <cstdio>
#include <limits>
#include <GLFW/glfw3.h>
// Expose native window handles for RHI Device initialization
#if defined(_WIN32)
	#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
	#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3native.h>

using namespace dyf::Platform;

namespace { unsigned int windowCount = 0; }

Window::Window(unsigned int width, unsigned int height)
	: Window(width, height, "New Window") {}

Window::Window(unsigned int width, unsigned int height, const char* title)
{
    Log::Initialize();
	if(!width || !height || width > static_cast<unsigned>((std::numeric_limits<int>::max)()) || height > static_cast<unsigned>((std::numeric_limits<int>::max)()))
    { dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "Platform", __FILE__, __LINE__, "dyf: invalid window dimensions."); return; }
    if(windowCount == 0 && !glfwInit())
    { dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "Platform", __FILE__, __LINE__, "dyf: failed to initialize GLFW."); return; }
	
	// Tell GLFW to NOT create an OpenGL context
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	m_window = glfwCreateWindow(width, height, title ? title : "New Window", nullptr, nullptr);
	if(!m_window)
	{
		if(windowCount == 0) glfwTerminate();
		dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "Platform", __FILE__, __LINE__, "dyf: failed to create GLFW window.");
		return;
	}
    ++windowCount;
}

Window::~Window()
{
	if(m_window)
    {
        glfwDestroyWindow(m_window);
        if(--windowCount == 0) glfwTerminate();
    }
}

bool Window::IsRunning() const { return m_window && !glfwWindowShouldClose(m_window); }

void Window::PollEvents() const { if(m_window) glfwPollEvents(); }

void* Window::GetHandle() const
{
    if(!m_window) return nullptr;
#if defined(_WIN32)
	return static_cast<void*>(glfwGetWin32Window(m_window));
#elif defined(__APPLE__)
	return static_cast<void*>(glfwGetCocoaWindow(m_window));
#else
	// vulkan use GLFW_window
	return static_cast<void*>(m_window);
#endif
}
