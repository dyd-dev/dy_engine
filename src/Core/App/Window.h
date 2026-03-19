#pragma once

struct GLFWWindow;

class Window
{
public:
	~Window();

	Window(unsigned int width, unsigned int height);
	Window(unsigned int width, unsigned int height, const char *title);

	bool ShouldClose() const;
	void PollEvents() const;

	void* GetHandle() const;

private:
	struct GLFWwindow* m_window;
};