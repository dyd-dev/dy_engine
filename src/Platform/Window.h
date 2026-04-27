#pragma once
struct GLFWwindow;

namespace dy::Platform
{
	class Window
	{
	public:
		~Window();

		Window(unsigned int width, unsigned int height);
		Window(unsigned int width, unsigned int height, const char *title);

		bool IsRunning() const;
		void PollEvents() const;

		void* GetHandle() const;
		struct GLFWwindow* GetGLFWWindow() const { return m_window; }

	private:
		struct GLFWwindow* m_window;
	};
}
