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
		void Resize(unsigned int width, unsigned int height) const;

		void* GetHandle() const;

	private:
		struct GLFWwindow* m_window;
	};
}
