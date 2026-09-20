#include "dyf/Platform/Window.h"
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

namespace { std::vector<Window*> windows; }

Window::Window(unsigned int width, unsigned int height)
	: Window(width, height, "New Window") {}

Window::Window(unsigned int width, unsigned int height, const char* title)
{
	if(!width || !height || width > static_cast<unsigned>((std::numeric_limits<int>::max)()) || height > static_cast<unsigned>((std::numeric_limits<int>::max)()))
    { std::fprintf(stderr, "dyf: invalid window dimensions.\n"); return; }
    if(windows.empty() && !glfwInit())
    { std::fprintf(stderr, "dyf: failed to initialize GLFW.\n"); return; }
	
	// Tell GLFW to NOT create an OpenGL context
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	m_window = glfwCreateWindow(width, height, title ? title : "New Window", nullptr, nullptr);
	if(!m_window)
	{
		if(windows.empty()) glfwTerminate();
		std::fprintf(stderr, "dyf: failed to create GLFW window.\n");
		return;
	}
    try { windows.push_back(this); }
    catch(...) { glfwDestroyWindow(m_window); if(windows.empty()) glfwTerminate(); throw; }
    glfwSetWindowUserPointer(m_window, this);
    glfwSetKeyCallback(m_window, [](GLFWwindow* handle,int key,int scan,int action,int mods)
    {
        auto& input=static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input;
        if(action!=GLFW_REPEAT) input.OnButton(Input::Index(static_cast<Key>(key)),action==GLFW_PRESS);
        InputEvent event; event.type=InputEventType::Key; event.code=key; event.scancode=scan;
        event.action=static_cast<InputAction>(action); event.modifiers=mods;
        input.m_pendingEvents.push_back(event);
    });
    glfwSetMouseButtonCallback(m_window, [](GLFWwindow* handle,int button,int action,int mods)
    {
        auto& input=static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input;
        input.OnButton(Input::Index(static_cast<MouseButton>(button)),action==GLFW_PRESS);
        InputEvent event; event.type=InputEventType::MouseButton; event.code=button;
        event.action=static_cast<InputAction>(action); event.modifiers=mods;
        input.m_pendingEvents.push_back(event);
    });
    glfwSetCursorPosCallback(m_window, [](GLFWwindow* handle,double x,double y)
    {
        auto& input=static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input;
        input.OnCursor(x,y);
        InputEvent event; event.type=InputEventType::Cursor; event.x=x; event.y=y;
        input.m_pendingEvents.push_back(event);
    });
    glfwSetScrollCallback(m_window, [](GLFWwindow* handle,double x,double y)
    {
        auto& input=static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input;
        input.m_pending.scroll.x+=x; input.m_pending.scroll.y+=y;
        InputEvent event; event.type=InputEventType::Scroll; event.x=x; event.y=y;
        input.m_pendingEvents.push_back(event);
    });
    glfwSetCharCallback(m_window, [](GLFWwindow* handle,unsigned int codepoint)
    {
        auto& input=static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input;
        InputEvent event; event.type=InputEventType::Text; event.codepoint=codepoint;
        input.m_pendingEvents.push_back(event);
    });
    glfwSetWindowFocusCallback(m_window, [](GLFWwindow* handle,int focused)
    {
        auto& input=static_cast<Window*>(glfwGetWindowUserPointer(handle))->m_input;
        if(!focused) input.OnFocusLost();
        else { input.m_pending.delta={}; input.m_hasCursorPosition=false; }
        InputEvent event; event.type=InputEventType::Focus; event.focused=focused!=0;
        input.m_pendingEvents.push_back(event);
    });
    glfwGetCursorPos(m_window,&m_input.m_pending.position.x,&m_input.m_pending.position.y);
    m_input.m_hasCursorPosition=true;
    m_input.PublishFrame();
}

Window::~Window()
{
	if(m_window)
    {
        glfwDestroyWindow(m_window);
        windows.erase(std::remove(windows.begin(),windows.end(),this),windows.end());
        if(windows.empty()) glfwTerminate();
    }
}

bool Window::IsRunning() const { return m_window && !glfwWindowShouldClose(m_window); }

void Window::PollEvents() const
{
    if(!m_window) return;
    // GLFW pumps all windows. Publish all snapshots exactly once per application frame.
    glfwPollEvents();
    for(auto* window:windows) window->m_input.PublishFrame();
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
