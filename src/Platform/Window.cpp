#include "dyf/Platform/Log.h"
#include "dyf/Platform/Window.h"
#include "dyf/Platform/RenderDocCapture.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>
#if !defined(__ANDROID__)
#include <GLFW/glfw3.h>
// Expose native window handles for RHI Device initialization
#if defined(_WIN32)
	#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
	#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3native.h>
#else
#include <android/native_window.h>
#include <android_native_app_glue.h>
#endif

using namespace dyf::Platform;

namespace
{
	std::vector<Window*> windows;

#if defined(__ANDROID__)
	static struct android_app* s_androidApp = nullptr;

	int32_t AndroidInputCallback(struct android_app*, AInputEvent* event)
	{
		const int32_t eventType = AInputEvent_getType(event);
		if(eventType == AINPUT_EVENT_TYPE_MOTION)
		{
			const int32_t action = AMotionEvent_getAction(event);
			const int32_t actionMasked = action & AMOTION_EVENT_ACTION_MASK;
			const float x = AMotionEvent_getX(event, 0);
			const float y = AMotionEvent_getY(event, 0);

			for(auto* window : windows)
			{
				InputEvent cursorEv{};
				cursorEv.type = InputEventType::Cursor;
				cursorEv.x = x;
				cursorEv.y = y;
				window->HandleInputEvent(cursorEv);

				if(actionMasked == AMOTION_EVENT_ACTION_DOWN || actionMasked == AMOTION_EVENT_ACTION_POINTER_DOWN)
				{
					InputEvent btnEv{};
					btnEv.type = InputEventType::MouseButton;
					btnEv.code = static_cast<int>(MouseButton::Left);
					btnEv.action = InputAction::Press;
					window->HandleInputEvent(btnEv);
				}
				else if(actionMasked == AMOTION_EVENT_ACTION_UP || actionMasked == AMOTION_EVENT_ACTION_POINTER_UP || actionMasked == AMOTION_EVENT_ACTION_CANCEL)
				{
					InputEvent btnEv{};
					btnEv.type = InputEventType::MouseButton;
					btnEv.code = static_cast<int>(MouseButton::Left);
					btnEv.action = InputAction::Release;
					window->HandleInputEvent(btnEv);
				}
			}
			return 1;
		}
		else if(eventType == AINPUT_EVENT_TYPE_KEY)
		{
			const int32_t keyCode = AKeyEvent_getKeyCode(event);
			const int32_t action = AKeyEvent_getAction(event);
			if(keyCode == AKEYCODE_BACK && action == AKEY_EVENT_ACTION_UP)
			{
				for(auto* window : windows)
				{
					window->RequestClose();
				}
				return 1;
			}
		}
		return 0;
	}
#else
	void ValidateSize(unsigned int width, unsigned int height)
	{
		const auto maxSize = static_cast<unsigned int>((std::numeric_limits<int>::max)());
		if(width == 0 || height == 0 || width > maxSize || height > maxSize)
			throw std::invalid_argument("Window dimensions must be positive and fit in int.");
	}
#endif
}

Window::Window(unsigned int width, unsigned int height)
	: Window(width, height, "New Window") {}

#if defined(__ANDROID__)
void Window::SetAndroidApp(struct android_app* app)
{
	s_androidApp = app;
	if(s_androidApp)
	{
		s_androidApp->onInputEvent = AndroidInputCallback;
	}
}

struct android_app* Window::GetAndroidApp()
{
	return s_androidApp;
}

Window::Window(unsigned int width, unsigned int height, const char* title)
{
	(void)width; (void)height; (void)title;
	Log::Initialize();
	if(s_androidApp)
	{
		// Wait for ANativeWindow to become available
		while(!s_androidApp->window && !s_androidApp->destroyRequested)
		{
			int events = 0;
			struct android_poll_source* source = nullptr;
			while(ALooper_pollOnce(-1, nullptr, &events, (void**)&source) >= 0)
			{
				if(source) source->process(s_androidApp, source);
				if(s_androidApp->window || s_androidApp->destroyRequested) break;
			}
		}
		m_nativeWindow = s_androidApp->window;
		if(m_nativeWindow)
		{
			ANativeWindow_setBuffersGeometry(static_cast<ANativeWindow*>(m_nativeWindow), 0, 0, WINDOW_FORMAT_RGBA_8888);
		}
		s_androidApp->onInputEvent = AndroidInputCallback;
	}
	windows.push_back(this);
	m_input.m_pending.focused = true;
	m_input.PublishFrame();
}
#else
Window::Window(unsigned int width, unsigned int height, const char* title)
{
    Log::Initialize();
	if(!width || !height || width > static_cast<unsigned>((std::numeric_limits<int>::max)()) || height > static_cast<unsigned>((std::numeric_limits<int>::max)()))
    { dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "Platform", __FILE__, __LINE__, "dyf: invalid window dimensions."); return; }
    if(windows.empty() && !glfwInit())
    { dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "Platform", __FILE__, __LINE__, "dyf: failed to initialize GLFW."); return; }
	
	// Tell GLFW to NOT create an OpenGL context
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	m_window = glfwCreateWindow(width, height, title ? title : "New Window", nullptr, nullptr);
	if(!m_window)
	{
		if(windows.empty()) glfwTerminate();
		dyf::Platform::Log::Writef(dyf::Platform::LogLevel::Error, "Platform", __FILE__, __LINE__, "dyf: failed to create GLFW window.");
		return;
	}
    try
    {
    windows.push_back(this);
    glfwSetWindowUserPointer(m_window, this);
    (void)RenderDocCapture::Initialize();
	glfwSetKeyCallback(m_window, [](GLFWwindow* handle, int key, int scan, int action, int mods)
	{
		InputEvent event; event.type = InputEventType::Key; event.code = key; event.scancode = scan;
        event.action = static_cast<InputAction>(action); event.modifiers = mods;
        if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle))) window->HandleInputEvent(event);
	});
	glfwSetMouseButtonCallback(m_window, [](GLFWwindow* handle, int button, int action, int mods)
	{
		InputEvent event; event.type = InputEventType::MouseButton; event.code = button;
		event.action = static_cast<InputAction>(action); event.modifiers = mods;
		if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle))) window->HandleInputEvent(event);
	});
	glfwSetCursorPosCallback(m_window, [](GLFWwindow* handle, double x, double y)
	{
		InputEvent event; event.type = InputEventType::Cursor; event.x = x; event.y = y;
		if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle))) window->HandleInputEvent(event);
	});
	glfwSetScrollCallback(m_window, [](GLFWwindow* handle, double x, double y)
	{
		InputEvent event; event.type = InputEventType::Scroll; event.x = x; event.y = y;
		if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle))) window->HandleInputEvent(event);
	});
	glfwSetCharCallback(m_window, [](GLFWwindow* handle, unsigned int codepoint)
	{
		InputEvent event; event.type = InputEventType::Text; event.codepoint = codepoint;
		if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle))) window->HandleInputEvent(event);
	});
	glfwSetWindowFocusCallback(m_window, [](GLFWwindow* handle, int focused)
	{
		InputEvent event; event.type = InputEventType::Focus; event.focused = focused != 0;
		if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle))) window->HandleInputEvent(event);
	});
	m_input.m_pending.focused = HasFocus();
	glfwGetCursorPos(m_window, &m_input.m_pending.position.x, &m_input.m_pending.position.y);
	m_input.m_hasCursorPosition = true;
	m_input.PublishFrame();
    }
    catch(...)
    {
        windows.erase(std::remove(windows.begin(), windows.end(), this), windows.end());
        glfwSetWindowUserPointer(m_window, nullptr);
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        if(windows.empty()) glfwTerminate();
        throw;
    }
}
#endif

void Window::HandleInputEvent(const InputEvent& event) noexcept
{
    try
    {
        m_input.OnEvent(event);
#if !defined(__ANDROID__)
        if(event.type == InputEventType::Key && event.action == InputAction::Press)
        {
            if(event.code == GLFW_KEY_F11) m_profilerToggle = true;
            if(event.code == GLFW_KEY_F12) (void)RenderDocCapture::TriggerNextFrame();
        }
#endif
    }
    catch(...)
    {
        // Keep the first error without allocating/logging from a native callback.
        if(!m_callbackError) m_callbackError = std::current_exception();
    }
}

void Window::RethrowCallbackError()
{
    for(auto* window : windows)
        if(window->m_callbackError)
            std::rethrow_exception(std::exchange(window->m_callbackError, {}));
}

#if defined(__ANDROID__)
Window::~Window()
{
	windows.erase(std::remove(windows.begin(), windows.end(), this), windows.end());
	m_nativeWindow = nullptr;
}

bool Window::IsRunning() const { return !m_shouldClose && (!s_androidApp || !s_androidApp->destroyRequested); }

void Window::RequestClose() const
{
	m_shouldClose = true;
	if(s_androidApp && s_androidApp->activity)
		ANativeActivity_finish(s_androidApp->activity);
}

void Window::PollEvents()
{
	if(s_androidApp)
	{
		int events = 0;
		struct android_poll_source* source = nullptr;
		while(ALooper_pollOnce(0, nullptr, &events, (void**)&source) >= 0)
		{
			if(source) source->process(s_androidApp, source);
		}
	}
	for(auto* window : windows)
	{
		try { window->m_input.PublishFrame(); }
		catch(...) { if(!window->m_callbackError) window->m_callbackError = std::current_exception(); }
	}
	RethrowCallbackError();
}

void Window::WaitEvents(double timeoutSeconds)
{
	if(!std::isfinite(timeoutSeconds) || timeoutSeconds <= 0)
		throw std::invalid_argument("Event wait timeout must be finite and positive.");
	if(s_androidApp)
	{
		int events = 0;
		struct android_poll_source* source = nullptr;
		const int timeoutMs = static_cast<int>(timeoutSeconds * 1000.0);
		if(ALooper_pollOnce(timeoutMs, nullptr, &events, (void**)&source) >= 0)
		{
			if(source) source->process(s_androidApp, source);
		}
	}
	RethrowCallbackError();
}

void Window::Resize(unsigned int, unsigned int) const {}

WindowSize Window::GetSize() const
{
	WindowSize size;
	auto* handle = static_cast<ANativeWindow*>(GetHandle());
	if(handle)
	{
		size.width = ANativeWindow_getWidth(handle);
		size.height = ANativeWindow_getHeight(handle);
	}
	return size;
}

WindowSize Window::GetFramebufferSize() const { return GetSize(); }

ContentScale Window::GetContentScale() const { return {1.0f, 1.0f}; }

bool Window::HasFocus() const { return true; }
bool Window::IsMinimized() const { return false; }

void Window::SetCursorMode(CursorMode) {}
void Window::ResetCursorDelta() {}
CursorMode Window::GetCursorMode() const { return CursorMode::Normal; }

bool Window::IsRawMouseMotionSupported() const { return false; }
bool Window::SetRawMouseMotion(bool) { return false; }
bool Window::IsRawMouseMotionEnabled() const { return false; }

bool Window::ConsumeKeyPress(Key, const void*) { return false; }

void* Window::GetHandle() const
{
	if(s_androidApp && s_androidApp->window) return s_androidApp->window;
	return m_nativeWindow;
}

#else

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

void Window::RequestClose() const { if(m_window) glfwSetWindowShouldClose(m_window, GLFW_TRUE); }

void Window::PollEvents()
{
	if(windows.empty()) return;
	glfwPollEvents();
	for(auto* window : windows)
    {
        try { window->m_input.PublishFrame(); }
        catch(...) { if(!window->m_callbackError) window->m_callbackError = std::current_exception(); }
    }
    RethrowCallbackError();
}

void Window::WaitEvents(double timeoutSeconds)
{
	if(!std::isfinite(timeoutSeconds) || timeoutSeconds <= 0)
		throw std::invalid_argument("Event wait timeout must be finite and positive.");
	if(!windows.empty()) glfwWaitEventsTimeout(timeoutSeconds);
    RethrowCallbackError();
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
	ResetCursorDelta();
}

void Window::ResetCursorDelta()
{
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

bool Window::IsRawMouseMotionSupported() const { return glfwRawMouseMotionSupported() == GLFW_TRUE; }

bool Window::SetRawMouseMotion(bool enabled)
{
    if(!m_window) return false;
	if(!IsRawMouseMotionSupported()) return !enabled;
	if(IsRawMouseMotionEnabled() == enabled) return true;
	glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, enabled ? GLFW_TRUE : GLFW_FALSE);
	ResetCursorDelta();
	return IsRawMouseMotionEnabled() == enabled;
}

bool Window::IsRawMouseMotionEnabled() const
{
	return m_window && glfwGetInputMode(m_window, GLFW_RAW_MOUSE_MOTION) == GLFW_TRUE;
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
#endif
