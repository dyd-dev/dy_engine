#pragma once

#include "dyf/Platform/Input.h"
#include <cstdint>
#include <exception>
#include <memory>

namespace dyf::Platform
{
	struct WindowSize
	{
		int width = 0, height = 0;
	};

	struct ContentScale
	{
		float x = 1, y = 1;
	};

	enum class CursorMode { Normal, Hidden, Locked };
	enum class NativeWindowKind { None, Win32, Glfw, Cocoa, Android, UIKit };

	// Platform-owned windows/views must outlive Window and every RHI swapchain using them.
	struct NativeWindowHandle
	{
		NativeWindowKind kind = NativeWindowKind::None;
		void* value = nullptr;
	};

	class WindowBackend;
	struct WindowPlatformAccess;

	// One window/event thread owns this facade. UIKit view work is dispatched to its main thread.
	class Window
	{
	public:
		Window(unsigned int width, unsigned int height);
		Window(unsigned int width, unsigned int height, const char* title);
		explicit Window(NativeWindowHandle borrowedWindow);
		~Window();
		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		bool IsRunning() const;
		void RequestClose() const;
		bool CancelClose() const;
		void SetTitle(const char* title) const;
		void Restore() const;
		// One application-wide event pump; publishes all windows' input snapshots.
		static void PollEvents();
		static void WaitEvents(double timeoutSeconds = 0.1);
		void Resize(unsigned int width, unsigned int height) const;
		[[nodiscard]] WindowSize GetSize() const;
		[[nodiscard]] WindowSize GetFramebufferSize() const;
		[[nodiscard]] ContentScale GetContentScale() const;
		[[nodiscard]] bool HasFocus() const;
		[[nodiscard]] bool IsMinimized() const;
		[[nodiscard]] const Input& GetInput() const { return m_input; }
		void SetCursorMode(CursorMode);
		[[nodiscard]] CursorMode GetCursorMode() const;
		[[nodiscard]] bool IsRawMouseMotionSupported() const;
		bool SetRawMouseMotion(bool enabled);
		[[nodiscard]] bool IsRawMouseMotionEnabled() const;
		[[nodiscard]] NativeWindowHandle GetNativeHandle() const;
		// Changes when a tracked surface is destroyed/recreated, even if its address
		// is reused. Zero means untracked; compare the native handle as well.
		[[nodiscard]] uint64_t GetSurfaceGeneration() const;
		// Compatibility with existing RHI SwapchainDesc::window; see GetNativeHandle for its kind.
		void* GetHandle() const;
	private:
		friend class WindowBackend;
		friend struct WindowPlatformAccess;
		// Native callbacks enqueue input only; errors are rethrown at the event-pump boundary.
		void HandleInputEvent(const InputEvent&) noexcept;

		void Initialize(unsigned int, unsigned int, const char*, NativeWindowHandle);
		static void RethrowCallbackError();
		void ResetCursorPosition(double, double);

		Input m_input;
		std::exception_ptr m_callbackError;
		std::unique_ptr<WindowBackend> m_backend;
	};
}
