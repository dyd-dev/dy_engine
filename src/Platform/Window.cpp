#include "dyf/Platform/Window.h"
#include "dyf/Platform/RenderDocCapture.h"
#include <cstdio>
#include <limits>
#include <algorithm>
#include <vector>
#include <GLFW/glfw3.h>
// Expose native window handles for RHI Device initialization
#if defined(_WIN32)
	#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
	#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3native.h>

using namespace dyf::Platform;

namespace { unsigned int windowCount = 0; std::vector<Window*> windows; }

Window::Window(unsigned int width, unsigned int height)
	: Window(width, height, "New Window") {}

Window::Window(unsigned int width, unsigned int height, const char* title)
{
	if(!width || !height || width > static_cast<unsigned>((std::numeric_limits<int>::max)()) || height > static_cast<unsigned>((std::numeric_limits<int>::max)()))
    { std::fprintf(stderr, "dyf: invalid window dimensions.\n"); return; }
    if(windowCount == 0 && !glfwInit())
    { std::fprintf(stderr, "dyf: failed to initialize GLFW.\n"); return; }
	
	// Tell GLFW to NOT create an OpenGL context
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	m_window = glfwCreateWindow(width, height, title ? title : "New Window", nullptr, nullptr);
	if(!m_window)
	{
		if(windowCount == 0) glfwTerminate();
		std::fprintf(stderr, "dyf: failed to create GLFW window.\n");
		return;
	}
    ++windowCount;
    windows.push_back(this);
    glfwSetWindowUserPointer(m_window, this);
    (void)RenderDocCapture::Initialize();
    glfwSetKeyCallback(m_window, [](GLFWwindow* window, int key, int, int action, int) {
        if(key == GLFW_KEY_F11 && action == GLFW_PRESS)
            static_cast<Window*>(glfwGetWindowUserPointer(window))->m_profilerToggle = true;
        if(key == GLFW_KEY_F12 && action == GLFW_PRESS)
            (void)RenderDocCapture::TriggerNextFrame();
    });
}

Window::~Window()
{
	if(m_window)
    {
		glfwDestroyWindow(m_window);
        windows.erase(std::remove(windows.begin(),windows.end(),this),windows.end());
        if(--windowCount == 0) glfwTerminate();
    }
}

bool Window::IsRunning() const { return m_window && !glfwWindowShouldClose(m_window); }

bool Window::ConsumeKeyPress(Key key, const void* nativeWindow)
{
    if(key != Key::F11) return false;
    for(auto* window : windows)
        if(window->GetHandle() == nativeWindow && window->m_profilerToggle)
        { window->m_profilerToggle = false; return true; }
    return false;
}

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
