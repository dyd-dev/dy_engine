#include "GlfwWindow.h"
#include <cstdio>
#include <GLFW/glfw3.h>
#include <limits>
#include <stdexcept>

namespace dyf::Platform
{
	namespace
	{
		unsigned int liveWindows = 0;
	}

	GlfwWindow::GlfwWindow(Window& window, const WindowDescription& desc) : WindowBackend(window)
	{
		const auto width = desc.width, height = desc.height;
		const auto* title = desc.title;
		if(!width || !height || width > static_cast<unsigned>((std::numeric_limits<int>::max)()) ||
			height > static_cast<unsigned>((std::numeric_limits<int>::max)()))
		{
			std::fprintf(stderr, "[Platform] dyf: invalid window dimensions.\n");
			return;
		}
		if(liveWindows == 0 && !glfwInit())
		{
			std::fprintf(stderr, "[Platform] dyf: failed to initialize GLFW.\n");
			return;
		}

		// Tell GLFW to NOT create an OpenGL context
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		m_window = glfwCreateWindow(width, height, title ? title : "New Window", nullptr, nullptr);
		if(!m_window)
		{
			std::fprintf(stderr, "[Platform] dyf: failed to create GLFW window.\n");
			return;
		}

		try
		{
			++liveWindows;
			glfwSetWindowUserPointer(m_window, &owner);
			glfwSetKeyCallback(m_window, [](GLFWwindow* handle, int key, int scan, int action, int mods)
			{
				InputEvent event;
				event.type = InputEventType::Key;
				event.code = key;
				event.scancode = scan;
				event.action = static_cast<InputAction>(action);
				event.modifiers = mods;
				if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle))) WindowPlatformAccess::HandleInputEvent(*window,event);
			});
			glfwSetMouseButtonCallback(m_window, [](GLFWwindow* handle, int button, int action, int mods)
			{
				InputEvent event;
				event.type = InputEventType::MouseButton;
				event.code = button;
				event.action = static_cast<InputAction>(action);
				event.modifiers = mods;
				if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle))) WindowPlatformAccess::HandleInputEvent(*window,event);
			});
			glfwSetCursorPosCallback(m_window, [](GLFWwindow* handle, double x, double y)
			{
				InputEvent event;
				event.type = InputEventType::Cursor;
				event.x = x;
				event.y = y;
				if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle))) WindowPlatformAccess::HandleInputEvent(*window,event);
			});
			glfwSetScrollCallback(m_window, [](GLFWwindow* handle, double x, double y)
			{
				InputEvent event;
				event.type = InputEventType::Scroll;
				event.x = x;
				event.y = y;
				if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle))) WindowPlatformAccess::HandleInputEvent(*window,event);
			});
			glfwSetCharCallback(m_window, [](GLFWwindow* handle, unsigned int codepoint)
			{
				InputEvent event;
				event.type = InputEventType::Text;
				event.codepoint = codepoint;
				if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle))) WindowPlatformAccess::HandleInputEvent(*window,event);
			});
			glfwSetWindowFocusCallback(m_window, [](GLFWwindow* handle, int focused)
			{
				InputEvent event;
				event.type = InputEventType::Focus;
				event.focused = focused == GLFW_TRUE;
				if(auto* window = static_cast<Window*>(glfwGetWindowUserPointer(handle)))
					WindowPlatformAccess::HandleInputEvent(*window,event);
			});

			InputEvent focus;
			focus.type = InputEventType::Focus;
			focus.focused = HasFocus();
			WindowPlatformAccess::HandleInputEvent(owner,focus);
			ResetCursorDelta();
		}
		catch(...)
		{
			--liveWindows;
			glfwSetWindowUserPointer(m_window, nullptr);
			glfwDestroyWindow(m_window);
			m_window = nullptr;
			throw;
		}
	}

	GlfwWindow::~GlfwWindow()
	{
		if(m_window)
		{
			glfwSetWindowUserPointer(m_window, nullptr);
			glfwDestroyWindow(m_window);
            // GLFW is process-global; a library window cannot terminate the host's windows.
            --liveWindows;
		}
	}

	bool GlfwWindow::IsRunning() const { return m_window && !glfwWindowShouldClose(m_window); }
	void GlfwWindow::RequestClose() const { if(m_window) glfwSetWindowShouldClose(m_window, GLFW_TRUE); }
	bool GlfwWindow::CancelClose() const
	{
		if(!m_window) return false;
		glfwSetWindowShouldClose(m_window, GLFW_FALSE);
		return true;
	}
	void GlfwWindow::SetTitle(const char* title) const
	{
		if(m_window) glfwSetWindowTitle(m_window, title ? title : "");
	}
	void GlfwWindow::Restore() const
	{
		if(m_window) glfwRestoreWindow(m_window);
	}
	void PollPlatformEvents() { glfwPollEvents(); }
	void WaitPlatformEvents(double seconds) { glfwWaitEventsTimeout(seconds); }

	void GlfwWindow::Resize(unsigned int width, unsigned int height) const
	{
		if(m_window) glfwSetWindowSize(m_window, static_cast<int>(width), static_cast<int>(height));
	}

	WindowSize GlfwWindow::GetSize() const
	{
		WindowSize size;
		if(m_window) glfwGetWindowSize(m_window, &size.width, &size.height);
		return size;
	}

	WindowSize GlfwWindow::GetFramebufferSize() const
	{
		WindowSize size;
		if(m_window) glfwGetFramebufferSize(m_window, &size.width, &size.height);
		return size;
	}

	ContentScale GlfwWindow::GetContentScale() const
	{
		ContentScale scale;
		if(m_window) glfwGetWindowContentScale(m_window, &scale.x, &scale.y);
		return scale;
	}

	bool GlfwWindow::HasFocus() const { return m_window && glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == GLFW_TRUE; }
	bool GlfwWindow::IsMinimized() const { return m_window && glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE; }

	void GlfwWindow::SetCursorMode(CursorMode mode)
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

	void GlfwWindow::ResetCursorDelta()
	{
		double x = 0, y = 0;
		glfwGetCursorPos(m_window, &x, &y);
		ResetCursorPosition(x, y);
	}

	CursorMode GlfwWindow::GetCursorMode() const
	{
		if(!m_window) return CursorMode::Normal;

		switch(glfwGetInputMode(m_window, GLFW_CURSOR))
		{
		case GLFW_CURSOR_DISABLED: return CursorMode::Locked;
		case GLFW_CURSOR_HIDDEN: return CursorMode::Hidden;
		default: return CursorMode::Normal;
		}
	}

	bool GlfwWindow::IsRawMouseMotionSupported() const { return glfwRawMouseMotionSupported() == GLFW_TRUE; }

	bool GlfwWindow::SetRawMouseMotion(bool enabled)
	{
		if(!m_window) return false;
		if(!IsRawMouseMotionSupported()) return !enabled;
		if(IsRawMouseMotionEnabled() == enabled) return true;

		glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, enabled ? GLFW_TRUE : GLFW_FALSE);
		ResetCursorDelta();
		return IsRawMouseMotionEnabled() == enabled;
	}

	bool GlfwWindow::IsRawMouseMotionEnabled() const
	{
		return m_window && glfwGetInputMode(m_window, GLFW_RAW_MOUSE_MOTION) == GLFW_TRUE;
	}
}
