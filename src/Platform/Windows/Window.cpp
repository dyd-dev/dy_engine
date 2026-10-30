#include "WindowBackend.h"
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dyf::Platform
{
	namespace
	{
		std::vector<Window*> windows;
	}

	const std::vector<Window*>& RegisteredWindows() noexcept { return windows; }

	Window::Window(unsigned int width, unsigned int height) : Window(width, height, "New Window") {}

	Window::Window(unsigned int width, unsigned int height, const char* title)
	{
		Initialize(width, height, title, {});
	}

	Window::Window(NativeWindowHandle borrowed)
	{
		Initialize(0, 0, nullptr, borrowed);
	}

	void Window::Initialize(unsigned int width, unsigned int height, const char* title, NativeWindowHandle borrowed)
	{
		const auto maximum = static_cast<unsigned>((std::numeric_limits<int>::max)());
		if(borrowed.kind == NativeWindowKind::None && (!width || !height || width > maximum || height > maximum))
		{
			std::fprintf(stderr, "[Platform] dyf: invalid window dimensions.\n");
			return;
		}

		m_backend = CreateWindowBackend(*this, { width, height, title, borrowed });
		if(!m_backend || !m_backend->GetNativeHandle().value)
		{
			m_backend.reset();
			return;
		}

		try
		{
			windows.push_back(this);
			m_input.PublishFrame();
			if(m_callbackError)
				std::rethrow_exception(std::exchange(m_callbackError, {}));
		}
		catch(...)
		{
			windows.erase(std::remove(windows.begin(), windows.end(), this), windows.end());
			m_backend.reset();
			throw;
		}
	}

	Window::~Window()
	{
		windows.erase(std::remove(windows.begin(), windows.end(), this), windows.end());
		m_backend.reset();
	}

	void WindowBackend::ResetCursorPosition(double x, double y)
	{
		owner.ResetCursorPosition(x, y);
	}

	void Window::ResetCursorPosition(double x, double y)
	{
		m_input.m_pending.position = { x, y };
		m_input.m_hasCursorPosition = true;
		m_input.m_pending.delta = {};
		m_input.m_frame.delta = {};
	}

	void Window::HandleInputEvent(const InputEvent& event) noexcept
	{
		try
		{
			m_input.OnEvent(event);
		}
		catch(...)
		{
			if(!m_callbackError)
				m_callbackError = std::current_exception();
		}
	}

	void Window::RethrowCallbackError()
	{
		for(auto* window : windows)
		{
			if(window->m_callbackError)
				std::rethrow_exception(std::exchange(window->m_callbackError, {}));
		}
	}

	void Window::PollEvents()
	{
		if(windows.empty()) return;

		PollPlatformEvents();
		for(auto* window : windows)
		{
			try
			{
				window->m_input.PublishFrame();
			}
			catch(...)
			{
				if(!window->m_callbackError)
					window->m_callbackError = std::current_exception();
			}
		}
		RethrowCallbackError();
	}

	void Window::WaitEvents(double seconds)
	{
		if(!std::isfinite(seconds) || seconds <= 0)
			throw std::invalid_argument("Event wait timeout must be finite and positive.");

		if(!windows.empty()) WaitPlatformEvents(seconds);
		RethrowCallbackError();
	}

	bool Window::IsRunning() const { return m_backend && m_backend->IsRunning(); }
	void Window::RequestClose() const
	{
		if(m_backend) m_backend->RequestClose();
	}
	bool Window::CancelClose() const { return m_backend && m_backend->CancelClose(); }
	void Window::SetTitle(const char* title) const { if(m_backend) m_backend->SetTitle(title); }
	void Window::Restore() const { if(m_backend) m_backend->Restore(); }

	void Window::Resize(unsigned int width, unsigned int height) const
	{
		const auto maximum = static_cast<unsigned>((std::numeric_limits<int>::max)());
		if(!width || !height || width > maximum || height > maximum)
			throw std::invalid_argument("Window dimensions must be positive and fit in int.");

		if(m_backend) m_backend->Resize(width, height);
	}

	WindowSize Window::GetSize() const { return m_backend ? m_backend->GetSize() : WindowSize{}; }
	WindowSize Window::GetFramebufferSize() const { return m_backend ? m_backend->GetFramebufferSize() : WindowSize{}; }
	ContentScale Window::GetContentScale() const { return m_backend ? m_backend->GetContentScale() : ContentScale{}; }
	bool Window::HasFocus() const { return m_backend && m_backend->HasFocus(); }
	bool Window::IsMinimized() const { return !m_backend || m_backend->IsMinimized(); }
	void Window::SetCursorMode(CursorMode mode) { if(m_backend) m_backend->SetCursorMode(mode); }
	CursorMode Window::GetCursorMode() const { return m_backend ? m_backend->GetCursorMode() : CursorMode::Normal; }
	bool Window::IsRawMouseMotionSupported() const { return m_backend && m_backend->IsRawMouseMotionSupported(); }
	bool Window::SetRawMouseMotion(bool value) { return m_backend ? m_backend->SetRawMouseMotion(value) : !value; }
	bool Window::IsRawMouseMotionEnabled() const { return m_backend && m_backend->IsRawMouseMotionEnabled(); }
	NativeWindowHandle Window::GetNativeHandle() const { return m_backend ? m_backend->GetNativeHandle() : NativeWindowHandle{}; }
	uint64_t Window::GetSurfaceGeneration() const { return m_backend ? m_backend->GetSurfaceGeneration() : 0; }
	void* Window::GetHandle() const { return GetNativeHandle().value; }

	GLFWwindow* GetGlfwWindow(const Window& window) noexcept
	{
		const auto* backend = WindowPlatformAccess::Backend(window);
		return backend ? static_cast<GLFWwindow*>(backend->GetGlfwWindow()) : nullptr;
	}
}
