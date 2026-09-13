#pragma once
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

	private:
		struct GLFWwindow* m_window = nullptr;
	};
}
