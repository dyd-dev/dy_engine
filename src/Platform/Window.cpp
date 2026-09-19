#include "Window.h"
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

using namespace dy::Platform;

namespace
{
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
	ValidateSize(width, height);
	if(windows.empty() && !glfwInit()) throw std::runtime_error("Failed to initialize GLFW.");
	
	// Tell GLFW to NOT create an OpenGL context
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	m_window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), title ? title : "New Window", nullptr, nullptr);
	if(!m_window)
	{
		if(windows.empty()) glfwTerminate();
		throw std::runtime_error("Failed to create GLFW window.");
	}

	try { windows.push_back(this); }
	catch(...)
	{
		glfwDestroyWindow(m_window);
		if(windows.empty()) glfwTerminate();
		throw;
	}
	glfwSetWindowUserPointer(m_window, this);
	glfwSetKeyCallback(m_window, [](GLFWwindow* handle, int key, int, int action, int)
	{
		auto& input = static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input;
		if(action != GLFW_REPEAT) input.OnButton(Input::Index(static_cast<Key>(key)), action == GLFW_PRESS);
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
	windows.erase(std::remove(windows.begin(), windows.end(), this), windows.end());
	glfwDestroyWindow(m_window);
	if(windows.empty()) glfwTerminate();
}

bool Window::IsRunning() const { return !glfwWindowShouldClose(m_window); }

void Window::RequestClose() const { glfwSetWindowShouldClose(m_window, GLFW_TRUE); }

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
	glfwSetWindowSize(m_window, static_cast<int>(width), static_cast<int>(height));
}

WindowSize Window::GetSize() const
{
	WindowSize size;
	glfwGetWindowSize(m_window, &size.width, &size.height);
	return size;
}

WindowSize Window::GetFramebufferSize() const
{
	WindowSize size;
	glfwGetFramebufferSize(m_window, &size.width, &size.height);
	return size;
}

ContentScale Window::GetContentScale() const
{
	ContentScale scale;
	glfwGetWindowContentScale(m_window, &scale.x, &scale.y);
	return scale;
}

bool Window::HasFocus() const { return glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE; }
bool Window::IsMinimized() const { return glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE; }

void Window::SetCursorMode(CursorMode mode)
{
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
	switch(glfwGetInputMode(m_window, GLFW_CURSOR))
	{
	case GLFW_CURSOR_DISABLED: return CursorMode::Locked;
	case GLFW_CURSOR_HIDDEN: return CursorMode::Hidden;
	default: return CursorMode::Normal;
	}
}

bool Window::ConsumeKeyPress(Key key)
{
	// ponytail: legacy HUD shortcut is application-wide; bind Renderer to a Window for independent HUDs.
	for(auto* window : windows)
		if(window->m_input.ConsumeKeyPress(key)) return true;
	return false;
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
