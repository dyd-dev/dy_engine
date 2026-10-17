#pragma once
#include "dyf/Platform/Input.h"
#include <exception>

#if !defined(__ANDROID__)
struct GLFWwindow;
#else
struct ANativeWindow;
struct android_app;
#endif

namespace dyf::Platform
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

#if defined(__ANDROID__)
		static void SetAndroidApp(struct android_app* app);
		static struct android_app* GetAndroidApp();
#endif

		bool IsRunning() const;
		void RequestClose() const;
		// Call once per application frame, even when rendering multiple windows.
		// Input snapshots are published before a native callback failure is rethrown.
		static void PollEvents();
		// Wait without publishing input. Call PollEvents afterwards to read the events.
		// Callback failures are rethrown here instead of crossing the native dispatcher.
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
		[[nodiscard]] bool IsRawMouseMotionSupported() const;
		// Opt-in flag; GLFW delivers raw motion only while CursorMode::Locked.
		// Unsupported enable returns false and leaves ordinary mouse input available.
		bool SetRawMouseMotion(bool enabled);
		[[nodiscard]] bool IsRawMouseMotionEnabled() const;
		// Consume this window's profiler shortcut without consuming input snapshots.
		[[nodiscard]] static bool ConsumeKeyPress(Key key, const void* nativeWindow);

		void* GetHandle() const;
#if !defined(__ANDROID__)
		// GLFW backend integration only. GetHandle remains the RHI native-window handle.
		[[nodiscard]] GLFWwindow* GetGlfwHandle() const { return m_window; }
#endif

		void HandleInputEvent(const InputEvent& event) noexcept;

	private:
		void ResetCursorDelta();
		static void RethrowCallbackError();

#if !defined(__ANDROID__)
		struct GLFWwindow* m_window = nullptr;
		bool m_profilerToggle = false;
#else
		void* m_nativeWindow = nullptr;
		mutable bool m_shouldClose = false;
#endif
		Input m_input;
		std::exception_ptr m_callbackError;
	};
}
