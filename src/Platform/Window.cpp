#include "dyf/Platform/Window.h"
#include "dyf/Platform/RenderDocCapture.h"
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
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

namespace
{
	unsigned int windowCount = 0;
	std::vector<Window*> windows;

	void ValidateSize(unsigned int width, unsigned int height)
	{
		const auto maxSize = static_cast<unsigned int>((std::numeric_limits<int>::max)());
		if(width == 0 || height == 0 || width > maxSize || height > maxSize)
			throw std::invalid_argument("Window dimensions must be positive and fit in int.");
	}
}

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
	glfwSetKeyCallback(m_window, [](GLFWwindow* handle, int key, int, int action, int)
	{
		auto& input = static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input;
		if(action != GLFW_REPEAT) input.OnButton(Input::Index(static_cast<Key>(key)), action == GLFW_PRESS);
        if(key == GLFW_KEY_F11 && action == GLFW_PRESS)
            static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_profilerToggle = true;
        if(key == GLFW_KEY_F12 && action == GLFW_PRESS) (void)RenderDocCapture::TriggerNextFrame();
	});
	glfwSetMouseButtonCallback(m_window, [](GLFWwindow* handle, int button, int action, int)
	{
		auto& input = static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input;
		input.OnButton(Input::Index(static_cast<MouseButton>(button)), action == GLFW_PRESS);
	});
	glfwSetCursorPosCallback(m_window, [](GLFWwindow* handle, double x, double y)
	{
		static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input.OnCursor(x, y);
	});
	glfwSetScrollCallback(m_window, [](GLFWwindow* handle, double x, double y)
	{
		auto& scroll = static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input.m_pending.scroll;
		scroll.x += x;
		scroll.y += y;
	});
	glfwSetWindowFocusCallback(m_window, [](GLFWwindow* handle, int focused)
	{
		auto& input = static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input;
		if(!focused) input.OnFocusLost();
		else
		{
			input.m_pending.delta = {};
			input.m_hasCursorPosition = false;
		}
	});
	glfwGetCursorPos(m_window, &m_input.m_pending.position.x, &m_input.m_pending.position.y);
	m_input.m_hasCursorPosition = true;
	m_input.PublishFrame();
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

void Window::RequestClose() const { if(m_window) glfwSetWindowShouldClose(m_window, GLFW_TRUE); }

void Window::PollEvents()
{
	if(windows.empty()) return;
	glfwPollEvents();
	for(auto* window : windows) window->m_input.PublishFrame();
}

void Window::WaitEvents(double timeoutSeconds)
{
	if(!std::isfinite(timeoutSeconds) || timeoutSeconds <= 0)
		throw std::invalid_argument("Event wait timeout must be finite and positive.");
	if(!windows.empty()) glfwWaitEventsTimeout(timeoutSeconds);
}

void Window::Resize(unsigned int width, unsigned int height) const
{
	ValidateSize(width, height);
	if(m_window) glfwSetWindowSize(m_window, static_cast<int>(width), static_cast<int>(height));
}

WindowSize Window::GetSize() const
{
	WindowSize size;
	if(m_window) glfwGetWindowSize(m_window, &size.width, &size.height);
	return size;
}

WindowSize Window::GetFramebufferSize() const
{
	WindowSize size;
	if(m_window) glfwGetFramebufferSize(m_window, &size.width, &size.height);
	return size;
}

ContentScale Window::GetContentScale() const
{
	ContentScale scale;
	if(m_window) glfwGetWindowContentScale(m_window, &scale.x, &scale.y);
	return scale;
}

bool Window::HasFocus() const { return m_window && glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE; }
bool Window::IsMinimized() const { return m_window && glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE; }

void Window::SetCursorMode(CursorMode mode)
{
    if(!m_window) return;
	int nativeMode;
	switch(mode)
	{
	case CursorMode::Normal: nativeMode = GLFW_CURSOR_NORMAL; break;
	case CursorMode::Hidden: nativeMode = GLFW_CURSOR_HIDDEN; break;
	case CursorMode::Locked: nativeMode = GLFW_CURSOR_DISABLED; break;
	default: throw std::invalid_argument("Unknown cursor mode.");
	}
	if(glfwGetInputMode(m_window, GLFW_CURSOR) == nativeMode) return;
	glfwSetInputMode(m_window, GLFW_CURSOR, nativeMode);
	glfwGetCursorPos(m_window, &m_input.m_pending.position.x, &m_input.m_pending.position.y);
	m_input.m_hasCursorPosition = true;
	m_input.m_pending.delta = {};
	m_input.m_frame.delta = {};
}

CursorMode Window::GetCursorMode() const
{
    if(!m_window) return CursorMode::Normal;
	switch(glfwGetInputMode(m_window, GLFW_CURSOR))
	{
	case GLFW_CURSOR_DISABLED: return CursorMode::Locked;
	case GLFW_CURSOR_HIDDEN: return CursorMode::Hidden;
	default: return CursorMode::Normal;
	}
}

bool Window::ConsumeKeyPress(Key key, const void* nativeWindow)
{
    if(key != Key::F11) return false;
    for(auto* window : windows)
        if(window->GetHandle() == nativeWindow && window->m_profilerToggle)
        { window->m_profilerToggle = false; return true; }
    return false;
}

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
