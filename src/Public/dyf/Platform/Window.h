#pragma once
#include "dyf/Platform/Input.h"
struct GLFWwindow;

namespace dyf::Platform
{
	class Window
	{
	public:
		~Window();

		Window(unsigned int width, unsigned int height);
		Window(unsigned int width, unsigned int height, const char *title);

		Window(const Window&) = delete;
		Window& operator=(const Window&) = delete;

		bool IsRunning() const;
		void PollEvents() const;

		void* GetHandle() const;
		// GLFW backend integration only. GetHandle remains the RHI native-window handle.
		GLFWwindow* GetGlfwHandle() const { return m_window; }
		const Input& GetInput() const { return m_input; }

	private:
		struct GLFWwindow* m_window = nullptr;
		Input m_input;
	};
}
