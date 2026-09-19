#pragma once
#include "Input.h"
struct GLFWwindow;

namespace dy::Platform
{
	struct WindowSize { int width = 0, height = 0; };
	struct ContentScale { float x = 1, y = 1; };
	enum class CursorMode { Normal, Hidden, Locked };

	// Window creation, destruction, event processing and access are main-thread only.
	class Window
	{
	public:
		~Window();

		Window(unsigned int width, unsigned int height);
		Window(unsigned int width, unsigned int height, const char *title);
		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		bool IsRunning() const;
		void RequestClose() const;
		// Call once per application frame, even when rendering multiple windows.
		static void PollEvents();
		// Wait without publishing input. Call PollEvents afterwards to read the events.
		static void WaitEvents(double timeoutSeconds = 0.1);
		void Resize(unsigned int width, unsigned int height) const;
		[[nodiscard]] WindowSize GetSize() const;
		[[nodiscard]] WindowSize GetFramebufferSize() const;
		[[nodiscard]] ContentScale GetContentScale() const;
		[[nodiscard]] bool HasFocus() const;
		[[nodiscard]] bool IsMinimized() const;
		[[nodiscard]] const Input& GetInput() const { return m_input; }
		void SetCursorMode(CursorMode mode);
		[[nodiscard]] CursorMode GetCursorMode() const;
		// Legacy application-wide profiler shortcut. Does not consume GetInput() queries.
		[[nodiscard]] static bool ConsumeKeyPress(Key key);

		void* GetHandle() const;

	private:
		struct GLFWwindow* m_window = nullptr;
		Input m_input;
	};
}
